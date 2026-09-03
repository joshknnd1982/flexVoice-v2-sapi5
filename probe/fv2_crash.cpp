// fv2_crash -- find out WHERE createEngine dies.
//
// EngineFactory is lazy: it accepts a nonsense path without complaint and
// defers all loading to createEngine, which then faults rather than throwing
// when something it needs is missing. That makes "it crashed" useless on its
// own, so this catches the fault and reports the address as an RVA inside
// FlexVoice_2_00_010.dll, which can be looked up against the export table and
// the DLL's string references.

#include "../src/ttsapi/fv2.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

using namespace MM_TTSAPI;

namespace {

class NullSite : public IWaveOutputSite
{
public:
    NullSite() : fmt_(16000, 16, WaveOutputFormat::WC_PCM_SIGNED) {}
    void put(unsigned char*, unsigned int) override { ++puts_; }
    const IOutputFormat& getOutputFormat() const override
    {
        printf("    [site] getOutputFormat\n");
        return fmt_;
    }
    void pause() override {}
    void play()  override {}
    void clear() override {}
    void setBookmark(Bookmark* b) override { delete b; }
    void sendBookmark(Bookmark&) override {}
    void registerNotify(INotify*, const BookmarkTypeList&) override {}
    void unregisterNotify(INotify*) override {}
    void addBookmarkTypes(INotify*, const BookmarkTypeList&) override {}
    void removeBookmarkTypes(INotify*, const BookmarkTypeList&) override {}
    int puts_ = 0;
private:
    WaveOutputFormat fmt_;
};

HMODULE g_engineModule = nullptr;

void describe(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* at = ep->ExceptionRecord->ExceptionAddress;
    printf("\n*** FAULT 0x%08lx at %p\n", (unsigned long)code, at);
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        printf("    %s address 0x%p\n",
               ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
               (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    HMODULE mods[256];
    DWORD needed = 0;
    // Which module is the faulting address in?
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(at, &mbi, sizeof(mbi))) {
        char name[MAX_PATH] = {0};
        GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, MAX_PATH);
        printf("    module %s base %p  ->  RVA 0x%06x\n",
               name[0] ? name : "(unknown)", mbi.AllocationBase,
               (unsigned)((char*)at - (char*)mbi.AllocationBase));
    }
    (void)mods; (void)needed;
}

// The auto_ptr return value needs unwinding, which __try forbids in the same
// frame, so the call lives here and the guard lives one frame out.
Engine* doCreate(EngineFactory* factory, IOutputSite* site, Speaker* speaker, int lang)
{
    std::auto_ptr<Engine> eng = factory->createEngine(site, *speaker, (Language)lang);
    return eng.release();
}

// POD-only frame so __try/__except is legal here.
int tryCreate(EngineFactory* factory, IOutputSite* site, Speaker* speaker, int lang)
{
    __try {
        Engine* eng = doCreate(factory, site, speaker, lang);
        printf("    createEngine OK -> %p\n", (void*)eng);
        return eng ? 0 : 1;
    }
    __except (describe(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (argc < 3) {
        printf("usage: fv2_crash <dataRoot> <tav> [language]\n");
        return 2;
    }
    const char* root = argv[1];
    const char* tav = argv[2];
    const int lang = argc > 3 ? (int)strtol(argv[3], nullptr, 0) : 0;

    g_engineModule = GetModuleHandleA("FlexVoice_2_00_010.dll");
    printf("engine %s, module base %p\n", getVersion(), (void*)g_engineModule);
    printf("root=%s\n tav=%s\n lang=%d\n\n", root, tav, lang);

    EngineFactory* factory = nullptr;
    try {
        factory = new EngineFactory(root);
        printf("factory ok\n");
    } catch (...) {
        printf("factory THREW\n");
        return 1;
    }

    std::vector<unsigned char> raw(sizeof(Speaker) + 4096, 0xCD);
    Speaker* sp = new (raw.data()) Speaker();
    try {
        sp->load(tav);
        printf("speaker loaded\n");
    } catch (...) {
        printf("speaker load THREW\n");
        return 1;
    }

    NullSite site;
    printf("calling createEngine...\n");
    const int rc = tryCreate(factory, &site, sp, lang);
    printf("result: %d\n", rc);
    return rc;
}
