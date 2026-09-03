#include "engine_client.h"
#include "flexvoice_log.h"
#include "settings.h"

#include <shlwapi.h>
#include <stdio.h>

namespace FlexVoice {

namespace {

std::wstring module_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &self);
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(self, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

// The host sits beside the SAPI DLL, or one level up when the caller is the
// 64-bit DLL in the x64\ subdirectory.
std::wstring find_host()
{
    const std::wstring dir = module_dir();
    const wchar_t* rel[] = { L"\\flexvoice_host.exe", L"\\..\\flexvoice_host.exe" };
    for (int i = 0; i < 2; ++i) {
        std::wstring p = dir + rel[i];
        wchar_t full[MAX_PATH] = {};
        if (!GetFullPathNameW(p.c_str(), MAX_PATH, full, nullptr)) continue;
        if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) return full;
    }
    return dir + L"\\flexvoice_host.exe";
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
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, FLEXVOICE_SERVER_MUTEX);
    if (!m) return false;
    CloseHandle(m);
    return true;
}

void EngineClient::disconnect()
{
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    versionChecked_ = false;
}

bool EngineClient::launchServer(std::string& error)
{
    // One launcher at a time, or two clients starting together spawn two hosts
    // and one of them exits after paying the engine-load cost.
    HANDLE launchLock = CreateMutexW(nullptr, FALSE, FLEXVOICE_LAUNCH_MUTEX);
    if (launchLock) WaitForSingleObject(launchLock, 5000);

    bool ok = true;
    if (!isServerRunning()) {
        const std::wstring exe = find_host();
        FV_LOG("client: starting \"%S\"", exe.c_str());
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
            FV_LOG("client: job forbids breakaway, starting the host inside it");
            std::vector<wchar_t> retryCmd(cmd.begin(), cmd.end());
            retryCmd.push_back(L'\0');
            started = CreateProcessW(exe.c_str(), retryCmd.data(), nullptr, nullptr,
                                     FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        }

        if (started) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            for (int i = 0; i < 60 && !isServerRunning(); ++i) Sleep(50);
            FV_LOG("client: launched the engine host");
        } else {
            char buf[512];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "cannot start the FlexVoice engine host (error %lu)", GetLastError());
            error = buf;
            FV_LOG("client: %s", error.c_str());
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
    // host really is not there -- a flat 100 ms back-off used to put 100 ms on
    // every single keypress.
    bool launched = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        pipe_ = CreateFileW(FLEXVOICE_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) break;

        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // Another client is mid-utterance. Wait for an instance rather
            // than spinning.
            WaitNamedPipeW(FLEXVOICE_PIPE_NAME, 2000);
            continue;
        }
        // Launch on the first miss, and again if the host died between the
        // mutex check and the connect. Gating this on attempt == 0 alone meant
        // a single ERROR_PIPE_BUSY on the first try consumed the only chance to
        // start the host, and every later attempt then failed for no reason.
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
        if (error.empty()) error = "cannot connect to the FlexVoice engine host";
        FV_LOG("client: %s (host path \"%S\", server mutex %s)", error.c_str(),
               find_host().c_str(), isServerRunning() ? "held" : "free");
        return false;
    }

    if (!versionChecked_) {
        // An upgrade that leaves an old host running would otherwise be a
        // silent field failure: the structs would have moved underneath it.
        FlexVoicePong pong = {};
        std::string perr;
        if (ping(pong, perr) && pong.protocolVersion != FLEXVOICE_PROTOCOL_VERSION) {
            FV_LOG("client: host speaks protocol %u, we speak %u; replacing it",
                   pong.protocolVersion, FLEXVOICE_PROTOCOL_VERSION);
            sendMessage(FV_CMD_SHUTDOWN, nullptr, 0);
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
    FlexVoiceMessageHeader h = { type, size };
    DWORD written = 0;
    if (!WriteFile(pipe_, &h, sizeof(h), &written, nullptr) || written != sizeof(h)) return false;

    const char* p = static_cast<const char*>(payload);
    uint32_t done = 0;
    while (done < size) {
        DWORD n = 0;
        if (!WriteFile(pipe_, p + done, size - done, &n, nullptr) || n == 0) return false;
        done += n;
    }
    return true;
}

bool EngineClient::readHeader(FlexVoiceMessageHeader& h)
{
    return readPayload(&h, sizeof(h)) && h.size <= FLEXVOICE_MAX_MESSAGE;
}

bool EngineClient::readPayload(void* data, uint32_t size)
{
    char* p = static_cast<char*>(data);
    uint32_t done = 0;
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
        const uint32_t chunk = size < sizeof(scratch) ? size : sizeof(scratch);
        if (!readPayload(scratch, chunk)) return false;
        size -= chunk;
    }
    return true;
}

bool EngineClient::ping(FlexVoicePong& pong, std::string& error)
{
    EnterCriticalSection(&lock_);
    bool ok = false;
    if (pipe_ != INVALID_HANDLE_VALUE || ensureConnected(error)) {
        if (sendMessage(FV_CMD_PING, nullptr, 0)) {
            FlexVoiceMessageHeader h = {};
            if (readHeader(h) && h.type == FV_RESP_PONG && h.size >= sizeof(pong)) {
                ok = readPayload(&pong, sizeof(pong));
                if (h.size > sizeof(pong)) skipPayload(h.size - sizeof(pong));
            }
        }
        if (!ok) { disconnect(); error = "the engine host did not answer"; }
    }
    LeaveCriticalSection(&lock_);
    return ok;
}

bool EngineClient::getVoices(std::string& out, std::string& error)
{
    EnterCriticalSection(&lock_);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (!ensureConnected(error)) break;
        if (!sendMessage(FV_CMD_GET_VOICES, nullptr, 0)) { disconnect(); continue; }
        FlexVoiceMessageHeader h = {};
        if (!readHeader(h)) { disconnect(); continue; }
        if (h.type != FV_RESP_VOICES) { skipPayload(h.size); disconnect(); continue; }
        out.assign(h.size, '\0');
        if (h.size && !readPayload(&out[0], h.size)) { disconnect(); continue; }
        ok = true;
    }
    if (!ok && error.empty()) error = "cannot read the voice list";
    LeaveCriticalSection(&lock_);
    return ok;
}

bool EngineClient::speakOnce(const SpeakParams& params,
                             const std::vector<SpeakSegment>& segments,
                             const SpeakSink& sink, std::string& error, bool& cancelled)
{
    cancelled = false;

    std::vector<char> payload;
    FlexVoiceSpeakRequest req = {};
    req.voiceIndex = params.voiceIndex;
    req.language = params.language;
    req.sampleRate = params.sampleRate;
    req.paramMask = params.paramMask;
    memcpy(req.params, params.params, sizeof(req.params));
    req.wantWordEvents = params.wantWordEvents ? 1u : 0u;
    req.wantSentenceEvents = params.wantSentenceEvents ? 1u : 0u;
    req.segmentCount = static_cast<uint32_t>(segments.size());

    payload.insert(payload.end(), reinterpret_cast<char*>(&req),
                   reinterpret_cast<char*>(&req) + sizeof(req));
    for (size_t i = 0; i < segments.size(); ++i) {
        FlexVoiceSegmentHeader sh = { static_cast<uint32_t>(segments[i].kind),
                                      segments[i].value };
        if (segments[i].kind == FV_SEG_TEXT || segments[i].kind == FV_SEG_SPELL) {
            sh.value = static_cast<uint32_t>(segments[i].text.size());
        }
        payload.insert(payload.end(), reinterpret_cast<char*>(&sh),
                       reinterpret_cast<char*>(&sh) + sizeof(sh));
        if (sh.kind == FV_SEG_TEXT || sh.kind == FV_SEG_SPELL) {
            payload.insert(payload.end(), segments[i].text.begin(), segments[i].text.end());
        }
    }

    if (!sendMessage(FV_CMD_SPEAK, payload.data(), static_cast<uint32_t>(payload.size()))) {
        return false;
    }

    std::vector<unsigned char> buffer;
    for (;;) {
        FlexVoiceMessageHeader h = {};
        if (!readHeader(h)) return false;

        switch (h.type) {
        case FV_RESP_AUDIO: {
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
        case FV_RESP_BOOKMARK: {
            uint32_t id = 0;
            if (h.size < sizeof(id) || !readPayload(&id, sizeof(id))) return false;
            if (h.size > sizeof(id) && !skipPayload(h.size - sizeof(id))) return false;
            if (sink.bookmark && !sink.bookmark(id)) { cancelled = true; disconnect(); return true; }
            break;
        }
        case FV_RESP_WORD:
        case FV_RESP_SENTENCE: {
            FlexVoiceBoundary b = {};
            if (h.size < sizeof(b) || !readPayload(&b, sizeof(b))) return false;
            if (h.size > sizeof(b) && !skipPayload(h.size - sizeof(b))) return false;
            const auto& cb = (h.type == FV_RESP_WORD) ? sink.word : sink.sentence;
            if (cb && !cb(b.position, b.length)) { cancelled = true; disconnect(); return true; }
            break;
        }
        case FV_RESP_END:
            if (h.size) skipPayload(h.size);
            return true;
        case FV_RESP_ERROR: {
            std::string msg(h.size, '\0');
            if (h.size && !readPayload(&msg[0], h.size)) return false;
            error = msg.empty() ? "the engine reported an error" : msg;
            FV_LOG("client: host error: %s", error.c_str());
            return true;      // a reported error is a complete exchange
        }
        default:
            if (!skipPayload(h.size)) return false;
            break;
        }
    }
}

bool EngineClient::speak(const SpeakParams& params, const std::vector<SpeakSegment>& segments,
                         const SpeakSink& sink, std::string& error)
{
    EnterCriticalSection(&lock_);
    bool ok = false;
    // Two attempts, so a host that exited between utterances costs one retry
    // rather than one lost utterance.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(error)) break;
        bool cancelled = false;
        std::string localError;
        if (speakOnce(params, segments, sink, localError, cancelled)) {
            if (!localError.empty()) error = localError;
            ok = localError.empty();
            break;
        }
        disconnect();
        if (attempt == 1) {
            if (error.empty()) error = "lost the connection to the engine host";
            FV_LOG("client: speak failed after a retry");
        }
    }
    LeaveCriticalSection(&lock_);
    return ok;
}

bool EngineClient::shutdownServer()
{
    if (!isServerRunning()) return true;
    HANDLE pipe = CreateFileW(FLEXVOICE_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    FlexVoiceMessageHeader h = { FV_CMD_SHUTDOWN, 0 };
    DWORD written = 0;
    WriteFile(pipe, &h, sizeof(h), &written, nullptr);
    FlexVoiceMessageHeader reply = {};
    DWORD read = 0;
    ReadFile(pipe, &reply, sizeof(reply), &read, nullptr);
    CloseHandle(pipe);
    for (int i = 0; i < 40; ++i) {
        if (!isServerRunning()) return true;
        Sleep(50);
    }
    return false;
}

}  // namespace FlexVoice
