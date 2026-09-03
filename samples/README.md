# FlexVoice 2 sample renders

Every file here was rendered through the same path the shipping
wrapper uses: parameters written into a generated `.tav`, handed to
the engine's `Speaker::load`. 16 kHz, 16-bit, mono.

Text, unless noted: *The quick brown fox jumps over the lazy dog. She sells sea shells by the sea shore.*

| File | What it is |
|---|---|
| `01-voices/01-Julie.wav` | Julie, at its measured level trim |
| `01-voices/02-Bill.wav` | Bill, at its measured level trim |
| `01-voices/03-Jill.wav` | Jill, at its measured level trim |
| `01-voices/04-Julius.wav` | Julius, at its measured level trim |
| `01-voices/05-Kit.wav` | Kit, at its measured level trim |
| `02-parameters/volume-000.wav` | volume at 0% |
| `02-parameters/volume-025.wav` | volume at 25% |
| `02-parameters/volume-050.wav` | volume at 50% |
| `02-parameters/volume-075.wav` | volume at 75% |
| `02-parameters/volume-100.wav` | volume at 100% |
| `02-parameters/speechRate-000.wav` | speechRate at 0% |
| `02-parameters/speechRate-025.wav` | speechRate at 25% |
| `02-parameters/speechRate-050.wav` | speechRate at 50% |
| `02-parameters/speechRate-075.wav` | speechRate at 75% |
| `02-parameters/speechRate-100.wav` | speechRate at 100% |
| `02-parameters/defaultPitch-000.wav` | defaultPitch at 0% |
| `02-parameters/defaultPitch-025.wav` | defaultPitch at 25% |
| `02-parameters/defaultPitch-050.wav` | defaultPitch at 50% |
| `02-parameters/defaultPitch-075.wav` | defaultPitch at 75% |
| `02-parameters/defaultPitch-100.wav` | defaultPitch at 100% |
| `02-parameters/pitchMin-000.wav` | pitchMin at 0% |
| `02-parameters/pitchMin-025.wav` | pitchMin at 25% |
| `02-parameters/pitchMin-050.wav` | pitchMin at 50% |
| `02-parameters/pitchMin-075.wav` | pitchMin at 75% |
| `02-parameters/pitchMin-100.wav` | pitchMin at 100% |
| `02-parameters/pitchMax-000.wav` | pitchMax at 0% |
| `02-parameters/pitchMax-025.wav` | pitchMax at 25% |
| `02-parameters/pitchMax-050.wav` | pitchMax at 50% |
| `02-parameters/pitchMax-075.wav` | pitchMax at 75% |
| `02-parameters/pitchMax-100.wav` | pitchMax at 100% |
| `02-parameters/pitchRate-000.wav` | pitchRate at 0% |
| `02-parameters/pitchRate-025.wav` | pitchRate at 25% |
| `02-parameters/pitchRate-050.wav` | pitchRate at 50% |
| `02-parameters/pitchRate-075.wav` | pitchRate at 75% |
| `02-parameters/pitchRate-100.wav` | pitchRate at 100% |
| `02-parameters/intonationLevel-000.wav` | intonationLevel at 0% |
| `02-parameters/intonationLevel-025.wav` | intonationLevel at 25% |
| `02-parameters/intonationLevel-050.wav` | intonationLevel at 50% |
| `02-parameters/intonationLevel-075.wav` | intonationLevel at 75% |
| `02-parameters/intonationLevel-100.wav` | intonationLevel at 100% |
| `02-parameters/headsize-000.wav` | headsize at 0% |
| `02-parameters/headsize-025.wav` | headsize at 25% |
| `02-parameters/headsize-050.wav` | headsize at 50% |
| `02-parameters/headsize-075.wav` | headsize at 75% |
| `02-parameters/headsize-100.wav` | headsize at 100% |
| `02-parameters/richness-000.wav` | richness at 0% |
| `02-parameters/richness-025.wav` | richness at 25% |
| `02-parameters/richness-050.wav` | richness at 50% |
| `02-parameters/richness-075.wav` | richness at 75% |
| `02-parameters/richness-100.wav` | richness at 100% |
| `02-parameters/smoothness-000.wav` | smoothness at 0% |
| `02-parameters/smoothness-025.wav` | smoothness at 25% |
| `02-parameters/smoothness-050.wav` | smoothness at 50% |
| `02-parameters/smoothness-075.wav` | smoothness at 75% |
| `02-parameters/smoothness-100.wav` | smoothness at 100% |
| `02-parameters/fricationRate-000.wav` | fricationRate at 0% |
| `02-parameters/fricationRate-025.wav` | fricationRate at 25% |
| `02-parameters/fricationRate-050.wav` | fricationRate at 50% |
| `02-parameters/fricationRate-075.wav` | fricationRate at 75% |
| `02-parameters/fricationRate-100.wav` | fricationRate at 100% |
| `02-parameters/plosiveRate-000.wav` | plosiveRate at 0% |
| `02-parameters/plosiveRate-025.wav` | plosiveRate at 25% |
| `02-parameters/plosiveRate-050.wav` | plosiveRate at 50% |
| `02-parameters/plosiveRate-075.wav` | plosiveRate at 75% |
| `02-parameters/plosiveRate-100.wav` | plosiveRate at 100% |
| `02-parameters/volumeSmoothWindow-000.wav` | volumeSmoothWindow at 0% |
| `02-parameters/volumeSmoothWindow-025.wav` | volumeSmoothWindow at 25% |
| `02-parameters/volumeSmoothWindow-050.wav` | volumeSmoothWindow at 50% |
| `02-parameters/volumeSmoothWindow-075.wav` | volumeSmoothWindow at 75% |
| `02-parameters/volumeSmoothWindow-100.wav` | volumeSmoothWindow at 100% |
| `03-rate-and-volume/rate-0.25x.wav` | engine speech rate multiplier 0.25x |
| `03-rate-and-volume/rate-0.5x.wav` | engine speech rate multiplier 0.5x |
| `03-rate-and-volume/rate-1x.wav` | engine speech rate multiplier 1x |
| `03-rate-and-volume/rate-2x.wav` | engine speech rate multiplier 2x |
| `03-rate-and-volume/rate-4x.wav` | engine speech rate multiplier 4x |
| `03-rate-and-volume/volume-0.25x.wav` | engine volume multiplier 0.25x |
| `03-rate-and-volume/volume-0.5x.wav` | engine volume multiplier 0.5x |
| `03-rate-and-volume/volume-1x.wav` | engine volume multiplier 1x |
| `04-robustness/numerals.wav` | Chapter 5. The year 1984. It costs 3.50 dollars. |
| `04-robustness/bare-digits.wav` | 1234567890 |
| `04-robustness/punctuation.wav` | Hello, world! Really? Yes -- absolutely; of course. |
| `04-robustness/symbols.wav` | Tom & Jerry, 100% done, a < b > c, x \| y. |
| `04-robustness/bracket-command.wav` | [:rate 200] this is spoken, not obeyed. |
| `04-robustness/backslash.wav` | A backslash \ is spoken, not an escape. |
| `04-robustness/mixed-case.wav` | NASA and ASCII and Wi-Fi and iPhone. |
| `04-robustness/long-word.wav` | supercalifragilisticexpialidocious |
| `04-robustness/accented.wav` | cafe naive resume Zoe |
| `04-robustness/single-letter.wav` | a |

Regenerate with `python probe/make_samples.py`.
