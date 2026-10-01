# Credits

## The engine

**FlexVoice 2.0** was made by **Mindmaker Ltd.** of Budapest, Hungary, and
released in 2001. `FlexVoice_2_00_010.dll` in this project is Mindmaker's
binary, unmodified, dated 2 April 2001. The voice data — the Julie diphone
database and the five speaker files built on it — is likewise theirs.

Mindmaker closed long ago. FlexVoice has not been sold, supported or updated
for over twenty years, and no successor company has claimed it. It is treated
here as abandonware: this project adds a modern interface to software nobody
is selling, and makes no claim to the engine itself.

Named in the engine's own binaries and documentation:

* Mindmaker Ltd., Budapest — the engine, the voices, and the FlexVoice SDK
* The FlexVoice 2.0 help file and `setup.inf` scripts, which is where the
  install-time construction of `RHL2.dat` and `Julie.bin` was found

## This wrapper

The SAPI 5 wrapper, the engine host, the `.tav` reader and writer, the
configuration utility and the installer are by **Josh Kennedy**, 2026, and are
released under the MIT License ([LICENSE](LICENSE)), except for the COM server
skeleton derived from Gozaltech's BestSpeech SAPI 5 wrapper, which keeps
Gozaltech's notice ([NOTICE.md](NOTICE.md)).

It descends from the same author's earlier SAPI 5 wrappers — bestSpeech, and
then FlexVoice 3.01 — and carries their architecture forward: the 32-bit host
process, the framed-pipe protocol, the accessibility-first configuration
dialog, and the registry handling. The uninstall behaviour in particular exists
because the FlexVoice 3.01 wrapper got it wrong in a way that broke every other
SAPI 5 voice on the machine; that story is in the README.

## What is not here

The engine and its voice data are not in this repository. They are Mindmaker's
work, they were never open source, and the installer on the Releases page is
where they live.
