// FlexVoiceConfig.cpp -- the FlexVoice configuration utility.
//
// Accessibility rules this file is built around, each of them learned the hard
// way on a real screen reader:
//
//   * Every control is created in code, in exactly the order it should be
//     reached by Tab. Z-order is tab order, and a dialog template with
//     fourteen label/edit/spin triples is far too easy to get subtly wrong.
//   * Numeric values are an edit box with an up-down buddy, never a trackbar.
//     MSAA reports a trackbar's position as a percentage of its range, so a
//     0..9 slider announces 5 as "55". A spin buddy announces the literal
//     number, which is what a percentage control has to do.
//   * Every control gets an explicit MSAA name through IAccPropServices,
//     spelling the range out in words, rather than relying on a screen reader
//     picking up the nearest preceding static.
//   * Access keys are unique across the whole dialog; the table below is
//     checked at startup in a debug build.
//   * Changes save the moment they are made, so there is no OK/Cancel model to
//     explain and closing the window can never lose anything.

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
#include "engine_client.h"
#include "flexvoice_log.h"
#include "settings.h"
#include "voice_data.hpp"

#pragma comment(linker, "/manifestdependency:\"type='win32' "                    \
                        "name='Microsoft.Windows.Common-Controls' "              \
                        "version='6.0.0.0' processorArchitecture='*' "           \
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace FlexVoice;

namespace {

HINSTANCE g_instance = nullptr;
HWND      g_dialog = nullptr;
Settings  g_settings;

// Dialogs receive control notifications before WM_INITDIALOG has finished:
// creating an edit box with a spin buddy fires EN_CHANGE straight away. This
// starts true so those notifications cannot save garbage over real settings.
bool g_loading = true;

std::vector<unsigned char> g_previewWav;   // must outlive PlaySound's SND_ASYNC

HWND g_paramEdit[FVP_COUNT] = {};
HWND g_paramSpin[FVP_COUNT] = {};
HWND g_baseVoice = nullptr;
HWND g_language = nullptr;
HWND g_sampleRate = nullptr;
HWND g_previewText = nullptr;
HWND g_debugLog = nullptr;
HWND g_status = nullptr;

const int kSampleRates[] = { 8000, 11025, 16000, 22050, 32000, 44100 };
const int kSampleRateCount = 6;

// ---------------------------------------------------------------------------
// MSAA naming
// ---------------------------------------------------------------------------

IAccPropServices* g_accProps = nullptr;

void set_acc_name(HWND ctl, const wchar_t* name)
{
    if (!g_accProps || !ctl || !name) return;
    g_accProps->SetHwndPropStr(ctl, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, name);
}

// ---------------------------------------------------------------------------
// Control creation
// ---------------------------------------------------------------------------

// Dialog units to pixels, so the code can lay out in the same units the
// template declares its size in.
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
    const voices::ParamDef& d = voices::param(static_cast<FlexVoiceParam>(index));

    make_label(d.label, x, y, labelW, IDC_PARAM_LABEL_BASE + index);

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
    set_acc_name(edit, d.accName);
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
    const voices::ParamDef& d = voices::param(static_cast<FlexVoiceParam>(index));
    const double v = voices::percent_to_value(d, g_settings.percent[index]);
    wchar_t buf[256];
    if (d.isInt) {
        swprintf_s(buf, L"%s: %d%%  (%.0f %s)", d.accName, g_settings.percent[index],
                   v, d.unit && *d.unit ? d.unit : L"");
    } else {
        swprintf_s(buf, L"%s: %d%%  (engine value %.3f)", d.accName,
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

    SendMessageW(g_baseVoice, CB_SETCURSEL, g_settings.baseVoice, 0);
    SendMessageW(g_language, CB_SETCURSEL, g_settings.languageIndex, 0);
    for (int i = 0; i < kSampleRateCount; ++i) {
        if (kSampleRates[i] == g_settings.sampleRate) {
            SendMessageW(g_sampleRate, CB_SETCURSEL, i, 0);
            break;
        }
    }
    for (int i = 0; i < FVP_COUNT; ++i) {
        SendMessageW(g_paramSpin[i], UDM_SETPOS32, 0, g_settings.percent[i]);
    }
    SendMessageW(g_debugLog, BM_SETCHECK,
                 g_settings.debugLogging ? BST_CHECKED : BST_UNCHECKED, 0);

    g_loading = false;
}

// Refresh only the numeric fields, never the whole dialog: rebuilding it would
// move screen-reader focus off whatever the user is on.
void refresh_params()
{
    const bool wasLoading = g_loading;
    g_loading = true;
    for (int i = 0; i < FVP_COUNT; ++i) {
        SendMessageW(g_paramSpin[i], UDM_SETPOS32, 0, g_settings.percent[i]);
    }
    g_loading = wasLoading;
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

    wchar_t text[512] = {};
    GetWindowTextW(g_previewText, text, 511);
    if (!text[0]) {
        wcscpy_s(text, L"The quick brown fox jumps over the lazy dog.");
    }

    const int li = g_settings.languageIndex;
    const unsigned cp = voices::kLanguages[li].codepage;
    const int n = WideCharToMultiByte(cp, 0, text, static_cast<int>(wcslen(text)),
                                      nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(cp, 0, text, static_cast<int>(wcslen(text)),
                        &bytes[0], n, nullptr, nullptr);

    SpeakParams params;
    // Preview always uses the Custom voice, which is what this dialog edits.
    params.voiceIndex = (g_settings.baseVoice == 1) ? 3u : 0u;
    params.language = voices::kLanguages[li].id;
    params.sampleRate = static_cast<uint32_t>(g_settings.sampleRate);
    for (int i = 0; i < FVP_COUNT; ++i) {
        const auto p = static_cast<FlexVoiceParam>(i);
        params.set(p, voices::percent_to_value(voices::param(p), g_settings.percent[i]));
    }

    std::vector<SpeakSegment> segments(1);
    segments[0].kind = FV_SEG_TEXT;
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
        const std::wstring msg = L"Preview failed: " +
            std::wstring(error.begin(), error.end());
        set_status(msg.c_str());
        FV_LOG("config: preview failed: %s", error.c_str());
        return;
    }

    // Hand PlaySound a WAV image in memory. g_previewWav is file-scope on
    // purpose: SND_ASYNC keeps reading it after this function returns.
    const uint32_t dataLen = static_cast<uint32_t>(pcm.size());
    const uint32_t rate = static_cast<uint32_t>(g_settings.sampleRate);
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
    const bool keepLogging = g_settings.debugLogging;
    g_settings = SettingsStore::defaults();
    g_settings.debugLogging = keepLogging;
    load_dialog();
    save_settings();
    set_status(L"All speech parameters restored to their defaults.");
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

void build_controls()
{
    // Column geometry, in dialog units.
    const int kLabelW = 74;
    const int kEditW = 34;
    const int kCol1 = 7;
    const int kCol2 = 200;
    const int kRowH = 17;

    // --- top row: what voice, what language, what sample rate --------------
    make_label(L"&Base voice:", kCol1, 8, 62, -1);
    g_baseVoice = make_combo(kCol1 + 64, 8, 108, IDC_BASEVOICE,
                             L"Base voice, the diphone database the custom voice starts from");
    make_label(L"&Language:", kCol2, 8, 52, -1);
    g_language = make_combo(kCol2 + 54, 8, 108, IDC_LANGUAGE, L"Language");

    make_label(L"Sa&mple rate:", kCol1, 26, 62, -1);
    g_sampleRate = make_combo(kCol1 + 64, 26, 108, IDC_SAMPLERATE,
                              L"Sample rate in hertz");

    make_label(L"Speech parameters. Each is a percentage: 0 is the minimum the "
               L"engine supports and 100 the maximum.",
               kCol1, 46, 370, -1);

    // --- fourteen parameter rows, seven per column -------------------------
    const int kPerColumn = (FVP_COUNT + 1) / 2;
    for (int i = 0; i < FVP_COUNT; ++i) {
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
         kCol1, yBottom + 20, 60, 14, IDC_PREVIEW);
    make(L"BUTTON", L"Stop pla&ying", BS_PUSHBUTTON | WS_TABSTOP, 0,
         kCol1 + 66, yBottom + 20, 50, 14, IDC_STOP);
    make(L"BUTTON", L"Reset to defa&ults", BS_PUSHBUTTON | WS_TABSTOP, 0,
         kCol1 + 122, yBottom + 20, 80, 14, IDC_DEFAULTS);
    g_debugLog = make(L"BUTTON", L"&Write a diagnostic log",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                      kCol1 + 208, yBottom + 21, 100, 12, IDC_DEBUGLOG);
    set_acc_name(g_debugLog, L"Write a diagnostic log file");

    make(L"BUTTON", L"Clos&e", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
         kCol1 + 314, yBottom + 20, 50, 14, IDOK);

    g_status = make(L"STATIC", L"Ready.", SS_LEFT | SS_ENDELLIPSIS, 0,
                    kCol1, yBottom + 40, 370, 9, IDC_STATUS);
    set_acc_name(g_status, L"Status");
}

void populate_lists()
{
    for (int i = 0; i < voices::kBaseVoiceCount; ++i) {
        SendMessageW(g_baseVoice, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(voices::kBaseVoices[i].name));
    }
    for (int i = 0; i < voices::kLanguageCount; ++i) {
        SendMessageW(g_language, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(voices::kLanguages[i].name));
    }
    for (int i = 0; i < kSampleRateCount; ++i) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d Hz", kSampleRates[i]);
        SendMessageW(g_sampleRate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
}

void on_param_changed(int index)
{
    if (g_loading) return;
    const int pos = static_cast<int>(SendMessageW(g_paramSpin[index], UDM_GETPOS32, 0, 0));
    g_settings.percent[index] = voices::clamp_percent(pos);
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
        FlexVoicePong pong = {};
        std::string error;
        if (sharedClient().ping(pong, error)) {
            wchar_t buf[160];
            swprintf_s(buf, L"Ready. FlexVoice engine host is running, %u voices installed.",
                       pong.voiceCount);
            set_status(buf);
        } else {
            set_status(L"The FlexVoice engine host is not responding. "
                       L"Preview will not work; check the diagnostic log.");
        }
        return TRUE;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);

        if (id >= IDC_PARAM_EDIT_BASE && id < IDC_PARAM_EDIT_BASE + FVP_COUNT) {
            if (code == EN_CHANGE) on_param_changed(id - IDC_PARAM_EDIT_BASE);
            return TRUE;
        }

        switch (id) {
        case IDC_BASEVOICE:
            if (code == CBN_SELCHANGE && !g_loading) {
                g_settings.baseVoice =
                    static_cast<int>(SendMessageW(g_baseVoice, CB_GETCURSEL, 0, 0));
                save_settings();
                set_status(L"Base voice changed. The custom voice now uses this "
                           L"diphone database.");
            }
            return TRUE;
        case IDC_LANGUAGE:
            if (code == CBN_SELCHANGE && !g_loading) {
                g_settings.languageIndex =
                    static_cast<int>(SendMessageW(g_language, CB_GETCURSEL, 0, 0));
                save_settings();
                set_status(L"Language changed.");
            }
            return TRUE;
        case IDC_SAMPLERATE:
            if (code == CBN_SELCHANGE && !g_loading) {
                const int sel = static_cast<int>(SendMessageW(g_sampleRate, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < kSampleRateCount) {
                    g_settings.sampleRate = kSampleRates[sel];
                    save_settings();
                    set_status(L"Sample rate changed. Restart the speaking "
                               L"application for it to take effect.");
                }
            }
            return TRUE;
        case IDC_DEBUGLOG:
            if (!g_loading) {
                g_settings.debugLogging =
                    SendMessageW(g_debugLog, BM_GETCHECK, 0, 0) == BST_CHECKED;
                log::enabled() = g_settings.debugLogging;
                save_settings();
                const std::wstring dir = SettingsStore::log_dir();
                std::wstring msg = g_settings.debugLogging
                    ? L"Diagnostic logging on. Logs are written to " + dir
                    : std::wstring(L"Diagnostic logging off.");
                set_status(msg.c_str());
            }
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

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CONFIG), nullptr, dialog_proc, 0);

    CoUninitialize();
    return 0;
}
