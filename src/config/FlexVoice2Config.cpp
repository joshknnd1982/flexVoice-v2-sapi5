// FlexVoice2Config.cpp -- the FlexVoice 2 configuration utility.
//
// Accessibility rules this file is built around, each of them learned the hard
// way on a real screen reader:
//
//   * Every control is created in code, in exactly the order it should be
//     reached by Tab. Z-order is tab order, and a dialog template with thirteen
//     label/edit/spin triples is far too easy to get subtly wrong.
//   * Numeric values are an edit box with an up-down buddy, never a trackbar.
//     MSAA reports a trackbar's position as a percentage of its range, so a
//     0..9 slider announces 5 as "55". A spin buddy announces the literal
//     number, which is what a percentage control has to do.
//   * Every control gets an explicit MSAA name through IAccPropServices,
//     spelling out what it does, rather than relying on a screen reader picking
//     up the nearest preceding static.
//   * Access keys are unique across the whole dialog.
//   * Changes save the moment they are made, so there is no OK/Cancel model to
//     explain and closing the window can never lose anything.
//   * Only the parameters the engine actually responds to get controls. tilt,
//     singingPitchRate and speedWPM are real keys in Mindmaker's voice files
//     and are preserved when one is read, but the engine ignores them --
//     swept from -5 to 100 they give bit-identical audio -- and a slider that
//     provably does nothing is worse than no slider, especially read aloud.

#ifdef _MSC_VER
#  pragma warning(disable : 4996)
#endif

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <initguid.h>     // before oleacc, so no extra import library is needed
#include <oleacc.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "resource.h"
#include "../fv2_client.h"
#include "../fv2_log.h"
#include "../fv2_settings.h"
#include "../fv2_speaker.hpp"
#include "../fv2_voices.hpp"

#pragma comment(linker, "/manifestdependency:\"type='win32' "                    \
                        "name='Microsoft.Windows.Common-Controls' "              \
                        "version='6.0.0.0' processorArchitecture='*' "           \
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace fv2;

namespace {

HINSTANCE g_instance = nullptr;
HWND      g_dialog = nullptr;
Settings  g_settings;

// Dialogs receive control notifications before WM_INITDIALOG has finished:
// creating an edit box with a spin buddy fires EN_CHANGE straight away. This
// starts true so those notifications cannot save garbage over real settings.
bool g_loading = true;

std::vector<unsigned char> g_previewWav;   // must outlive PlaySound's SND_ASYNC

HWND g_paramEdit[P_COUNT] = {};
HWND g_paramSpin[P_COUNT] = {};
HWND g_baseVoice = nullptr;
HWND g_language = nullptr;
HWND g_sampleRate = nullptr;
HWND g_previewVoice = nullptr;
HWND g_previewText = nullptr;
HWND g_debugLog = nullptr;
HWND g_verboseLog = nullptr;
HWND g_status = nullptr;

const int kSampleRates[] = { 8000, 11025, 16000, 22050, 32000, 44100 };
const int kSampleRateCount = 6;

std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

int clamp_percent(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

// ---------------------------------------------------------------------------
// MSAA naming
// ---------------------------------------------------------------------------

IAccPropServices* g_accProps = nullptr;

void set_acc_name(HWND ctl, const wchar_t* name)
{
    if (!g_accProps || !ctl || !name) return;
    g_accProps->SetHwndPropStr(ctl, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, name);
}

void set_acc_description(HWND ctl, const wchar_t* text)
{
    if (!g_accProps || !ctl || !text) return;
    g_accProps->SetHwndPropStr(ctl, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_DESCRIPTION, text);
}

// ---------------------------------------------------------------------------
// Control creation
// ---------------------------------------------------------------------------

int g_baseX = 6, g_baseY = 13;

int dux(int x) { return MulDiv(x, g_baseX, 4); }
int duy(int y) { return MulDiv(y, g_baseY, 8); }

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle,
          int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(exStyle, cls, text, style | WS_CHILD | WS_VISIBLE,
                             dux(x), duy(y), dux(w), duy(h), g_dialog,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             g_instance, nullptr);
    if (c) {
        const LRESULT font = SendMessageW(g_dialog, WM_GETFONT, 0, 0);
        SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    }
    return c;
}

HWND make_label(const wchar_t* text, int x, int y, int w, int id)
{
    return make(L"STATIC", text, SS_LEFT | WS_GROUP, 0, x, y + 2, w, 9, id);
}

HWND make_combo(int x, int y, int w, int id, const wchar_t* accName)
{
    HWND c = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                  WS_EX_CLIENTEDGE, x, y, w, 100, id);
    set_acc_name(c, accName);
    return c;
}

// An edit box restricted to digits plus an up-down buddy. UDS_AUTOBUDDY takes
// the previous control in z-order, which is why the edit must be created
// immediately before the spin.
void make_spin_edit(int index, int x, int y, int labelW, int editW)
{
    const ParamInfo& d = kParams[index];

    const std::wstring label = widen(std::string("&") + d.label + ":");
    make_label(label.c_str(), x, y, labelW, IDC_PARAM_LABEL_BASE + index);

    HWND edit = make(L"EDIT", L"", ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP,
                     WS_EX_CLIENTEDGE, x + labelW, y, editW, 13,
                     IDC_PARAM_EDIT_BASE + index);
    HWND spin = make(UPDOWN_CLASSW, L"",
                     UDS_SETBUDDYINT | UDS_AUTOBUDDY | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                         UDS_NOTHOUSANDS,
                     0, 0, 0, 0, 0, IDC_PARAM_SPIN_BASE + index);

    SendMessageW(spin, UDM_SETRANGE32, 0, 100);

    g_paramEdit[index] = edit;
    g_paramSpin[index] = spin;

    // Name and description are separate on purpose: a screen reader reads the
    // name every time focus lands, and the description on request. The name
    // says what the control is and that it is a percentage; the description
    // says what the parameter does.
    const std::wstring accName = widen(std::string(d.label) + ", percent, 0 is minimum and 100 is maximum");
    set_acc_name(edit, accName.c_str());
    set_acc_description(edit, widen(d.help).c_str());
}

// ---------------------------------------------------------------------------
// Settings <-> dialog
// ---------------------------------------------------------------------------

void set_status(const wchar_t* text)
{
    if (g_status) SetWindowTextW(g_status, text);
}

// Show what a percentage actually means, so the user is not guessing.
void update_status_for(int index)
{
    const ParamInfo& d = kParams[index];
    const double v = percentToValue((ParamId)index, g_settings.percent[index]);
    wchar_t buf[256];
    const std::wstring label = widen(d.label);
    if (d.type == PT_INT) {
        swprintf_s(buf, L"%s: %d%%, engine value %.0f", label.c_str(),
                   g_settings.percent[index], v);
    } else {
        swprintf_s(buf, L"%s: %d%%, engine value %.3f", label.c_str(),
                   g_settings.percent[index], v);
    }
    set_status(buf);
}

void save_settings()
{
    if (g_loading) return;
    SettingsStore::save(g_settings);
}

void load_dialog()
{
    g_loading = true;

    int baseSel = 0;
    for (size_t i = 0; i < builtinVoices().size(); ++i) {
        if (g_settings.baseVoice == builtinVoices()[i].name) baseSel = (int)i;
    }
    SendMessageW(g_baseVoice, CB_SETCURSEL, baseSel, 0);
    SendMessageW(g_language, CB_SETCURSEL, 0, 0);
    for (int i = 0; i < kSampleRateCount; ++i) {
        if (kSampleRates[i] == g_settings.sampleRate) {
            SendMessageW(g_sampleRate, CB_SETCURSEL, i, 0);
            break;
        }
    }
    for (int i = 0; i < P_ACTIVE_COUNT; ++i) {
        SendMessageW(g_paramSpin[i], UDM_SETPOS32, 0, g_settings.percent[i]);
    }
    SendMessageW(g_debugLog, BM_SETCHECK,
                 g_settings.debugLogging ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_verboseLog, BM_SETCHECK,
                 g_settings.verboseLogging ? BST_CHECKED : BST_UNCHECKED, 0);

    g_loading = false;
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

void stop_preview()
{
    PlaySoundW(nullptr, nullptr, 0);
}

void do_preview()
{
    stop_preview();

    wchar_t text[512] = {0};
    GetWindowTextW(g_previewText, text, 511);
    if (!text[0]) {
        wcscpy_s(text, L"The quick brown fox jumps over the lazy dog.");
    }

    // The engine takes a single-byte string; CP1252 with best-fit is the same
    // conversion the SAPI path uses, so the preview sounds like the voice will.
    const int n = WideCharToMultiByte(1252, 0, text, (int)wcslen(text),
                                      nullptr, 0, nullptr, nullptr);
    std::string bytes((size_t)n, '\0');
    WideCharToMultiByte(1252, 0, text, (int)wcslen(text), &bytes[0], n, nullptr, nullptr);

    const int previewSel = (int)SendMessageW(g_previewVoice, CB_GETCURSEL, 0, 0);

    SpeakParams params;
    if (previewSel <= 0) {
        // The Custom Voice: everything from this dialog.
        params.baseVoice = g_settings.baseVoice;
        for (int i = 0; i < P_ACTIVE_COUNT; ++i) {
            params.set((ParamId)i, percentToValue((ParamId)i, g_settings.percent[i]));
        }
    } else {
        // One of Mindmaker's own, exactly as the SAPI voice of that name would
        // sound: its own file, its own measured level trim, no overrides.
        params.baseVoice = builtinVoices()[previewSel - 1].name;
    }

    std::vector<SpeakSegment> segments(1);
    segments[0].kind = FV2_SEG_TEXT;
    segments[0].text = bytes;

    std::vector<unsigned char> pcm;
    SpeakSink sink;
    sink.audio = [&](const unsigned char* data, uint32_t size) -> bool {
        pcm.insert(pcm.end(), data, data + size);
        return pcm.size() < 40u * 1024 * 1024;
    };

    set_status(L"Rendering preview...");
    std::string error;
    if (!sharedClient().speak(params, segments, sink, error) || pcm.empty()) {
        const std::wstring msg = L"Preview failed: " + widen(error);
        set_status(msg.c_str());
        FV2_LOG("config: preview failed: %s", error.c_str());
        return;
    }

    // Hand PlaySound a WAV image in memory. g_previewWav is file-scope on
    // purpose: SND_ASYNC keeps reading it after this function returns.
    const uint32_t dataLen = (uint32_t)pcm.size();
    const uint32_t rate = (uint32_t)g_settings.sampleRate;
    g_previewWav.assign(44, 0);
    auto put32 = [&](size_t off, uint32_t v) { memcpy(&g_previewWav[off], &v, 4); };
    auto put16 = [&](size_t off, uint16_t v) { memcpy(&g_previewWav[off], &v, 2); };
    memcpy(&g_previewWav[0], "RIFF", 4);
    put32(4, 36 + dataLen);
    memcpy(&g_previewWav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);            // PCM
    put16(22, 1);            // mono
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&g_previewWav[36], "data", 4);
    put32(40, dataLen);
    g_previewWav.insert(g_previewWav.end(), pcm.begin(), pcm.end());

    wchar_t status[128];
    swprintf_s(status, L"Playing %.1f seconds of preview audio.",
               dataLen / (rate * 2.0));
    set_status(status);

    PlaySoundW(reinterpret_cast<LPCWSTR>(g_previewWav.data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void restore_defaults()
{
    const bool keepDebug = g_settings.debugLogging;
    const bool keepVerbose = g_settings.verboseLogging;
    g_settings = SettingsStore::defaults();
    g_settings.debugLogging = keepDebug;
    g_settings.verboseLogging = keepVerbose;
    load_dialog();
    g_loading = false;
    save_settings();
    set_status(L"All speech parameters restored to the values Mindmaker shipped.");
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

void build_controls()
{
    const int kLabelW = 76;
    const int kEditW = 32;
    const int kCol1 = 7;
    const int kCol2 = 200;
    const int kRowH = 17;

    // --- top: which voice, which language, what sample rate ----------------
    make_label(L"&Base voice:", kCol1, 8, 62, -1);
    g_baseVoice = make_combo(kCol1 + 64, 8, 108, IDC_BASEVOICE,
                             L"Base voice, the voice the Custom Voice starts from");
    set_acc_description(g_baseVoice,
        L"The Custom Voice takes this voice's character and then applies the "
        L"parameters below. All five share one recorded database.");

    make_label(L"&Language:", kCol2, 8, 52, -1);
    g_language = make_combo(kCol2 + 54, 8, 108, IDC_LANGUAGE, L"Language");
    set_acc_description(g_language,
        L"FlexVoice 2.0 shipped English data only. No other language pack for "
        L"this engine is known to survive.");

    make_label(L"Sa&mple rate:", kCol1, 26, 62, -1);
    g_sampleRate = make_combo(kCol1 + 64, 26, 108, IDC_SAMPLERATE,
                              L"Sample rate in hertz");

    make_label(L"&Preview voice:", kCol2, 26, 52, -1);
    g_previewVoice = make_combo(kCol2 + 54, 26, 108, IDC_PREVIEWVOICE,
                                L"Which voice the Speak it button uses");

    make_label(L"Speech parameters. Each is a whole percentage: 0 is the minimum "
               L"the engine supports and 100 the maximum.",
               kCol1, 46, 370, -1);

    // --- thirteen parameter rows, seven and six ---------------------------
    const int kPerColumn = (P_ACTIVE_COUNT + 1) / 2;
    for (int i = 0; i < P_ACTIVE_COUNT; ++i) {
        const bool second = i >= kPerColumn;
        const int row = second ? i - kPerColumn : i;
        make_spin_edit(i, second ? kCol2 : kCol1, 62 + row * kRowH, kLabelW, kEditW);
    }

    const int yBottom = 62 + kPerColumn * kRowH + 6;

    // --- preview -----------------------------------------------------------
    make_label(L"Preview te&xt:", kCol1, yBottom, 62, -1);
    g_previewText = make(L"EDIT", L"The quick brown fox jumps over the lazy dog.",
                         ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                         kCol1 + 64, yBottom, 304, 13, IDC_PREVIEWTEXT);
    set_acc_name(g_previewText, L"Preview text");

    make(L"BUTTON", L"Spea&k it", BS_PUSHBUTTON | WS_TABSTOP, 0,
         kCol1, yBottom + 20, 52, 14, IDC_PREVIEW);
    make(L"BUTTON", L"S&top", BS_PUSHBUTTON | WS_TABSTOP, 0,
         kCol1 + 56, yBottom + 20, 38, 14, IDC_STOP);
    make(L"BUTTON", L"Reset to defa&ults", BS_PUSHBUTTON | WS_TABSTOP, 0,
         kCol1 + 98, yBottom + 20, 72, 14, IDC_DEFAULTS);
    make(L"BUTTON", L"Open lo&g folder", BS_PUSHBUTTON | WS_TABSTOP, 0,
         kCol1 + 174, yBottom + 20, 66, 14, IDC_OPENLOGS);

    g_debugLog = make(L"BUTTON", L"&Write a diagnostic log",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                      kCol1, yBottom + 38, 110, 12, IDC_DEBUGLOG);
    set_acc_name(g_debugLog, L"Write a diagnostic log file");

    g_verboseLog = make(L"BUTTON", L"Log e&very utterance",
                        BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                        kCol1 + 116, yBottom + 38, 110, 12, IDC_VERBOSELOG);
    set_acc_name(g_verboseLog, L"Log every utterance in detail");
    set_acc_description(g_verboseLog,
        L"Slower. Turn this on only while diagnosing a problem, because it "
        L"writes to the log for every phrase spoken.");

    make(L"BUTTON", L"Clos&e", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
         kCol1 + 314, yBottom + 20, 50, 14, IDOK);

    g_status = make(L"STATIC", L"Ready.", SS_LEFT | SS_ENDELLIPSIS, 0,
                    kCol1, yBottom + 56, 370, 9, IDC_STATUS);
    set_acc_name(g_status, L"Status");
}

void populate_lists()
{
    for (size_t i = 0; i < builtinVoices().size(); ++i) {
        const std::wstring n = widen(builtinVoices()[i].name);
        SendMessageW(g_baseVoice, CB_ADDSTRING, 0, (LPARAM)n.c_str());
    }

    // One entry, because one is all that exists. Saying so in a list the user
    // can open is more honest than hiding the control.
    SendMessageW(g_language, CB_ADDSTRING, 0, (LPARAM)L"English (United States)");

    for (int i = 0; i < kSampleRateCount; ++i) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d Hz", kSampleRates[i]);
        SendMessageW(g_sampleRate, CB_ADDSTRING, 0, (LPARAM)buf);
    }

    SendMessageW(g_previewVoice, CB_ADDSTRING, 0, (LPARAM)L"Custom Voice (this dialog)");
    for (size_t i = 0; i < builtinVoices().size(); ++i) {
        const std::wstring n = widen(builtinVoices()[i].name);
        SendMessageW(g_previewVoice, CB_ADDSTRING, 0, (LPARAM)n.c_str());
    }
    SendMessageW(g_previewVoice, CB_SETCURSEL, 0, 0);
}

void on_param_changed(int index)
{
    if (g_loading) return;
    const int pos = (int)SendMessageW(g_paramSpin[index], UDM_GETPOS32, 0, 0);
    g_settings.percent[index] = clamp_percent(pos);
    save_settings();
    update_status_for(index);
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        g_dialog = dlg;

        const LONG units = GetDialogBaseUnits();
        g_baseX = LOWORD(units);
        g_baseY = HIWORD(units);

        CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IAccPropServices, reinterpret_cast<void**>(&g_accProps));

        build_controls();
        populate_lists();
        g_settings = SettingsStore::load();
        load_dialog();

        // Say up front whether the engine is reachable, rather than making the
        // first Preview the thing that discovers it is not.
        Fv2Pong pong = {};
        std::string error;
        if (sharedClient().ping(pong, error)) {
            wchar_t buf[200];
            swprintf_s(buf, L"Ready. The FlexVoice 2 engine host is running with "
                            L"%u voices installed.", pong.voiceCount);
            set_status(buf);
        } else {
            set_status(L"The FlexVoice 2 engine host is not responding. Preview "
                       L"will not work; see the log folder.");
        }
        return TRUE;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);

        if (id >= IDC_PARAM_EDIT_BASE && id < IDC_PARAM_EDIT_BASE + P_ACTIVE_COUNT) {
            if (code == EN_CHANGE) on_param_changed(id - IDC_PARAM_EDIT_BASE);
            return TRUE;
        }

        switch (id) {
        case IDC_BASEVOICE:
            if (code == CBN_SELCHANGE && !g_loading) {
                const int sel = (int)SendMessageW(g_baseVoice, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)builtinVoices().size()) {
                    g_settings.baseVoice = builtinVoices()[sel].name;
                    save_settings();
                    const std::wstring msg =
                        L"Base voice changed to " + widen(g_settings.baseVoice) +
                        L". The Custom Voice now starts from it.";
                    set_status(msg.c_str());
                }
            }
            return TRUE;
        case IDC_LANGUAGE:
            if (code == CBN_SELCHANGE && !g_loading) {
                set_status(L"FlexVoice 2.0 shipped English data only.");
            }
            return TRUE;
        case IDC_SAMPLERATE:
            if (code == CBN_SELCHANGE && !g_loading) {
                const int sel = (int)SendMessageW(g_sampleRate, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < kSampleRateCount) {
                    g_settings.sampleRate = kSampleRates[sel];
                    save_settings();
                    set_status(L"Sample rate changed. Restart the speaking "
                               L"application for it to take effect.");
                }
            }
            return TRUE;
        case IDC_PREVIEWVOICE:
            if (code == CBN_SELCHANGE && !g_loading) {
                set_status(L"Preview voice changed.");
            }
            return TRUE;
        case IDC_DEBUGLOG:
            if (!g_loading) {
                g_settings.debugLogging =
                    SendMessageW(g_debugLog, BM_GETCHECK, 0, 0) == BST_CHECKED;
                log::enabled() = g_settings.debugLogging;
                save_settings();
                const std::wstring dir = log::log_dir();
                const std::wstring msg = g_settings.debugLogging
                    ? L"Diagnostic logging on. Logs are written to " + dir
                    : std::wstring(L"Diagnostic logging off.");
                set_status(msg.c_str());
            }
            return TRUE;
        case IDC_VERBOSELOG:
            if (!g_loading) {
                g_settings.verboseLogging =
                    SendMessageW(g_verboseLog, BM_GETCHECK, 0, 0) == BST_CHECKED;
                log::verbose() = g_settings.verboseLogging;
                save_settings();
                set_status(g_settings.verboseLogging
                    ? L"Logging every utterance. This is slower; turn it off when done."
                    : L"Per-utterance logging off.");
            }
            return TRUE;
        case IDC_OPENLOGS:
            ShellExecuteW(dlg, L"open", log::log_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            set_status(L"Opened the log folder.");
            return TRUE;
        case IDC_PREVIEW:
            do_preview();
            return TRUE;
        case IDC_STOP:
            stop_preview();
            set_status(L"Preview stopped.");
            return TRUE;
        case IDC_DEFAULTS:
            restore_defaults();
            return TRUE;
        case IDOK:
        case IDCANCEL:
            stop_preview();
            save_settings();
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
        }
        return FALSE;
    }

    case WM_CLOSE:
        stop_preview();
        save_settings();
        EndDialog(dlg, 0);
        return TRUE;

    case WM_DESTROY:
        if (g_accProps) { g_accProps->Release(); g_accProps = nullptr; }
        return FALSE;

    default:
        break;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    g_instance = hInstance;
    log::component() = L"config";
    log::enabled() = SettingsStore::current().debugLogging;
    log::verbose() = SettingsStore::current().verboseLogging;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CONFIG), nullptr, dialog_proc, 0);

    CoUninitialize();
    return 0;
}
