// fv2_trace -- log every file the FlexVoice 2.0 engine tries to open.
//
// createEngine() faults on a null pointer loaded from an engine object rather
// than reporting what it could not find, so the only way to see what is
// missing is to watch the file system calls. The engine is statically linked
// against its CRT and imports exactly one file-opening function from
// KERNEL32 -- CreateFileA -- so patching that one slot in its import address
// table produces the complete list, in order, with the failures marked.

#include "../src/ttsapi/fv2.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

using namespace MM_TTSAPI;

namespace {

typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
CreateFileA_t g_realCreateFileA = nullptr;
int g_opened = 0, g_failed = 0;

HANDLE WINAPI hookCreateFileA(LPCSTR name, DWORD access, DWORD share,
                              LPSECURITY_ATTRIBUTES sa, DWORD disp,
                              DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_realCreateFileA(name, access, share, sa, disp, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE) {
        ++g_failed;
        printf("  [open] FAILED (%lu)  %s\n", GetLastError(), name ? name : "(null)");
    } else {
        ++g_opened;
        printf("  [open] ok            %s\n", name ? name : "(null)");
    }
    return h;
}

// Replace one function pointer in a module's import address table.
bool patchIAT(HMODULE mod, const char* dllName, const char* funcName,
              void* replacement, void** original)
{
    auto* b = (unsigned char*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)b;
    auto* nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(b + imp->Name);
        if (_stricmp(dll, dllName) != 0) continue;

        auto* thunk = (IMAGE_THUNK_DATA*)(b + imp->FirstThunk);
        auto* orig = (IMAGE_THUNK_DATA*)(b + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                     : imp->FirstThunk));
        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(b + orig->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, funcName) != 0) continue;

            DWORD old = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            *original = (void*)thunk->u1.Function;
            thunk->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

class NullSite : public IWaveOutputSite
{
public:
    NullSite() : fmt_(16000, 16, WaveOutputFormat::WC_PCM_SIGNED) {}
    void put(unsigned char*, unsigned int size) override { bytes_ += size; }
    const IOutputFormat& getOutputFormat() const override { return fmt_; }
    void pause() override {}
    void play()  override {}
    void clear() override {}
    void setBookmark(Bookmark* b) override { delete b; }
    void sendBookmark(Bookmark&) override {}
    void registerNotify(INotify*, const BookmarkTypeList&) override {}
    void unregisterNotify(INotify*) override {}
    void addBookmarkTypes(INotify*, const BookmarkTypeList&) override {}
    void removeBookmarkTypes(INotify*, const BookmarkTypeList&) override {}
    unsigned bytes_ = 0;
private:
    WaveOutputFormat fmt_;
};

Engine* doCreate(EngineFactory* f, IOutputSite* s, Speaker* sp, int lang)
{
    std::auto_ptr<Engine> e = f->createEngine(s, *sp, (Language)lang);
    return e.release();
}

int guardedCreate(EngineFactory* f, IOutputSite* s, Speaker* sp, int lang, Engine** out)
{
    __try {
        *out = doCreate(f, s, sp, lang);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (argc < 3) {
        printf("usage: fv2_trace <dataRoot> <tav> [language]\n");
        return 2;
    }
    const char* root = argv[1];
    const char* tav  = argv[2];
    const int lang = argc > 3 ? (int)strtol(argv[3], nullptr, 0) : 0;

    HMODULE mod = GetModuleHandleA("FlexVoice_2_00_010.dll");
    printf("engine %s at %p\n", getVersion(), (void*)mod);

    if (!patchIAT(mod, "KERNEL32.dll", "CreateFileA",
                  (void*)&hookCreateFileA, (void**)&g_realCreateFileA)) {
        printf("could not patch CreateFileA -- no trace available\n");
        return 1;
    }
    printf("CreateFileA hooked\n\n");

    printf("-- EngineFactory(\"%s\")\n", root);
    EngineFactory* factory = nullptr;
    try { factory = new EngineFactory(root); }
    catch (...) { printf("factory THREW\n"); return 1; }

    printf("\n-- Speaker::load(\"%s\")\n", tav);
    std::vector<unsigned char> raw(sizeof(Speaker) + 4096, 0xCD);
    Speaker* sp = new (raw.data()) Speaker();
    try { sp->load(tav); }
    catch (...) { printf("speaker load THREW\n"); return 1; }

    printf("\n-- createEngine(language=%d)\n", lang);
    NullSite site;
    Engine* eng = nullptr;
    const int rc = guardedCreate(factory, &site, sp, lang, &eng);

    printf("\n%d opened, %d failed; createEngine %s\n",
           g_opened, g_failed, rc == 0 ? "OK" : "FAULTED");

    if (rc == 0 && eng && argc > 4) {
        printf("\n-- speaking\n");
        eng->addFragment(argv[4]);
        eng->speakRequest(1);
        eng->wait();
        printf("   %u bytes of audio\n", site.bytes_);
    }
    return rc;
}
