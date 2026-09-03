// speaker_test -- the .tav reader/writer, on its own.
//
// FlexVoice 2.0 has no attribute interface, so every parameter the wrapper and
// the configuration utility can change reaches the engine as a generated .tav.
// That makes this parser load-bearing: if it drops a field, the voice changes;
// if it spins or faults on a malformed file, the configuration utility hangs or
// dies while a screen-reader user is in it.
//
// Links against nothing but the parser, so it runs without the engine present.

#include "../src/fv2_speaker.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void checkNear(double a, double b, const char* what)
{
    ++g_checks;
    if (std::fabs(a - b) > 1e-6) {
        ++g_failures;
        std::printf("  FAIL  %s (%.10g vs %.10g)\n", what, a, b);
    }
}

const char* kJulie =
    "str gender=\"female\";\r\n"
    "str age=\"adult\";\r\n"
    "str voiceDescr=\"Julie\";\r\n"
    "str durationDescr=\"_T21\";\r\n"
    "str pitchDescr=\"\";\r\n"
    "str pitchQDescr=\"\";\r\n"
    "str volumeDescr=\"_L\";\r\n"
    "int volumeSmoothWindow=1;\r\n"
    "dbl volume=5.8;\r\n"
    "dbl speechRate=1.0;\r\n"
    "int speedWPM=250;\r\n"
    "int defaultPitch=170;\r\n"
    "int pitchMin=50;\r\n"
    "int pitchMax=500;\r\n"
    "dbl pitchRate=1.0;\r\n"
    "dbl singingPitchRate=0.63;\r\n"
    "dbl intonationLevel=1.0;\r\n"
    "dbl headsize=1.0;\r\n"
    "dbl tilt=0.0;\r\n"
    "dbl richness=0.99;\r\n"
    "dbl smoothness=0.78;\r\n"
    "dbl fricationRate=0.2;\r\n"
    "dbl plosiveRate=1.2;\r\n"
    "obj:Filter vsNoiseFilter\r\n"
    "{\r\n"
    "    dbl d1=0.1;\r\n"
    "    dbl d2=0.1;\r\n"
    "    dbl f1=1000;\r\n"
    "    dbl f2=7000;\r\n"
    "}\r\n"
    "obj:Filter vsHarmFilter\r\n"
    "{\r\n"
    "    dbl d1=0.1;\r\n"
    "    dbl d2=0.3;\r\n"
    "    dbl f1=1800;\r\n"
    "    dbl f2=4300;\r\n"
    "}\r\n"
    "str vsShapeFileName=\"JulieVsShape\";\r\n"
    "obj:VSNoiseParams vsNoiseParams\r\n"
    "{\r\n"
    "    dbl noisePeakAmpl = 0.3;\r\n"
    "    dbl noiseStartPos = 0.7;\r\n"
    "    dbl noiseFloor = 0.0;\r\n"
    "}\r\n"
    "obj:Equalizer equalizer[3]\r\n"
    "<\r\n"
    "    obj { 31.000000  ; 0.500000; 0.67 ; },\r\n"
    "    obj { 39.018975  ; 0.500000; 1.44 ; },\r\n"
    "    obj { 7750.000000; 0.166667; 3    ; }\r\n"
    ">\r\n";

void testParse()
{
    std::printf("parse a complete speaker\n");
    fv2::SpeakerDef d;
    std::string err;
    check(d.parse(kJulie, err), "parses");

    check(d.gender == "female", "gender");
    check(d.age == "adult", "age");
    check(d.voiceDescr == "Julie", "voiceDescr");
    check(d.durationDescr == "_T21", "durationDescr");
    check(d.volumeDescr == "_L", "volumeDescr");
    check(d.pitchDescr.empty(), "pitchDescr empty");
    check(d.vsShapeFileName == "JulieVsShape", "vsShapeFileName");

    checkNear(d.get(fv2::P_VOLUME), 5.8, "volume");
    checkNear(d.get(fv2::P_DEFAULT_PITCH), 170, "defaultPitch");
    checkNear(d.get(fv2::P_PITCH_MIN), 50, "pitchMin");
    checkNear(d.get(fv2::P_PITCH_MAX), 500, "pitchMax");
    checkNear(d.get(fv2::P_RICHNESS), 0.99, "richness");
    checkNear(d.get(fv2::P_SMOOTHNESS), 0.78, "smoothness");
    checkNear(d.get(fv2::P_FRICATION), 0.2, "fricationRate");
    checkNear(d.get(fv2::P_PLOSIVE), 1.2, "plosiveRate");
    checkNear(d.get(fv2::P_SPEED_WPM), 250, "speedWPM");
    checkNear(d.get(fv2::P_VOLUME_SMOOTH), 1, "volumeSmoothWindow");
    checkNear(d.get(fv2::P_SINGING_PITCH_RATE), 0.63, "singingPitchRate");

    check(d.vsNoiseFilter.present, "vsNoiseFilter present");
    checkNear(d.vsNoiseFilter.f2, 7000, "vsNoiseFilter.f2");
    check(d.vsHarmFilter.present, "vsHarmFilter present");
    checkNear(d.vsHarmFilter.d2, 0.3, "vsHarmFilter.d2");
    check(d.vsNoiseParams.present, "vsNoiseParams present");
    checkNear(d.vsNoiseParams.startPos, 0.7, "noiseStartPos");

    check(d.equalizer.size() == 3, "3 equalizer bands");
    if (d.equalizer.size() == 3) {
        checkNear(d.equalizer[0].f0, 31.0, "band0 f0");
        checkNear(d.equalizer[0].gain, 0.67, "band0 gain");
        checkNear(d.equalizer[2].f0, 7750.0, "band2 f0");
        checkNear(d.equalizer[2].bw, 0.166667, "band2 bw");
        checkNear(d.equalizer[2].gain, 3.0, "band2 gain");
    }
}

void testRoundTrip()
{
    std::printf("round-trip through serialize()\n");
    fv2::SpeakerDef a, b;
    std::string err;
    check(a.parse(kJulie, err), "first parse");

    const std::string text = a.serialize();
    check(!text.empty(), "serialize produces output");
    check(b.parse(text, err), "re-parse our own output");

    check(a.gender == b.gender, "gender survives");
    check(a.voiceDescr == b.voiceDescr, "voiceDescr survives");
    check(a.durationDescr == b.durationDescr, "durationDescr survives");
    check(a.volumeDescr == b.volumeDescr, "volumeDescr survives");
    check(a.vsShapeFileName == b.vsShapeFileName, "vsShapeFileName survives");
    for (int i = 0; i < fv2::P_COUNT; ++i) {
        checkNear(a.get((fv2::ParamId)i), b.get((fv2::ParamId)i),
                  fv2::kParams[i].tavKey);
    }
    check(a.equalizer.size() == b.equalizer.size(), "band count survives");
    for (size_t i = 0; i < a.equalizer.size() && i < b.equalizer.size(); ++i) {
        checkNear(a.equalizer[i].f0, b.equalizer[i].f0, "band f0 survives");
        checkNear(a.equalizer[i].bw, b.equalizer[i].bw, "band bw survives");
        checkNear(a.equalizer[i].gain, b.equalizer[i].gain, "band gain survives");
    }
    checkNear(a.vsHarmFilter.f1, b.vsHarmFilter.f1, "harm filter survives");
    checkNear(a.vsNoiseParams.peakAmpl, b.vsNoiseParams.peakAmpl, "noise params survive");
}

void testTableOrder()
{
    std::printf("parameter table matches the enum\n");
    // kParams is indexed by ParamId, so a row inserted in the wrong place
    // silently relabels every parameter after it -- the utility would show
    // "Pitch" and write headsize. The id in each row is the check.
    for (int i = 0; i < fv2::P_COUNT; ++i) {
        check(fv2::kParams[i].id == (fv2::ParamId)i, "row id matches its index");
        check(fv2::kParams[i].tavKey != nullptr && fv2::kParams[i].tavKey[0] != '\0',
              "row has a .tav key");
        check(fv2::kParams[i].label != nullptr && fv2::kParams[i].label[0] != '\0',
              "row has a label");
        check(fv2::kParams[i].help != nullptr && fv2::kParams[i].help[0] != '\0',
              "row has help text");
        check(fv2::kParams[i].hi > fv2::kParams[i].lo, "row range is non-empty");
        check(fv2::kParams[i].fallback >= fv2::kParams[i].lo &&
              fv2::kParams[i].fallback <= fv2::kParams[i].hi,
              "row default is inside its range");
    }
    // The active parameters must be a prefix, because the utility builds its
    // controls by walking 0..P_ACTIVE_COUNT.
    for (int i = 0; i < fv2::P_ACTIVE_COUNT; ++i) {
        check(fv2::kParams[i].active, "parameter before P_ACTIVE_COUNT is active");
    }
    for (int i = fv2::P_ACTIVE_COUNT; i < fv2::P_COUNT; ++i) {
        check(!fv2::kParams[i].active, "parameter after P_ACTIVE_COUNT is inert");
    }

    // No duplicated .tav keys -- two rows writing the same key would make the
    // last one win and the other control do nothing.
    for (int i = 0; i < fv2::P_COUNT; ++i) {
        for (int j = i + 1; j < fv2::P_COUNT; ++j) {
            check(std::strcmp(fv2::kParams[i].tavKey, fv2::kParams[j].tavKey) != 0,
                  "no duplicate .tav keys");
        }
    }
}

void testPercentMapping()
{
    std::printf("percent <-> value mapping\n");
    for (int i = 0; i < fv2::P_COUNT; ++i) {
        const fv2::ParamId id = (fv2::ParamId)i;
        const fv2::ParamInfo& p = fv2::kParams[i];

        // 0% must be the minimum and 100% the maximum -- this is the whole
        // contract the configuration utility presents to the user.
        checkNear(fv2::percentToValue(id, 0.0), p.type == fv2::PT_INT
                      ? std::floor(p.lo + 0.5) : p.lo, "0% is the minimum");
        checkNear(fv2::percentToValue(id, 100.0), p.type == fv2::PT_INT
                      ? std::floor(p.hi + 0.5) : p.hi, "100% is the maximum");

        // And the mapping must be invertible in between.
        for (double pct = 5.0; pct < 100.0; pct += 15.0) {
            const double v = fv2::percentToValue(id, pct);
            const double back = fv2::valueToPercent(id, v);
            if (p.type == fv2::PT_DOUBLE) {
                checkNear(back, pct, p.tavKey);
            } else {
                // An integer parameter quantises, so the round trip is only as
                // good as one step -- and how wide a step is in PERCENT depends
                // on the scale. On a linear scale it is constant
                // (volumeSmoothWindow spans 0..10, so a step is a tenth of it).
                // On a geometric scale it is widest at the bottom: one hertz at
                // defaultPitch's floor of 50 is a far bigger fraction of the
                // curve than one hertz at its ceiling of 400.
                double stepPct;
                if (p.geometric && p.lo > 0.0 && v > 0.0) {
                    stepPct = 100.0 * std::log((v + 1.0) / v) / std::log(p.hi / p.lo);
                } else {
                    stepPct = 100.0 / (p.hi - p.lo);
                }
                check(std::fabs(back - pct) <= stepPct, p.tavKey);
            }
        }
        check(fv2::percentToValue(id, -50.0) == fv2::percentToValue(id, 0.0),
              "below 0% clamps");
        check(fv2::percentToValue(id, 500.0) == fv2::percentToValue(id, 100.0),
              "above 100% clamps");
    }
}

void testMalformed()
{
    std::printf("malformed input terminates\n");
    // Every one of these must return rather than spin or fault: a .tav can be
    // hand-edited, and the configuration utility parses whatever it finds.
    const char* bad[] = {
        "",
        "garbage",
        "str",
        "str gender",
        "str gender=",
        "str gender=\"unterminated",
        "dbl volume=;",
        "dbl volume=1.0",                       // no terminator
        "obj:Filter f {",                       // unterminated block
        "obj:Filter f { dbl d1=1.0;",
        "obj:Equalizer e[3] <",                 // unterminated array
        "obj:Equalizer e[3] < obj { 1; 2; 3; },",
        "obj:Equalizer e[] <>",
        "obj",
        "obj:",
        "obj:Equalizer e[999999999] <>",
        "<<<<>>>>",
        "{{{{}}}}",
        ";;;;;;;;",
        "str gender=\"f\"; \x01\x02\x03 dbl volume=1.0;",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fv2::SpeakerDef d;
        std::string err;
        d.parse(bad[i], err);        // must simply return
        ++g_checks;                  // reaching here is the assertion
    }
    std::printf("  %u malformed inputs all returned\n",
                (unsigned)(sizeof(bad) / sizeof(bad[0])));
}

void testShippedFiles(int argc, char** argv)
{
    if (argc < 2) return;
    std::printf("Mindmaker's own files under %s\n", argv[1]);
    const char* names[] = { "Default", "Julie", "Bill", "Jill", "Julius", "Kit" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const std::string path = std::string(argv[1]) + "\\" + names[i] + ".tav";
        fv2::SpeakerDef a, b;
        std::string err;
        if (!a.loadFile(path, err)) {
            std::printf("  skip %s (%s)\n", names[i], err.c_str());
            continue;
        }
        check(b.parse(a.serialize(), err), names[i]);
        for (int k = 0; k < fv2::P_COUNT; ++k) {
            checkNear(a.get((fv2::ParamId)k), b.get((fv2::ParamId)k), fv2::kParams[k].tavKey);
        }
        check(a.equalizer.size() == b.equalizer.size(), "band count");
        std::printf("  %-8s %u bands, gender=%s age=%s\n", names[i],
                    (unsigned)a.equalizer.size(), a.gender.c_str(), a.age.c_str());
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testParse();
    testRoundTrip();
    testTableOrder();
    testPercentMapping();
    testMalformed();
    testShippedFiles(argc, argv);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
