#include "fv2_client.h"
#include "fv2_log.h"

#include <shlwapi.h>
#include <stdio.h>

namespace fv2 {

namespace {

std::wstring module_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &self);
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(self, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

// The host sits beside the SAPI DLL, or one level up when the caller is the
// 64-bit DLL in the x64\ subdirectory.
std::wstring find_host()
{
    const std::wstring dir = module_dir();
    const wchar_t* rel[] = { L"\\fv2_host.exe", L"\\..\\fv2_host.exe" };
    for (int i = 0; i < 2; ++i) {
        std::wstring p = dir + rel[i];
        wchar_t full[MAX_PATH] = {0};
        if (!GetFullPathNameW(p.c_str(), MAX_PATH, full, nullptr)) continue;
        if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) return full;
    }
    return dir + L"\\fv2_host.exe";
}

}  // namespace

EngineClient::EngineClient() { InitializeCriticalSection(&lock_); }

EngineClient::~EngineClient()
{
    disconnect();
    DeleteCriticalSection(&lock_);
}

EngineClient& sharedClient()
{
    static EngineClient client;
    return client;
}

bool EngineClient::isServerRunning()
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, FV2_SERVER_MUTEX);
    if (!m) return false;
    CloseHandle(m);
    return true;
}

bool EngineClient::shutdownServer()
{
    HANDLE pipe = CreateFileW(FV2_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    Fv2MessageHeader h;
    h.type = FV2_CMD_SHUTDOWN;
    h.size = 0;
    DWORD n = 0;
    const BOOL ok = WriteFile(pipe, &h, sizeof(h), &n, nullptr);
    CloseHandle(pipe);
    for (int i = 0; i < 40 && isServerRunning(); ++i) Sleep(25);
    return ok != FALSE;
}

void EngineClient::disconnect()
{
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool EngineClient::launchServer(std::string& error)
{
    // One launcher at a time, or two clients starting together spawn two hosts
    // and one of them exits after paying the engine-load cost.
    HANDLE launchLock = CreateMutexW(nullptr, FALSE, FV2_LAUNCH_MUTEX);
    if (launchLock) WaitForSingleObject(launchLock, 5000);

    bool ok = true;
    if (!isServerRunning()) {
        const std::wstring exe = find_host();
        FV2_LOG("client: starting \"%S\"", exe.c_str());
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        std::wstring cmd = L"\"" + exe + L"\"";
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');

        // The host outlives whichever client happened to start it, so it must
        // not inherit that client's job object. Plenty of launchers -- shells,
        // test harnesses, some application sandboxes -- run their children in a
        // job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, and without this flag
        // the host is killed the moment that client exits, taking speech away
        // from every other client still using it.
        PROCESS_INFORMATION pi = {};
        BOOL started = CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr,
                                      FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
                                      nullptr, nullptr, &si, &pi);
        if (!started && GetLastError() == ERROR_ACCESS_DENIED) {
            // The job forbids breakaway. Start it anyway: tied to this client's
            // lifetime is still better than no speech at all.
            FV2_LOG("client: job forbids breakaway, starting the host inside it");
            std::vector<wchar_t> retryCmd(cmd.begin(), cmd.end());
            retryCmd.push_back(L'\0');
            started = CreateProcessW(exe.c_str(), retryCmd.data(), nullptr, nullptr,
                                     FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        }

        if (started) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            for (int i = 0; i < 60 && !isServerRunning(); ++i) Sleep(50);
            FV2_LOG("client: launched the engine host");
        } else {
            char buf[512];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "cannot start the FlexVoice 2 engine host (error %lu)",
                        GetLastError());
            error = buf;
            FV2_LOG("client: %s", error.c_str());
            ok = false;
        }
    }

    if (launchLock) { ReleaseMutex(launchLock); CloseHandle(launchLock); }
    return ok;
}

bool EngineClient::ensureConnected(std::string& error)
{
    if (pipe_ != INVALID_HANDLE_VALUE) return true;

    // Reconnecting is on the critical path: cancelling an utterance drops the
    // connection, so every keystroke while arrowing through a document comes
    // back through here. Retry fast and only slow down once it is clear the
    // host really is not there -- a flat 100 ms back-off would put 100 ms on
    // every single keypress.
    bool launched = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        pipe_ = CreateFileW(FV2_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) break;

        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // Another client is mid-utterance. Wait for an instance rather
            // than spinning.
            WaitNamedPipeW(FV2_PIPE_NAME, 2000);
            continue;
        }
        // Launch on the first miss, and again if the host died between the
        // mutex check and the connect. Gating this on attempt == 0 alone would
        // mean a single ERROR_PIPE_BUSY on the first try consumes the only
        // chance to start the host, and every later attempt fails for no reason.
        if (!launched) {
            launched = true;
            if (!launchServer(error)) return false;
        }
        // The host is between DisconnectNamedPipe and ConnectNamedPipe for
        // microseconds at a time, so the first few retries should be immediate.
        if (attempt < 20)      Sleep(0);
        else if (attempt < 30) Sleep(2);
        else                   Sleep(50);
    }

    if (pipe_ == INVALID_HANDLE_VALUE) {
        if (error.empty()) error = "cannot connect to the FlexVoice 2 engine host";
        FV2_LOG("client: %s (host path \"%S\", server mutex %s)", error.c_str(),
                find_host().c_str(), isServerRunning() ? "held" : "free");
        return false;
    }

    if (!versionChecked_) {
        // An upgrade that leaves an old host running would otherwise be a
        // silent field failure: the structs would have moved underneath it.
        Fv2Pong pong = {};
        std::string perr;
        if (pingLocked(pong, perr) && pong.protocolVersion != FV2_PROTOCOL_VERSION) {
            FV2_LOG("client: host speaks protocol %u, we speak %u; replacing it",
                    pong.protocolVersion, FV2_PROTOCOL_VERSION);
            sendMessage(FV2_CMD_SHUTDOWN, nullptr, 0);
            disconnect();
            for (int i = 0; i < 60 && isServerRunning(); ++i) Sleep(50);
            return ensureConnected(error);
        }
        versionChecked_ = true;
    }
    return true;
}

bool EngineClient::sendMessage(uint32_t type, const void* payload, uint32_t size)
{
    Fv2MessageHeader h;
    h.type = type;
    h.size = size;
    DWORD n = 0;
    if (!WriteFile(pipe_, &h, sizeof(h), &n, nullptr) || n != sizeof(h)) return false;
    const char* p = static_cast<const char*>(payload);
    DWORD done = 0;
    while (done < size) {
        if (!WriteFile(pipe_, p + done, size - done, &n, nullptr) || n == 0) return false;
        done += n;
    }
    return true;
}

bool EngineClient::readPayload(void* data, uint32_t size)
{
    char* p = static_cast<char*>(data);
    DWORD done = 0;
    while (done < size) {
        DWORD n = 0;
        if (!ReadFile(pipe_, p + done, size - done, &n, nullptr) || n == 0) return false;
        done += n;
    }
    return true;
}

bool EngineClient::skipPayload(uint32_t size)
{
    char scratch[4096];
    while (size) {
        const DWORD chunk = size < sizeof(scratch) ? size : (DWORD)sizeof(scratch);
        if (!readPayload(scratch, chunk)) return false;
        size -= chunk;
    }
    return true;
}

bool EngineClient::readHeader(Fv2MessageHeader& h)
{
    if (!readPayload(&h, sizeof(h))) return false;
    if (h.size > FV2_MAX_MESSAGE) {
        FV2_LOG("client: host sent a %u-byte message; dropping the connection", h.size);
        return false;
    }
    return true;
}

// Assumes the caller holds the lock and the connection is up. ensureConnected
// uses this for its version check, which is why it cannot be the public entry
// point: connecting would recurse.
bool EngineClient::pingLocked(Fv2Pong& pong, std::string& error)
{
    if (!sendMessage(FV2_CMD_PING, nullptr, 0)) { error = "ping failed"; return false; }
    Fv2MessageHeader h = {};
    if (!readHeader(h) || h.type != FV2_RESP_PONG) { error = "no pong"; return false; }
    if (h.size < sizeof(pong)) { error = "short pong"; return false; }
    if (!readPayload(&pong, sizeof(pong))) { error = "short pong"; return false; }
    if (h.size > sizeof(pong) && !skipPayload(h.size - sizeof(pong))) return false;
    return true;
}

// The public entry point. It connects first -- which the previous version did
// not, so the warm-up ping fired on a handle that was still
// INVALID_HANDLE_VALUE, failed immediately, and every log in the field showed
// "warm-up ping failed: ping failed". The warm-up existed to move the ~150 ms
// host launch off the user's first keystroke, and it had never once done so.
bool EngineClient::ping(Fv2Pong& pong, std::string& error)
{
    EnterCriticalSection(&lock_);
    bool ok = false;
    if (ensureConnected(error)) {
        ok = pingLocked(pong, error);
        if (!ok) disconnect();
    }
    LeaveCriticalSection(&lock_);
    return ok;
}

bool EngineClient::getVoices(std::string& out, std::string& error)
{
    EnterCriticalSection(&lock_);
    bool ok = false;
    if (ensureConnected(error) && sendMessage(FV2_CMD_GET_VOICES, nullptr, 0)) {
        Fv2MessageHeader h = {};
        if (readHeader(h) && h.type == FV2_RESP_VOICES) {
            out.assign(h.size, '\0');
            ok = (h.size == 0) || readPayload(&out[0], h.size);
        }
    }
    if (!ok) { disconnect(); if (error.empty()) error = "could not read the voice list"; }
    LeaveCriticalSection(&lock_);
    return ok;
}

bool EngineClient::speakOnce(const SpeakParams& params,
                             const std::vector<SpeakSegment>& segments,
                             const SpeakSink& sink, std::string& error, bool& cancelled)
{
    cancelled = false;

    std::vector<char> payload;
    Fv2SpeakRequest req = {};
    req.flags = (params.wantWordEvents ? FV2_SPEAK_WORDS : 0u) |
                (params.wantSentenceEvents ? FV2_SPEAK_SENTENCES : 0u);
    req.rate = params.rate;
    req.volume = params.volume;
    req.voiceBytes = (uint32_t)params.baseVoice.size();
    req.paramCount = (uint32_t)params.overrides.size();
    req.segmentCount = (uint32_t)segments.size();

    const char* p = reinterpret_cast<const char*>(&req);
    payload.insert(payload.end(), p, p + sizeof(req));
    payload.insert(payload.end(), params.baseVoice.begin(), params.baseVoice.end());
    for (size_t i = 0; i < params.overrides.size(); ++i) {
        const char* o = reinterpret_cast<const char*>(&params.overrides[i]);
        payload.insert(payload.end(), o, o + sizeof(Fv2ParamOverride));
    }
    for (size_t i = 0; i < segments.size(); ++i) {
        Fv2Segment sh;
        sh.kind = (uint32_t)segments[i].kind;
        sh.value = segments[i].value;
        sh.textBytes = (uint32_t)segments[i].text.size();
        const char* s = reinterpret_cast<const char*>(&sh);
        payload.insert(payload.end(), s, s + sizeof(sh));
        payload.insert(payload.end(), segments[i].text.begin(), segments[i].text.end());
    }

    if (!sendMessage(FV2_CMD_SPEAK, payload.data(), (uint32_t)payload.size())) return false;

    std::vector<unsigned char> buffer;
    for (;;) {
        Fv2MessageHeader h = {};
        if (!readHeader(h)) return false;

        switch (h.type) {
        case FV2_RESP_AUDIO: {
            buffer.resize(h.size);
            if (h.size && !readPayload(&buffer[0], h.size)) return false;
            if (sink.audio && !sink.audio(buffer.data(), h.size)) {
                // Cancel: drop the handle. The host's next write fails and it
                // stops the engine, which is instantaneous.
                cancelled = true;
                disconnect();
                return true;
            }
            break;
        }
        case FV2_RESP_BOOKMARK: {
            uint32_t id = 0;
            if (h.size < sizeof(id) || !readPayload(&id, sizeof(id))) return false;
            if (h.size > sizeof(id) && !skipPayload(h.size - sizeof(id))) return false;
            if (sink.bookmark && !sink.bookmark(id)) { cancelled = true; disconnect(); return true; }
            break;
        }
        case FV2_RESP_WORD:
        case FV2_RESP_SENTENCE: {
            Fv2Boundary b = {};
            if (h.size < sizeof(b) || !readPayload(&b, sizeof(b))) return false;
            if (h.size > sizeof(b) && !skipPayload(h.size - sizeof(b))) return false;
            const std::function<bool(uint32_t, uint32_t)>& cb =
                (h.type == FV2_RESP_WORD) ? sink.word : sink.sentence;
            if (cb && !cb(b.position, b.length)) { cancelled = true; disconnect(); return true; }
            break;
        }
        case FV2_RESP_END:
            if (h.size) skipPayload(h.size);
            return true;
        case FV2_RESP_ERROR: {
            std::string msg(h.size, '\0');
            if (h.size && !readPayload(&msg[0], h.size)) return false;
            error = msg.empty() ? "the engine reported an error" : msg;
            FV2_LOG("client: host error: %s", error.c_str());
            return true;      // a reported error is a complete exchange
        }
        default:
            if (!skipPayload(h.size)) return false;
            break;
        }
    }
}

bool EngineClient::speak(const SpeakParams& params,
                         const std::vector<SpeakSegment>& segments,
                         const SpeakSink& sink, std::string& error)
{
    EnterCriticalSection(&lock_);
    bool ok = false;
    // Two attempts: the first can fail because the host exited between
    // utterances, which is normal after an upgrade or a crash. The second is a
    // real failure.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(error)) break;
        bool cancelled = false;
        if (speakOnce(params, segments, sink, error, cancelled)) { ok = true; break; }
        disconnect();
        if (attempt == 0) error.clear();
    }
    if (!ok && error.empty()) error = "the FlexVoice 2 engine host stopped responding";
    LeaveCriticalSection(&lock_);
    return ok;
}

}  // namespace fv2
