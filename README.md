# FlexVoice 2 SAPI 5

Mindmaker's **FlexVoice 2.0** speech synthesiser, from 2001, as ordinary Windows
SAPI 5 voices — usable from NVDA, JAWS, Narrator, Balabolka or anything else
that speaks SAPI 5. Both 32-bit and 64-bit. No SAPI 4 runtime, no ActiveX, no
browser plugin, and nothing in the registry that the engine itself has to read.

Six voices, a configuration utility that exposes every speech parameter the
engine actually responds to, and an uninstall that leaves the machine exactly
as it found it.

**[Download the installer from the Releases page](https://github.com/joshknnd1982/flexxVoice-v2-sapi5/releases).**
The engine and its voice data are not in this repository; the installer carries
them.

> **This is not the same engine as [flexVoice-sapi5](https://github.com/joshknnd1982/flexVoice-sapi5).**
> That project wraps FlexVoice **3.01**. FlexVoice 2.0 is an earlier and
> genuinely different engine: a different API, a different voice-file format,
> and different behaviour. The two can be installed side by side.

---

## What FlexVoice is

FlexVoice is a *hybrid* synthesiser: it concatenates recorded diphones, but it
does the concatenation in LPC parameter space rather than in the waveform. That
design is why the engine is tiny and startlingly fast — it renders about
**ninety times faster than real time** on a modern machine — and why it has so
many knobs. Because every segment is a parameter set rather than a fixed piece
of audio, head size, richness, frication and the rest can be moved after the
fact without the artefacts a waveform-domain synthesiser would produce.

Mindmaker Ltd. of Budapest closed long ago and FlexVoice has not been sold for
over twenty years. See [CREDITS.md](CREDITS.md).

### How it differs from FlexVoice 3.01

Anyone who has used the 3.01 wrapper will notice these:

| | FlexVoice 2.0 | FlexVoice 3.01 |
|---|---|---|
| Numerals | **spoken correctly** | faulted or wedged the engine |
| Embedded commands in client text | none reachable — text is spoken literally | backslash was an escape character |
| Parameter interface | the `.tav` voice file only | a named get/set attribute API |
| `tilt` | ignored | works |
| Equalizer | 24 bands | 12 bands |
| Voices | 5, one diphone database | 2 databases, 3 speaker files |

The first two are the reasons this wrapper's text handling is so much smaller
than 3.01's had to be: nothing has to be escaped, and numbers are left exactly
as the caller wrote them, which keeps every word-boundary offset where it
belongs.

---

## Voices

| Voice | Gender | Age | Base pitch | What it is |
|---|---|---|---|---|
| FlexVoice2 Custom Voice | — | — | configurable | every parameter comes from the configuration utility |
| FlexVoice2 Julie | Female | Adult | 170 Hz | the voice the diphone database was recorded from |
| FlexVoice2 Bill | Male | Adult | 90 Hz | Julie's recordings at a lower pitch and a larger vocal tract |
| FlexVoice2 Jill | Female | Child | 275 Hz | a small vocal tract and a slower speaking rate |
| FlexVoice2 Julius | Male | Adult | 90 Hz | a second male adult, softer in its plosives than Bill |
| FlexVoice2 Kit | Male | Child | 115 Hz | the most frication of the five |

Being honest about what is what:

* All five share **one** recorded voice. FlexVoice 2.0's Julie voice pack
  contains a single diphone database, `Julie.bin`. Bill, Jill, Julius and Kit
  are Mindmaker's own presets over it — different pitch, head size, richness and
  prosody models — not different recordings. That is the point of a parametric
  synthesiser, and it is also why the Custom Voice can sound like none of them.
* The pack ships **six** `.tav` files, but `Default.tav` and `Julie.tav` are
  byte-identical, so shipping both would put one voice in the list twice.
* Every voice carries a **measured level trim**. Rendering a demanding sentence
  at each voice's own shipped `volume` gives peaks from 31285 to 54590 against a
  ceiling of 32767: four of the five clip out of the box, Jill by 3.5 dB. The
  trims put every voice at about −2 dBFS, which also means switching voices does
  not jump the level.

Sample renders of every voice, of every parameter across its range, and of the
inputs that were tried against the engine to see what breaks it, are in
[`samples/`](samples/).

---

## Languages

**English only, and that is not a limitation of this wrapper.**

FlexVoice 2.0's `Language` argument is an enumeration, and English is `0` — a
value that appears nowhere in the engine binary and had to be read out of the
call site inside Mindmaker's own SAPI 4 layer, `FVWrapper.dll`. Unlike 3.01, the
2.0 engine exports no `getLangID` or `getLangName`, so it cannot even be asked
what else it knows.

What is certain is that the only language data that survives for FlexVoice 2.0
is English. The engine's data directory holds one letter-to-sound table, one
part-of-speech model, one text-normalisation table and one phoneme set, all
English. No Hungarian, Czech or Malay pack for the 2.0 engine is known to exist,
and the 3.01 packs are not compatible.

The configuration utility still shows the language list, with the one entry it
honestly has.

---

## Speech parameters

Thirteen parameters, each exposed in the configuration utility as a whole
percentage where **0 is the minimum the engine supports and 100 the maximum**.

Mindmaker documented a range for none of them, and the engine accepts whatever
you write. So every range below was found by sweeping the parameter and
measuring the rendered audio — duration, level, peak and clipping. See
[`probe/sweep.py`](probe/sweep.py).

| Parameter | `.tav` key | Range | Scale | What moves |
|---|---|---|---|---|
| Volume | `volume` | 0 – 8 | linear | the voice's own loudness; peak amplitude is exactly proportional, 0 is true silence |
| Speed | `speechRate` | 0.25 – 4.0× | geometric | duration only, exactly; **no pitch change** |
| Pitch | `defaultPitch` | 50 – 400 Hz | geometric | base F0, in hertz |
| Pitch floor | `pitchMin` | 20 – 300 Hz | geometric | hard floor on the contour; only audible once it rises above the base pitch |
| Pitch ceiling | `pitchMax` | 150 – 1000 Hz | geometric | hard ceiling; a low one flattens the voice |
| Pitch scale | `pitchRate` | 0.5 – 2.0× | geometric | the whole F0 contour; duration unchanged |
| Intonation | `intonationLevel` | 0.0 – 4.0 | linear | F0 variance; 0 is a monotone |
| Head size | `headsize` | 0.5 – 2.0 | geometric | vocal tract scale; also shortens speech slightly |
| Richness | `richness` | 0.0 – 2.0 | linear | voice source richness; at 0 the voice thins by ~13 dB |
| Smoothness | `smoothness` | 0.0 – 2.0 | linear | voice source smoothing; loudest at both ends of its range |
| Frication | `fricationRate` | 0.0 – 3.0 | linear | energy in S and F; above ~50% a sibilant sentence clips |
| Plosive | `plosiveRate` | 0.0 – 3.0 | linear | energy in the bursts of P, T and K |
| Volume smoothing | `volumeSmoothWindow` | 0 – 10 | linear | loudness smoothing window; a small but real effect |

### Three parameters that do nothing

`tilt`, `singingPitchRate` and `speedWPM` are real keys in Mindmaker's voice
files, and this wrapper preserves them when it reads and writes one. But the
FlexVoice 2.0 engine **ignores all three**: swept from −5 to 100, every value
produced bit-identical audio — same sample count, same RMS to four decimal
places. So the configuration utility offers no controls for them. A slider that
provably does nothing is worse than no slider, particularly when it is being
read aloud.

(`tilt` does work in FlexVoice 3.01. It is one more way the two engines differ.)

### The equalizer and the voice source

Each `.tav` also carries a 24-band equalizer, two filter blocks and a noise
model. These are read, preserved and written back untouched, so a voice file
round-trips exactly, but the utility does not expose 24 bands of EQ as 72 spin
boxes. All five voices round-trip through the parser to **byte-identical
audio**, which is the test that keeps that claim honest.

---

## The Custom Voice

A screen reader can offer rate, pitch and volume. FlexVoice has thirteen
parameters that do something, and that gap is why this project exists.

**FlexVoice2 Custom Voice** is a SAPI voice whose parameters come from the
configuration utility instead of from a file. Pick it in NVDA (or anywhere
else), then open **FlexVoice 2 Configuration** and change whatever you like:
changes are saved as you make them and take effect on the very next thing
spoken, with no restart.

The utility is built for a screen reader first:

* Every control is created in tab order and has an explicit accessible name,
  spelling out that the value is a percentage and which end is which.
* Values are **edit boxes with spin buttons, never sliders**. MSAA reports a
  trackbar's position as a percentage of its range, so a 0-to-9 slider announces
  5 as "55". A spin buddy announces the number you actually set.
* Each control also carries a description saying what the parameter does.
* Nothing is modal and there is no OK/Cancel: closing the window cannot lose a
  setting.
* **Speak it** previews either the Custom Voice as you have it, or any of
  Mindmaker's five exactly as the SAPI voice of that name would sound.

The other five voices are unaffected by the utility. If they were not, every
one of them would collapse onto the same sound.

---

## How it is put together

```
NVDA / JAWS / Narrator / Balabolka
        |  SAPI 5
        v
FlexVoice2SAPI.dll  (32-bit and 64-bit)
        |  named pipe, framed messages
        v
fv2_host.exe  (32-bit, one per session)
        |  C++ API
        v
FlexVoice_2_00_010.dll  (Mindmaker, 2001, x86)
```

The engine is a 32-bit DLL, so a 64-bit SAPI client could not load it in any
case. But **both** bitnesses go through the host, for a second reason: this
engine can fault on input, and a fault inside a screen reader's process would
take the screen reader down with it. The host is cheap to restart. NVDA is not.

One engine is kept warm for the life of the host. Measured end to end, client
through pipe through host through engine:

| | |
|---|---|
| First audio, 25 short utterances each cancelled | median **4.6 ms**, worst 7.3 ms |
| Cold start: launch the host, load the engine, first utterance | 153 ms |
| Switch voices | 4.4 ms |
| Stop speaking | 0.3 ms |
| Rendering speed | ~90× real time |

The first row is the one that matters: it is the gap between pressing an arrow
key and the machine starting to talk. It is measured with
`QueryPerformanceCounter`, because `GetTickCount`'s 15.6 ms granularity reports
a flat "15 ms" for everything in that range.

What keeps it there: the host is started in the background as soon as a voice is
selected rather than on the first keystroke; the client holds its pipe open and
retries a dropped connection immediately before backing off; rate and volume are
engine-level multipliers that never rewrite a voice file; and re-selecting a
voice whose parameters have not changed does nothing at all.

---

## Uninstalling

Uninstalling removes everything, including the parts that are not files.

This is called out because the earlier FlexVoice 3.01 wrapper got it wrong, and
the way it went wrong is worth knowing about. `DllUnregisterServer` removed each
voice token with `RegDeleteKeyW`, which refuses to delete a key that has
subkeys — and every SAPI voice token has an `Attributes` subkey, because SAPI
requires one. Every removal failed, silently, inside a `catch (...)`. So
uninstalling deleted the DLL and left the voice tokens behind, naming a CLSID
that no longer resolved. SAPI 5's voice list is machine-wide and shared: a
client would list voices it could not create, and because NVDA remembers its
chosen voice by token path, a user whose voice was one of those could not start
the SAPI 5 synthesiser **at all** — for any voice, from any vendor.

Here, deleting a registry key always means deleting the subtree, and three
independent mechanisms remove the registration, none of which can take the
others down with it:

1. `DllUnregisterServer`, via the installer's `regserver` flag.
2. `[Registry]` entries flagged `uninsdeletekey dontcreatekey`, recorded in the
   uninstall log — these do not care whether `regsvr32` ever ran.
3. A sweep in the uninstaller's own code that matches tokens by their
   `FlexVoice2_` prefix, in **both** registry views, so a voice renamed in a
   later release is caught too.

No parent key is ever touched — `Speech\Voices\Tokens` is the machine's voice
list, and `RegDeleteTreeW` with an empty subkey name would empty it. A
default-voice pointer aimed at one of our voices is cleared rather than left
dangling, in every loaded user hive, since the person who installed this need
not be the person about to lose their speech.

`test/sapi_probe.cpp` checks all of it against the **shipping** registration
code, not helpers of its own — that is exactly how 3.01's test missed the bug —
and plants another vendor's token first to confirm the sweep leaves it alone.

---

## When it does not speak

The configuration utility says on startup whether the engine host is running.
Beyond that:

* **Diagnostic logs** are in `%APPDATA%\FlexVoice2SAPI\logs`, one file per
  component (`sapi32`, `sapi64`, `host`, `config`), plus the installer's own log
  in the program folder. The utility has an **Open log folder** button. Logging
  of lifecycle and errors is on by default; per-utterance detail is a separate
  checkbox, because it writes a line for every phrase spoken.
* **A self-test** ships with the host. From the program folder:

  ```
  fv2_host.exe --self-test
  ```

  It checks every data file the engine needs, then loads each voice and speaks
  with it. The installer runs this automatically and says so if it fails.

* **"Engine data is incomplete: RHL2.dat, Julie.bin"** means the installer could
  not build them — see below. Reinstalling rebuilds them; if security software
  is blocking `FVZip.exe`, allow it and reinstall.

### Two files the installer builds

FlexVoice 2.0 never shipped `RHL2.dat` (the letter-to-sound table) or
`Julie.bin` (the diphone database). Mindmaker's own setup generated them from
`CHL2.dat` and `Julie.cod` using `FVZip.exe`, and this installer does the same:

```
FVZip.exe chl2rhl { CHL2.dat RHL2.dat }
FVZip.exe vq2bin  { Julie.cod Julie.bin }
```

The braces are literally part of FVZip's command line. Without them it exits 6
and writes nothing. Without the files, the engine faults on a null pointer
inside `createEngine` and reports nothing at all, which is a memorable
afternoon.

---

## Building from source

Needs Visual Studio 2022 (or the Build Tools), CMake, and Inno Setup 6.

```
build_all.bat
```

That builds both architectures, stages `output\`, and compiles the installer.

The engine itself is **not** in this repository — it is Mindmaker's, and it was
never open source. To build, put `FlexVoice_2_00_010.dll` in `engine\` and the
voice data in `engine\Data\` (with the speaker files in `engine\Data\Voices\`).
`build_all.bat` generates the import library from the DLL's own export table;
FlexVoice 2.0 shipped no `.lib`.

### Tests

| | |
|---|---|
| `speaker_test` | the `.tav` reader and writer, standalone — no engine needed |
| `engine_test` | the engine layer: voices, switching, bookmarks, stop, multipliers |
| `client_test` | the whole pipe path, and the keystroke latency figures above |
| `sapi_probe` | through real SAPI, registered into HKCU so it needs no admin |
| `a11y_probe` | walks the configuration dialog through MSAA |
| `probe/sweep.py` | regenerates the parameter measurements in the table above |

---

## Licence

The wrapper is under the licence in [LICENSE](LICENSE). The FlexVoice 2.0 engine
and its voice data are Mindmaker Ltd.'s and are not covered by it; see
[CREDITS.md](CREDITS.md).
