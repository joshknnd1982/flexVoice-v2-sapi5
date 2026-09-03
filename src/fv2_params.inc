// fv2_params.inc -- the measured parameter table, included by fv2_speaker.cpp.
//
// Every range here was found by sweeping the parameter and measuring the
// rendered audio, because Mindmaker documented a range for none of them and the
// engine accepts whatever you write. probe/sweep.py regenerates the evidence;
// the numbers quoted in the comments are from a 6-second reference sentence
// spoken by Julie at 16 kHz.
//
// Three keys are marked inert. tilt, singingPitchRate and speedWPM were swept
// from -5 to 100 and produced **bit-identical** audio at every value: same
// sample count, same RMS to four decimals. They are real keys in Mindmaker's
// files and are preserved on round-trip, but FlexVoice 2.0 does not read them.
// (FlexVoice 3.01 does respond to tilt, which is one more way the two engines
// differ.)

//   id                    tavKey                label
//     type        lo      hi      fallback  geometric  active
{ P_VOLUME, "volume", "Volume", PT_DOUBLE,
  0.0, 8.0, 5.8, false, true,
  "Loudness of the voice itself. Exactly linear: peak amplitude is "
  "proportional to this value, 0 is true silence, and above about 7.7 a "
  "loud sentence clips." },

{ P_SPEECH_RATE, "speechRate", "Speed", PT_DOUBLE,
  0.25, 4.0, 1.0, true, true,
  "How fast the voice speaks. Scales duration exactly and leaves pitch "
  "alone: a quarter speed took 24 seconds where normal took 6." },

{ P_DEFAULT_PITCH, "defaultPitch", "Pitch", PT_INT,
  50.0, 400.0, 170.0, true, true,
  "Base pitch in hertz. Mindmaker's own voices run from 90 for Bill and "
  "Julius to 275 for Jill." },

{ P_PITCH_MIN, "pitchMin", "Pitch floor", PT_INT,
  20.0, 300.0, 50.0, true, true,
  "Lowest pitch the voice may reach, in hertz. It only changes the sound "
  "once it rises above the base pitch." },

{ P_PITCH_MAX, "pitchMax", "Pitch ceiling", PT_INT,
  150.0, 1000.0, 500.0, true, true,
  "Highest pitch the voice may reach, in hertz. It only changes the sound "
  "once it falls below the base pitch, which flattens the voice." },

{ P_PITCH_RATE, "pitchRate", "Pitch scale", PT_DOUBLE,
  0.5, 2.0, 1.0, true, true,
  "Multiplies the whole pitch contour without changing the speed." },

{ P_INTONATION, "intonationLevel", "Intonation", PT_DOUBLE,
  0.0, 4.0, 1.0, false, true,
  "How much the pitch moves over a sentence. Zero is a flat monotone." },

{ P_HEADSIZE, "headsize", "Head size", PT_DOUBLE,
  0.5, 2.0, 1.0, true, true,
  "Size of the vocal tract. Smaller sounds younger and thinner, and also "
  "shortens the speech slightly." },

{ P_RICHNESS, "richness", "Richness", PT_DOUBLE,
  0.0, 2.0, 0.99, false, true,
  "Richness of the voice source. At zero the voice thins out by about "
  "13 decibels." },

{ P_SMOOTHNESS, "smoothness", "Smoothness", PT_DOUBLE,
  0.0, 2.0, 0.78, false, true,
  "Smoothing of the voice source. It is loudest at both ends of its range "
  "and quietest in the middle." },

{ P_FRICATION, "fricationRate", "Frication", PT_DOUBLE,
  0.0, 3.0, 0.2, false, true,
  "Energy in fricatives such as S and F. Above about half its range a "
  "sibilant sentence starts to clip." },

{ P_PLOSIVE, "plosiveRate", "Plosive", PT_DOUBLE,
  0.0, 3.0, 1.2, false, true,
  "Energy in the bursts of P, T and K." },

{ P_VOLUME_SMOOTH, "volumeSmoothWindow", "Volume smoothing", PT_INT,
  0.0, 10.0, 1.0, false, true,
  "Width of the loudness smoothing window. The effect is small but real." },

// --- keys the engine ignores; kept so voice files round-trip --------------

{ P_TILT, "tilt", "Tilt", PT_DOUBLE,
  0.0, 4.0, 0.0, false, false,
  "Spectral tilt. FlexVoice 2.0 ignores it." },

{ P_SINGING_PITCH_RATE, "singingPitchRate", "Singing pitch scale", PT_DOUBLE,
  0.0, 2.0, 0.63, false, false,
  "Pitch scaling for sung notes. FlexVoice 2.0 ignores it." },

{ P_SPEED_WPM, "speedWPM", "Words per minute", PT_INT,
  50.0, 500.0, 250.0, false, false,
  "Nominal speed in words per minute. FlexVoice 2.0 ignores it; Speed is "
  "the control that works." },
