// fv2_host.exe -- the only process that loads FlexVoice_2_00_010.dll.
//
// The engine is a 32-bit DLL, so a 64-bit SAPI client cannot load it at all.
// It can also fault on input, and a fault inside a screen reader's process
// would take the screen reader down with it. So both bitnesses talk to this
// process over a named pipe instead. The host is cheap to restart; NVDA is not.
//
// One engine is kept warm for the life of the process. Selecting a voice costs
// about 4 ms because it reuses that engine through setSpeaker rather than
// building a new one, and rate and volume never touch a voice file at all --
// they are engine-level multipliers that apply immediately.

#include "fv2_engine.hpp"
#include "fv2_limiter.hpp"
#include "fv2_log.h"
#include "fv2_protocol.h"
#include "fv2_speaker.hpp"
#include "fv2_voices.hpp"

#include <windows.h>

#include <crtdbg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

namespace {

// The engine calls abort() on some malformed input. Left alone, that puts a
// modal "This application has requested the Runtime to terminate" box on the
// screen of a blind user, who cannot see it, and the host stops answering.
// Dying quietly is much better: the client notices the broken pipe and
// relaunches us.
void silence_crt_popups()
{
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
#endif
    signal(SIGABRT, [](int) { _exit(3); });
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

std::wstring module_dir()
{
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    std::string r = a;
    if (r.back() != '\\' && r.back() != '/') r += '\\';
    return r + b;
}

// The engine's Data directory: <install>\engine\Data, holding RHL2.dat,
// Julie.bin and Voices\*.tav.
//
// FLEXVOICE2_DATA overrides it outright. Otherwise the search walks up from
// the executable, which matters because a build tree puts the host in
// build_x86\Release while the engine sits at the root -- and the host is
// launched as a child process by whichever client wanted it, so it cannot ask
// anyone where it is.
std::string engine_data_dir()
{
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"FLEXVOICE2_DATA", buf, MAX_PATH) > 0) {
        return narrow(buf);
    }

    std::string dir = narrow(module_dir());
    for (int up = 0; up < 4; ++up) {
        const std::string candidate = join(join(dir, "engine"), "Data");
        const DWORD attrs = GetFileAttributesA(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidate;
        }
        const size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos) break;
        dir = dir.substr(0, slash);
    }
    // Nothing found; return the installed location so the error names it.
    return join(join(narrow(module_dir()), "engine"), "Data");
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
    Fv2MessageHeader h;
    h.type = type;
    h.size = size;
    if (!write_all(pipe, &h, sizeof(h))) return false;
    return size == 0 || write_all(pipe, payload, size);
}

bool send_error(HANDLE pipe, const std::string& msg)
{
    FV2_LOG("error to client: %s", msg.c_str());
    return send_message(pipe, FV2_RESP_ERROR, msg.data(), (uint32_t)msg.size());
}

// --- the engine, kept warm across utterances -------------------------------

fv2::Engine g_engine;
std::string g_dataDir;

const fv2::VoiceInfo* find_voice(const std::string& name)
{
    const std::vector<fv2::VoiceInfo>& v = fv2::builtinVoices();
    for (size_t i = 0; i < v.size(); ++i) {
        if (name == v[i].name) return &v[i];
    }
    return nullptr;
}

// --- request handlers ------------------------------------------------------

bool handle_speak(HANDLE pipe, const std::vector<char>& payload)
{
    if (payload.size() < sizeof(Fv2SpeakRequest)) {
        return send_error(pipe, "truncated speak request");
    }
    Fv2SpeakRequest req;
    memcpy(&req, payload.data(), sizeof(req));

    size_t off = sizeof(req);
    auto take = [&](size_t n, const char** out) -> bool {
        if (off + n > payload.size()) return false;
        *out = payload.data() + off;
        off += n;
        return true;
    };

    const char* namePtr = nullptr;
    if (!take(req.voiceBytes, &namePtr)) return send_error(pipe, "bad voice name");
    const std::string voiceName(namePtr, req.voiceBytes);

    // The client names one of the built-in voices as the base and sends only
    // the parameters that differ from it. The Custom Voice is not a separate
    // file on disk: it is a base plus overrides, which is why a user can
    // reshape it without ever touching the engine's own voice files.
    const fv2::VoiceInfo* info = find_voice(voiceName);
    if (!info) {
        info = &fv2::builtinVoices()[0];
        FV2_LOG("unknown voice '%s'; falling back to %s", voiceName.c_str(), info->name);
    }

    fv2::SpeakerDef def;
    std::string err;
    const std::string tav = join(join(g_dataDir, "Voices"), info->tavFile);
    if (!def.loadFile(tav, err)) return send_error(pipe, err);

    // The measured trim, unless the client asks for a volume of its own.
    bool volumeOverridden = false;
    for (uint32_t i = 0; i < req.paramCount; ++i) {
        const char* p = nullptr;
        if (!take(sizeof(Fv2ParamOverride), &p)) return send_error(pipe, "bad parameter list");
        Fv2ParamOverride ov;
        memcpy(&ov, p, sizeof(ov));
        if (ov.id >= (uint32_t)fv2::P_COUNT) continue;
        def.set((fv2::ParamId)ov.id, ov.value);
        if (ov.id == (uint32_t)fv2::P_VOLUME) volumeOverridden = true;
    }
    if (!volumeOverridden && info->trimmedVolume > 0.0) {
        def.set(fv2::P_VOLUME, info->trimmedVolume);
    }

    std::vector<fv2::Segment> segments;
    segments.reserve(req.segmentCount);
    for (uint32_t i = 0; i < req.segmentCount; ++i) {
        const char* p = nullptr;
        if (!take(sizeof(Fv2Segment), &p)) return send_error(pipe, "bad segment list");
        Fv2Segment s;
        memcpy(&s, p, sizeof(s));
        const char* text = nullptr;
        if (!take(s.textBytes, &text)) return send_error(pipe, "bad segment text");

        fv2::Segment seg;
        seg.kind = (Fv2SegmentKind)s.kind;
        seg.value = s.value;
        seg.text.assign(text, s.textBytes);
        segments.push_back(seg);
    }

    if (!g_engine.selectVoice(def, 16000, err)) return send_error(pipe, err);

    FV2_VLOG("speak: voice=%s segments=%u rate=%.3f volume=%.3f",
             info->name, (unsigned)segments.size(), req.rate, req.volume);

    if (!g_engine.speak(segments,
                        (req.flags & FV2_SPEAK_WORDS) != 0,
                        (req.flags & FV2_SPEAK_SENTENCES) != 0,
                        req.rate, req.volume, err)) {
        return send_error(pipe, err);
    }

    // Stream results back as they arrive. A client cancels by closing its
    // handle, which fails the next write; that is the cancel path, and it is
    // why nothing here waits for the whole utterance before sending.
    for (;;) {
        fv2::StreamItem item;
        if (!g_engine.poll(item)) {
            if (g_engine.finished()) break;
            g_engine.waitForItem(100);
            continue;
        }
        bool ok = true;
        switch (item.kind) {
        case fv2::StreamItem::AUDIO:
            fv2::limit(item.audio);
            ok = send_message(pipe, FV2_RESP_AUDIO, item.audio.data(),
                              (uint32_t)item.audio.size());
            break;
        case fv2::StreamItem::BOOKMARK:
            ok = send_message(pipe, FV2_RESP_BOOKMARK, &item.value, sizeof(item.value));
            break;
        case fv2::StreamItem::WORD:
        case fv2::StreamItem::SENTENCE: {
            Fv2Boundary b;
            b.position = item.position;
            b.length = item.length;
            ok = send_message(pipe,
                              item.kind == fv2::StreamItem::WORD ? FV2_RESP_WORD
                                                                 : FV2_RESP_SENTENCE,
                              &b, sizeof(b));
            break;
        }
        case fv2::StreamItem::DONE:
            return send_message(pipe, FV2_RESP_END, nullptr, 0);
        case fv2::StreamItem::FAILED:
            return send_error(pipe, item.message);
        }
        if (!ok) {
            // The client went away mid-utterance. Stop the engine so the next
            // client does not inherit a running one.
            FV2_LOG("client disconnected mid-utterance; stopping");
            g_engine.stop();
            return false;
        }
    }
    return send_message(pipe, FV2_RESP_END, nullptr, 0);
}

bool handle_get_voices(HANDLE pipe)
{
    std::string out;
    const std::vector<fv2::VoiceInfo>& v = fv2::builtinVoices();
    for (size_t i = 0; i < v.size(); ++i) {
        out += v[i].name;
        out += "\tEnglish\t";
        out += v[i].gender;
        out += "\t";
        out += v[i].age;
        out += "\t";
        out += v[i].tavFile;
        out += "\n";
    }
    return send_message(pipe, FV2_RESP_VOICES, out.data(), (uint32_t)out.size());
}

void handle_client(HANDLE pipe)
{
    for (;;) {
        Fv2MessageHeader h;
        if (!read_all(pipe, &h, sizeof(h))) return;
        if (h.size > FV2_MAX_MESSAGE) {
            FV2_LOG("message of %u bytes exceeds the cap; dropping the connection", h.size);
            return;
        }
        std::vector<char> payload(h.size);
        if (h.size && !read_all(pipe, payload.data(), h.size)) return;

        switch (h.type) {
        case FV2_CMD_PING: {
            Fv2Pong p;
            p.protocolVersion = FV2_PROTOCOL_VERSION;
            p.engineReady = g_engine.isOpen() ? 1u : 0u;
            p.voiceCount = (uint32_t)fv2::builtinVoices().size();
            p.sampleRate = (uint32_t)g_engine.sampleRate();
            if (!send_message(pipe, FV2_RESP_PONG, &p, sizeof(p))) return;
            break;
        }
        case FV2_CMD_SPEAK:
            if (!handle_speak(pipe, payload)) return;
            break;
        case FV2_CMD_GET_VOICES:
            if (!handle_get_voices(pipe)) return;
            break;
        case FV2_CMD_SHUTDOWN:
            FV2_LOG("shutdown requested");
            send_message(pipe, FV2_RESP_END, nullptr, 0);
            ExitProcess(0);
        default:
            if (!send_error(pipe, "unknown command")) return;
            break;
        }
    }
}

int run_server()
{
    fv2::log::component() = L"host";
    FV2_LOG("host starting, protocol %u", FV2_PROTOCOL_VERSION);

    // One host per machine session.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, FV2_SERVER_MUTEX);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        FV2_LOG("another host is already running; exiting");
        return 0;
    }

    g_dataDir = engine_data_dir();
    std::string err;
    if (!g_engine.open(g_dataDir, err)) {
        FV2_LOG("engine failed to open: %s", err.c_str());
        // Keep serving anyway: a client that pings gets engineReady=0 and can
        // report something better than silence.
    }

    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            FV2_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            FV2_LOG("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(200);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            FV2_VLOG("client connected");
            handle_client(pipe);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

int request_shutdown()
{
    HANDLE pipe = CreateFileW(FV2_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 0;   // nothing running
    Fv2MessageHeader h;
    h.type = FV2_CMD_SHUTDOWN;
    h.size = 0;
    DWORD n = 0;
    WriteFile(pipe, &h, sizeof(h), &n, nullptr);
    CloseHandle(pipe);
    return 0;
}

// A self-check the installer and a user can both run: does the engine start,
// and does every voice speak?
int self_test(const std::string& dataDir)
{
    fv2::log::component() = L"host";
    printf("FlexVoice 2.0 host self-test\n");
    printf("data directory: %s\n", dataDir.c_str());

    const std::vector<std::string> missing = fv2::Engine::missingDataFiles(dataDir);
    if (!missing.empty()) {
        printf("MISSING FILES:\n");
        for (size_t i = 0; i < missing.size(); ++i) printf("   %s\n", missing[i].c_str());
        printf("\nRHL2.dat and Julie.bin are built by the installer with FVZip.exe.\n");
        return 1;
    }
    printf("all required data files present\n");

    fv2::Engine eng;
    std::string err;
    if (!eng.open(dataDir, err)) {
        printf("FAILED to open the engine: %s\n", err.c_str());
        return 1;
    }
    printf("engine opened\n");

    int failures = 0;
    const std::vector<fv2::VoiceInfo>& voices = fv2::builtinVoices();
    for (size_t i = 0; i < voices.size(); ++i) {
        fv2::SpeakerDef def;
        if (!def.loadFile(join(join(dataDir, "Voices"), voices[i].tavFile), err)) {
            printf("  %-8s FAILED: %s\n", voices[i].name, err.c_str());
            ++failures;
            continue;
        }
        def.set(fv2::P_VOLUME, voices[i].trimmedVolume);
        if (!eng.selectVoice(def, 16000, err)) {
            printf("  %-8s FAILED: %s\n", voices[i].name, err.c_str());
            ++failures;
            continue;
        }
        fv2::Segment seg;
        seg.kind = FV2_SEG_TEXT;
        seg.text = "FlexVoice two, testing one two three.";
        if (!eng.speak(std::vector<fv2::Segment>(1, seg), false, false, 1.0, 1.0, err)) {
            printf("  %-8s FAILED: %s\n", voices[i].name, err.c_str());
            ++failures;
            continue;
        }
        size_t bytes = 0;
        const DWORD deadline = GetTickCount() + 20000;
        for (;;) {
            fv2::StreamItem it;
            if (!eng.poll(it)) {
                if (eng.finished() || GetTickCount() > deadline) break;
                eng.waitForItem(50);
                continue;
            }
            if (it.kind == fv2::StreamItem::AUDIO) bytes += it.audio.size();
            if (it.kind == fv2::StreamItem::DONE) break;
            if (it.kind == fv2::StreamItem::FAILED) { err = it.message; break; }
        }
        const bool ok = bytes > 8000;
        printf("  %-8s %s (%.2f seconds of audio)\n", voices[i].name,
               ok ? "ok" : "FAILED", bytes / 32000.0);
        if (!ok) ++failures;
    }

    printf("\n%s\n", failures ? "SELF-TEST FAILED" : "self-test passed");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    silence_crt_popups();
    setvbuf(stdout, nullptr, _IONBF, 0);

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shutdown") return request_shutdown();
        if (a == "--verbose") fv2::log::verbose() = true;
        if (a == "--self-test") {
            const std::string dir = (i + 1 < argc) ? argv[i + 1] : engine_data_dir();
            return self_test(dir);
        }
    }
    return run_server();
}
