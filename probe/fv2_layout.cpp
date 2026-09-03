// fv2_layout -- measure what the DLL actually builds, rather than trusting the
// reconstructed declarations.
//
// createEngine() segfaults with a header whose virtual ordering has been proven
// correct against the DLL's own vtables, so the next suspect is data layout:
// the engine calls getOutputFormat() and reads the sampling rate out of the
// WaveOutputFormat we hand it. If our fields sit at different offsets than the
// engine's do, it reads nonsense and dies -- with no bad pointer anywhere for a
// vtable check to catch.
//
// Constructing into a poisoned buffer shows exactly which bytes the DLL's own
// constructor writes, and where 16000 / 16 / 1 land.

#include "../src/ttsapi/fv2.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

using namespace MM_TTSAPI;

namespace {

void hexdump(const char* label, const unsigned char* p, size_t n)
{
    printf("%s (%u bytes)\n", label, (unsigned)n);
    for (size_t i = 0; i < n; i += 16) {
        printf("  +%04x  ", (unsigned)i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < n) printf("%02x ", p[i + j]); else printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; ++j) {
            const unsigned char c = p[i + j];
            putchar(c >= 32 && c < 127 ? c : '.');
        }
        printf("|\n");
    }
}

// Report every 4-byte little-endian word equal to `value`.
void findWord(const unsigned char* p, size_t n, unsigned int value, const char* what)
{
    for (size_t i = 0; i + 4 <= n; ++i) {
        unsigned int w;
        memcpy(&w, p + i, 4);
        if (w == value) printf("    %-22s found at +%u\n", what, (unsigned)i);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Unbuffered: this probe is expected to crash, and a lost buffer would
    // hide the very line that says where.
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("engine version = %s\n\n", getVersion());

    printf("as declared in fv2.hpp:\n");
    printf("  sizeof(WaveOutputFormat) = %u\n", (unsigned)sizeof(WaveOutputFormat));
    printf("  offsetof samplingFrequency = %u\n",
           (unsigned)offsetof(WaveOutputFormat, samplingFrequency));
    printf("  offsetof bitResolution     = %u\n",
           (unsigned)offsetof(WaveOutputFormat, bitResolution));
    printf("  offsetof waveCoding        = %u\n\n",
           (unsigned)offsetof(WaveOutputFormat, waveCoding));

    // ---- what the DLL's constructor really writes ---------------------------
    const size_t SLACK = 128;
    std::vector<unsigned char> raw(sizeof(WaveOutputFormat) + SLACK, 0xCD);
    WaveOutputFormat* fmt =
        new (raw.data()) WaveOutputFormat(16000, 16, WaveOutputFormat::WC_PCM_SIGNED);

    size_t touched = 0;
    for (size_t i = raw.size(); i-- > 0; ) {
        if (raw[i] != 0xCD) { touched = i + 1; break; }
    }
    printf("DLL constructor touched %u bytes\n", (unsigned)touched);
    hexdump("WaveOutputFormat raw", raw.data(), touched + 8 > raw.size() ? raw.size() : touched + 8);
    printf("  looking for the values we passed:\n");
    findWord(raw.data(), touched, 16000, "16000 (sample rate)");
    findWord(raw.data(), touched, 16, "16 (bit resolution)");
    findWord(raw.data(), touched, 1, "1 (WC_PCM_SIGNED)");

    printf("\n  reading through our declaration: sf=%d br=%d wc=%d\n",
           fmt->samplingFrequency, fmt->bitResolution, fmt->waveCoding);
    printf("  outputType() = %d  (WAVE should be %d)\n",
           (int)fmt->outputType(), (int)IOutputFormat::WAVE);

    fmt->~WaveOutputFormat();

    // ---- and the Speaker, for the record ------------------------------------
    printf("\nsizeof(Speaker) declared = %u\n", (unsigned)sizeof(Speaker));
    {
        std::vector<unsigned char> sraw(sizeof(Speaker) + 4096, 0xCD);
        Speaker* sp = new (sraw.data()) Speaker();
        size_t t = 0;
        for (size_t i = sraw.size(); i-- > 0; ) { if (sraw[i] != 0xCD) { t = i + 1; break; } }
        printf("  default-constructed Speaker touches %u bytes\n", (unsigned)t);
        sp->~Speaker();
    }

    // ---- EngineFactory ------------------------------------------------------
    //
    // This is the one that matters. If the real EngineFactory is bigger than
    // the buffer fv2.hpp declares for it, its constructor writes past the end
    // of our object -- and when that object is a stack local, the damage lands
    // on the return address of whatever called us. It would show up exactly as
    // it did: a crash that comes and goes with no relation to the input.
    if (argc > 1) {
        printf("\nsizeof(EngineFactory) declared = %u\n", (unsigned)sizeof(EngineFactory));
        std::vector<unsigned char> fraw(sizeof(EngineFactory) + 65536, 0xCD);
        EngineFactory* f = new (fraw.data()) EngineFactory(argv[1]);
        size_t touched = 0;
        for (size_t i = fraw.size(); i-- > 0; ) {
            if (fraw[i] != 0xCD) { touched = i + 1; break; }
        }
        printf("  constructed from %s\n", argv[1]);
        printf("  DLL constructor touches %u bytes%s\n", (unsigned)touched,
               touched > sizeof(EngineFactory) ? "   *** OVERFLOW ***" : "");
        f->~EngineFactory();
    } else {
        printf("\n(pass the data directory to measure EngineFactory)\n");
    }
    return 0;
}
