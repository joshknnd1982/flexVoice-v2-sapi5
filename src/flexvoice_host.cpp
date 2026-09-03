// flexvoice_host.cpp -- the 32-bit process that owns the FlexVoice engine.
//
// Both SAPI DLLs and the configuration utility are clients of this one host
// over \\.\pipe\FlexVoiceTTS. Running the engine out of process is not just
// about the 64-bit bridge: the engine calls abort() on some malformed input,
// and an abort() inside NVDA's process would take the screen reader down.
// Here it costs one host restart and one lost utterance.
//
//   flexvoice_host.exe                 serve until told to shut down
//   flexvoice_host.exe --shutdown      ask a running host to exit (installer)
//   flexvoice_host.exe --selftest DIR  render one utterance and report

#ifdef _MSC_VER
#  pragma warning(disable : 4996)
#endif

#include <windows.h>
#include <crtdbg.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "flexvoice_protocol.h"
#include "flexvoice_log.h"
#include "fv_engine.hpp"
#include "settings.h"
#include "voice_data.hpp"

using namespace FlexVoice;

namespace {

HANDLE g_serverMutex = nullptr;
volatile LONG g_shuttingDown = 0;

// The engine can abort() and its CRT would put a modal "abnormal program
// termination" box on the desktop. A screen reader user cannot see it and it
// blocks the process forever. Die quietly instead; the client relaunches us.
void silence_crt_popups()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring module_dir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

// Where the language data lives. In an installed tree it is <app>\engine,
// beside this executable. FLEXVOICE_ENGINE_ROOT overrides that, which is both
// the development path and an escape hatch for an unusual install.
std::string engine_root()
{
    {
        wchar_t env[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"FLEXVOICE_ENGINE_ROOT", env, MAX_PATH)) {
            const DWORD attr = GetFileAttributesW(env);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return narrow(env);
            }
            FV_LOG("host: FLEXVOICE_ENGINE_ROOT is set but not a directory");
        }
    }

    const std::wstring dir = module_dir();
    // Installed layout first, then the places the build tree puts it, walking
    // up far enough to reach the repository root from build_x86\bin\Release.
    static const wchar_t* kCandidates[] = {
        L"\\engine",
        L"\\..\\engine",
        L"\\..\\..\\engine",
        L"\\bin\\fv",
        L"\\..\\bin\\fv",
        L"\\..\\..\\bin\\fv",
        L"\\..\\..\\..\\bin\\fv",
        L"\\..\\..\\..\\..\\bin\\fv",
    };
    for (const wchar_t* rel : kCandidates) {
        std::wstring p = dir + rel;
        wchar_t full[MAX_PATH] = {};
        if (!GetFullPathNameW(p.c_str(), MAX_PATH, full, nullptr)) continue;
        const DWORD attr = GetFileAttributesW(full);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // A directory only counts if it actually holds language data.
        if (!Engine::probeLanguages(narrow(full)).empty()) return narrow(full);
        FV_LOG("host: \"%s\" exists but holds no loadable language", narrow(full).c_str());
    }
    return narrow(dir + L"\\engine");
}

std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    std::string r = a;
    if (r.back() != '\\' && r.back() != '/') r += '\\';
    return r + b;
}

// --- framed pipe I/O -------------------------------------------------------

bool write_all(HANDLE pipe, const void* data, DWORD size)
{
    const char* p = static_cast<const char*>(data);
    DWORD done = 0;
    while (done < size) {
        DWORD n = 0;
        if (!WriteFile(pipe, p + done, size - done, &n, nullptr) || n == 0) return false;
        done += n;
    }
    return true;
}

bool read_all(HANDLE pipe, void* data, DWORD size)
{
    char* p = static_cast<char*>(data);
    DWORD done = 0;
    while (done < size) {
        DWORD n = 0;
        if (!ReadFile(pipe, p + done, size - done, &n, nullptr) || n == 0) return false;
        done += n;
    }
    return true;
}

bool send_message(HANDLE pipe, uint32_t type, const void* payload, uint32_t size)
{
    FlexVoiceMessageHeader h = { type, size };
    if (!write_all(pipe, &h, sizeof(h))) return false;
    if (size && !write_all(pipe, payload, size)) return false;
    return true;
}

bool send_error(HANDLE pipe, const std::string& msg)
{
    return send_message(pipe, FV_RESP_ERROR, msg.data(), static_cast<uint32_t>(msg.size()));
}

// --- the engine, kept warm across utterances -------------------------------

Engine g_engine;
std::string g_engineRoot;

// Resolve a voice index to the absolute .tav path for its language.
std::string tav_path_for(uint32_t voiceIndex, uint32_t language)
{
    const int li = voices::language_index_from_id(language);
    const char* langDir = (li >= 0) ? voices::kLanguages[li].dir : "English";
    const int vi = (voiceIndex < static_cast<uint32_t>(voices::kVoiceCount))
                       ? static_cast<int>(voiceIndex) : 0;
    return join(join(g_engineRoot, langDir), voices::kVoices[vi].tavRelative);
}

// A soft ceiling so no combination of user parameters can produce the harsh
// wrap-around of a clipped 16-bit sample. Only touches samples that were going
// to clip anyway, so it is inaudible at normal levels.
void limit(std::vector<unsigned char>& pcm)
{
    if (pcm.size() < 2) return;
    short* s = reinterpret_cast<short*>(&pcm[0]);
    const size_t n = pcm.size() / 2;
    const int kKnee = 29000;
    for (size_t i = 0; i < n; ++i) {
        int v = s[i];
        const int a = v < 0 ? -v : v;
        if (a > kKnee) {
            // compress the top 3768 counts into the remaining headroom
            const int over = a - kKnee;
            const int comp = kKnee + (over * (32700 - kKnee)) / (32768 - kKnee);
            v = (v < 0) ? -comp : comp;
            s[i] = static_cast<short>(v);
        }
    }
}

bool handle_speak(HANDLE pipe, const std::vector<char>& payload)
{
    if (payload.size() < sizeof(FlexVoiceSpeakRequest)) {
        return send_error(pipe, "short speak request");
    }
    FlexVoiceSpeakRequest req;
    memcpy(&req, payload.data(), sizeof(req));

    size_t off = sizeof(FlexVoiceSpeakRequest);
    std::vector<Segment> segments;
    segments.reserve(req.segmentCount);
    for (uint32_t i = 0; i < req.segmentCount; ++i) {
        if (off + sizeof(FlexVoiceSegmentHeader) > payload.size()) {
            return send_error(pipe, "truncated segment list");
        }
        FlexVoiceSegmentHeader sh;
        memcpy(&sh, payload.data() + off, sizeof(sh));
        off += sizeof(sh);

        Segment seg;
        seg.kind = static_cast<FlexVoiceSegmentKind>(sh.kind);
        seg.value = sh.value;
        if (sh.kind == FV_SEG_TEXT || sh.kind == FV_SEG_SPELL) {
            if (off + sh.value > payload.size()) {
                return send_error(pipe, "truncated segment text");
            }
            seg.text.assign(payload.data() + off, payload.data() + off + sh.value);
            off += sh.value;
        }
        segments.push_back(seg);
    }

    FV_LOG("host: speak request, voice %u lang 0x%04x rate %u mask 0x%04x, %u segment(s)",
           req.voiceIndex, req.language, req.sampleRate, req.paramMask, req.segmentCount);

    std::string err;
    if (!g_engine.open(g_engineRoot, req.language, err)) {
        return send_error(pipe, "cannot load language: " + err);
    }
    const std::string tav = tav_path_for(req.voiceIndex, req.language);
    if (!g_engine.selectVoice(tav, req.params, req.paramMask,
                              static_cast<int>(req.sampleRate), err)) {
        return send_error(pipe, "cannot select voice: " + err);
    }
    if (!g_engine.speak(segments, req.wantWordEvents != 0, req.wantSentenceEvents != 0, err)) {
        return send_error(pipe, "cannot speak: " + err);
    }

    // Stream results as they appear. A write failure means the client closed
    // the handle, which is how a cancel reaches us -- stop the engine at once.
    //
    // The deadline is a real watchdog, not a formality. Hostile input can wedge
    // the engine permanently: no audio, no BM_TEXT_END, and a worker thread
    // still inside the synthesiser so the engine cannot even be destroyed. The
    // normalizer removes every trigger we know of, but the engine is closed
    // source and thirty years old, so assume there are more. Since it renders
    // around seventy times faster than real time, anything still silent after
    // a few seconds is wedged rather than slow.
    size_t textBytes = 0;
    for (const Segment& s : segments) textBytes += s.text.size();
    const DWORD budget = 5000 + static_cast<DWORD>(textBytes * 20);   // ~50 chars/s floor
    const DWORD deadline = GetTickCount() + (budget > 60000 ? 60000 : budget);

    for (;;) {
        StreamItem item;
        if (!g_engine.poll(item)) {
            if (g_engine.finished()) break;
            // Wait on the engine's own signal rather than polling. A Sleep(1)
            // here costs a full 15.6 ms timer tick, which was most of the
            // per-keystroke latency once the pipe reconnect was fixed.
            g_engine.waitForItem(5);
            if (GetTickCount() > deadline) {
                FV_LOG("host: engine wedged on this utterance; exiting so the next "
                       "request gets a clean one");
                send_error(pipe, "the FlexVoice engine stopped responding");
                FlushFileBuffers(pipe);
                // Do not unwind. The engine's worker thread is still running
                // inside the synthesiser and any teardown from here faults.
                // The client relaunches us; startup is about 30 ms.
                TerminateProcess(GetCurrentProcess(), 2);
            }
            continue;
        }

        bool ok = true;
        switch (item.kind) {
        case StreamItem::AUDIO:
            limit(item.audio);
            ok = send_message(pipe, FV_RESP_AUDIO, item.audio.data(),
                              static_cast<uint32_t>(item.audio.size()));
            break;
        case StreamItem::BOOKMARK:
            ok = send_message(pipe, FV_RESP_BOOKMARK, &item.value, sizeof(item.value));
            break;
        case StreamItem::WORD:
        case StreamItem::SENTENCE: {
            FlexVoiceBoundary b = { item.position, item.length };
            ok = send_message(pipe,
                              item.kind == StreamItem::WORD ? FV_RESP_WORD : FV_RESP_SENTENCE,
                              &b, sizeof(b));
            break;
        }
        case StreamItem::DONE:
            return send_message(pipe, FV_RESP_END, nullptr, 0);
        case StreamItem::FAILED:
            g_engine.stop();
            return send_error(pipe, item.message.empty() ? "engine error" : item.message);
        }

        if (!ok) {
            FV_LOG("host: client went away mid-utterance, stopping the engine");
            g_engine.stop();
            return false;
        }
    }
    return send_message(pipe, FV_RESP_END, nullptr, 0);
}

bool handle_get_voices(HANDLE pipe)
{
    std::string out;
    const std::vector<uint32_t> langs = Engine::probeLanguages(g_engineRoot);
    for (int i = 0; i < voices::kVoiceCount; ++i) {
        const voices::Voice& v = voices::kVoices[i];
        const uint32_t id = voices::kLanguages[v.languageIndex].id;
        bool available = false;
        for (size_t k = 0; k < langs.size(); ++k) {
            if (langs[k] == id) { available = true; break; }
        }
        if (!available) continue;
        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%d\t%S\t%s\t%S\t%S\n",
                    i, v.displayName, voices::kLanguages[v.languageIndex].code,
                    v.gender, v.age);
        out += line;
    }
    return send_message(pipe, FV_RESP_VOICES, out.data(), static_cast<uint32_t>(out.size()));
}

void handle_client(HANDLE pipe)
{
    for (;;) {
        FlexVoiceMessageHeader h = {};
        if (!read_all(pipe, &h, sizeof(h))) return;
        if (h.size > FLEXVOICE_MAX_MESSAGE) {
            FV_LOG("host: refusing a %u byte message, dropping the connection", h.size);
            return;
        }
        std::vector<char> payload(h.size);
        if (h.size && !read_all(pipe, &payload[0], h.size)) return;

        switch (h.type) {
        case FV_CMD_PING: {
            FlexVoicePong pong = { FLEXVOICE_PROTOCOL_VERSION,
                                   g_engine.isOpen() ? 1u : 0u,
                                   static_cast<uint32_t>(voices::kVoiceCount) };
            if (!send_message(pipe, FV_RESP_PONG, &pong, sizeof(pong))) return;
            break;
        }
        case FV_CMD_SPEAK:
            if (!handle_speak(pipe, payload)) return;
            break;
        case FV_CMD_GET_VOICES:
            if (!handle_get_voices(pipe)) return;
            break;
        case FV_CMD_SHUTDOWN:
            FV_LOG("host: shutdown requested");
            InterlockedExchange(&g_shuttingDown, 1);
            send_message(pipe, FV_RESP_END, nullptr, 0);
            return;
        default:
            FV_LOG("host: unknown command %u", h.type);
            if (!send_error(pipe, "unknown command")) return;
            break;
        }
    }
}

int run_server()
{
    // No timeBeginPeriod here on purpose. Raising the system timer resolution
    // looked like an obvious win for latency and, measured, changes nothing:
    // 6.1 ms per utterance with it, 6.1 ms without. The reason is that nothing
    // on this path sleeps -- the reader waits on an event the output site
    // signals. A system-wide timer change with no measurable benefit is not
    // worth the power it costs.

    g_serverMutex = CreateMutexW(nullptr, TRUE, FLEXVOICE_SERVER_MUTEX);
    if (!g_serverMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        FV_LOG("host: another instance already holds the mutex, exiting");
        return 0;
    }

    // The engine drops trace files into the current directory; keep those out
    // of Program Files and out of whatever directory the client was started in.
    const std::wstring logDir = SettingsStore::log_dir();
    SetCurrentDirectoryW(logDir.c_str());

    g_engineRoot = engine_root();
    FV_LOG("host: starting, engine root \"%s\"", g_engineRoot.c_str());

    // Warm the engine so the first utterance is not the one that pays for it.
    std::string err;
    const std::vector<uint32_t> langs = Engine::probeLanguages(g_engineRoot);
    if (!langs.empty()) {
        if (!g_engine.open(g_engineRoot, langs[0], err)) {
            FV_LOG("host: preload failed: %s", err.c_str());
        }
    } else {
        FV_LOG("host: no usable language data under \"%s\"", g_engineRoot.c_str());
    }

    // The pipe instance is created once and never destroyed. Recreating it per
    // client leaves a window in which the pipe name does not exist at all, and
    // a client that connects in that window gets ERROR_FILE_NOT_FOUND and backs
    // off. That is not a rare race: cancelling drops the connection, so it
    // happened on *every* keystroke while arrowing through a document, and cost
    // about 100 ms each time. Connect, serve, disconnect, connect again.
    HANDLE pipe = CreateNamedPipeW(
        FLEXVOICE_PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        FV_LOG("host: CreateNamedPipe failed, %lu", GetLastError());
        if (g_serverMutex) { ReleaseMutex(g_serverMutex); CloseHandle(g_serverMutex); }
        return 1;
    }

    while (!g_shuttingDown) {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            handle_client(pipe);
            FlushFileBuffers(pipe);
        }
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);

    FV_LOG("host: exiting");
    if (g_serverMutex) { ReleaseMutex(g_serverMutex); CloseHandle(g_serverMutex); }
    return 0;
}

int request_shutdown()
{
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, FLEXVOICE_SERVER_MUTEX);
    if (!running) return 0;      // nothing to stop
    CloseHandle(running);

    HANDLE pipe = CreateFileW(FLEXVOICE_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    send_message(pipe, FV_CMD_SHUTDOWN, nullptr, 0);
    FlexVoiceMessageHeader h = {};
    read_all(pipe, &h, sizeof(h));
    CloseHandle(pipe);

    for (int i = 0; i < 40; ++i) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, FLEXVOICE_SERVER_MUTEX);
        if (!m) return 0;
        CloseHandle(m);
        Sleep(50);
    }
    return 1;
}

int self_test(const char* root)
{
    log::enabled() = true;
    g_engineRoot = root ? root : engine_root();
    printf("engine root: %s\n", g_engineRoot.c_str());

    const std::vector<uint32_t> langs = Engine::probeLanguages(g_engineRoot);
    printf("languages with data: %u\n", static_cast<unsigned>(langs.size()));
    for (size_t i = 0; i < langs.size(); ++i) {
        const int li = voices::language_index_from_id(langs[i]);
        printf("  0x%04x %s\n", langs[i], li >= 0 ? voices::kLanguages[li].code : "?");
    }
    if (langs.empty()) return 1;

    std::string err;
    if (!g_engine.open(g_engineRoot, langs[0], err)) {
        printf("open failed: %s\n", err.c_str());
        return 1;
    }

    int failures = 0;
    for (int v = 0; v < voices::kVoiceCount; ++v) {
        double params[FVP_COUNT] = {};
        uint32_t mask = 0;
        const voices::Voice& def = voices::kVoices[v];
        for (int k = 0; k < def.overrideCount; ++k) {
            for (int p = 0; p < FVP_COUNT; ++p) {
                // map the override name onto a protocol parameter where one exists
                static const char* names[FVP_COUNT] = {
                    "speechRate", "volume", "defaultPitch", "pitchRate", "pitchMin",
                    "pitchMax", "intonationLevel", "headsize", "tilt", "richness",
                    "breathiness", "smoothness", "fricationRate", "plosiveRate" };
                if (strcmp(def.overrides[k].name, names[p]) == 0) {
                    params[p] = def.overrides[k].value;
                    mask |= (1u << p);
                }
            }
        }
        const std::string tav = tav_path_for(v, langs[0]);
        if (!g_engine.selectVoice(tav, params, mask, 16000, err)) {
            printf("  %-28S FAIL select: %s\n", def.displayName, err.c_str());
            ++failures;
            continue;
        }
        std::vector<Segment> segs(1);
        segs[0].kind = FV_SEG_TEXT;
        segs[0].text = "FlexVoice self test.";
        if (!g_engine.speak(segs, true, true, err)) {
            printf("  %-28S FAIL speak: %s\n", def.displayName, err.c_str());
            ++failures;
            continue;
        }
        size_t bytes = 0, words = 0, marks = 0;
        const DWORD deadline = GetTickCount() + 20000;
        for (;;) {
            StreamItem it;
            if (!g_engine.poll(it)) {
                if (g_engine.finished() || GetTickCount() > deadline) break;
                Sleep(1);
                continue;
            }
            if (it.kind == StreamItem::AUDIO) bytes += it.audio.size();
            else if (it.kind == StreamItem::WORD) ++words;
            else if (it.kind == StreamItem::BOOKMARK) ++marks;
            else if (it.kind == StreamItem::DONE) break;
            else if (it.kind == StreamItem::FAILED) { printf("  stream error: %s\n", it.message.c_str()); ++failures; break; }
        }
        printf("  %-28S %7u bytes  %u word events\n", def.displayName,
               static_cast<unsigned>(bytes), static_cast<unsigned>(words));
        if (bytes == 0) ++failures;
    }
    printf("%s\n", failures ? "SELFTEST FAILED" : "SELFTEST OK");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    silence_crt_popups();
    log::component() = L"host";
    log::enabled() = SettingsStore::current().debugLogging;

    for (int i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "--shutdown") == 0) return request_shutdown();
        if (_stricmp(argv[i], "--selftest") == 0) {
            log::enabled() = true;
            return self_test(i + 1 < argc ? argv[i + 1] : nullptr);
        }
    }
    return run_server();
}
