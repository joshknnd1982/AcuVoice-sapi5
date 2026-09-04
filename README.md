# AcuVoice SAPI5

A SAPI 5 speech engine for **AcuVoice**, the 1990s concatenative text-to-speech system
from AcuVoice, Inc. of San Jose, California. It gives the engine eight voices in the
Windows voice list, 32-bit and 64-bit, with a configuration utility that exposes every
parameter the engine actually has.

The original product spoke to Windows through SAPI 4, an interface Windows has not
shipped since XP. **Nothing here uses SAPI 4.** The 1999 SAPI 4 engine, `AcuEng.dll`, is
not installed and is not needed: the real engine underneath it, `avcore.dll`, is a plain
`__stdcall` C API that imports nothing but `kernel32` and `user32` — no COM, no type
library, no registry, no SAPI of any generation. This project drives that directly.

Download the installer from
[Releases](https://github.com/joshknnd1982/AcuVoice-sapi5/releases).

---

## What AcuVoice is

AcuVoice was founded in 1994 and built one of the first genuinely *concatenative*
synthesizers to run on a desktop PC. Where its contemporaries — DECtalk, Eloquence,
SoftVoice, the Klatt-family formant synthesizers — computed speech from a model of the
vocal tract, AcuVoice recorded a human being reading a very large corpus, cut it into
syllables and polysyllabic fragments, and reassembled them. That is why it still sounds
noticeably more human than most synthesizers of its own decade, and why the engine's data
files are 154 MB when a formant synthesizer of the same era fits in 300 KB.

AcuVoice, Inc. was acquired by **Fonix Corporation** in 1998. The version this project
was built against is **AcuVoice 3.02**, dated 1 July 1999. Fonix wound its speech
business down in the late 2000s; the product has not been sold or supported since, and is
abandonware.

### One voice, one language

This is worth saying plainly, because it is the first thing anyone asks.

The engine ships **one recorded voice** — a male American English speaker the SAPI 4
engine published as *"AcuVoice, Roger"* — and **one language**, US English. There is no
second voice hiding in the data files:

* `Ulaw08Sb\Hashfon1..4.cmp` look like four voices and are not. They are four hash
  buckets of a single sound bank; which one a syllable comes from is decided by the
  lookup, not by any voice selection. `Hashsnds.ply` is the fifth file of the same bank.
* The engine's tag language has a `\vce=` tag. The parser accepts it and stores the name;
  nothing in the engine ever reads it back.
* `AcuEng.dll`, the original SAPI 4 engine, declares exactly one mode: speaker `Roger`,
  manufacturer `Fonix`, product `AcuVoice`, style `Business`, language `American,
  English`, gender/age `Adult`.
* The long list of language names inside `avcore.dll` (`german-swiss`, `portuguese-
  brazilian`, and so on) is the Microsoft C runtime's `setlocale` table, not an AcuVoice
  one.

The wrapper therefore publishes Roger plus six presets built from him, and one Custom
Voice you shape yourself. They are all the same recorded speaker with different rate,
pitch, volume and pause settings — the voice list says so, and so does this paragraph.

---

## The voices

| Voice | What it is |
| --- | --- |
| **AcuVoice Roger** | The engine's own voice, exactly as AcuVoice shipped it: 175 words a minute, neutral pitch, the engine's own pause lengths. |
| **AcuVoice Roger Deep** | A fourth lower and a little slower. |
| **AcuVoice Roger Bright** | A fourth higher, at his usual pace. |
| **AcuVoice Roger Brisk** | 260 words a minute with shortened pauses, for reading rather than listening. |
| **AcuVoice Roger Measured** | 120 words a minute with longer pauses. |
| **AcuVoice Roger Announcer** | Slightly lower and louder, with broadcast pacing. |
| **AcuVoice Roger Clipped** | Fast with the pauses taken out, for skimming. |
| **AcuVoice Custom Voice** | Every parameter comes from the configuration utility, re-read on every utterance. |

All eight are registered under `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens`, in both
the 32-bit and 64-bit views, so every SAPI 5 client sees them: NVDA, JAWS, Narrator,
Balabolka, TextAloud, Windows' own Speech settings.

---

## The engine's parameters, and which of them the engine applies

`avcore.dll` has a tag language: control sequences written inline in the text between
backslashes. Reverse-engineering the parser gave the full list and every range it
enforces. **The important finding is in the last column.**

| Tag | Range | Default | Meaning | Applied by |
| --- | --- | --- | --- | --- |
| `\spd=` | 85 – 350 | 175 | Speaking rate in words per minute | **the wrapper** |
| `\pit=` | 45 – 91 | 63 | Pitch; the scale is ±6 semitones around 63 | **the wrapper** |
| `\vol=` | 0 – 65535 | 32767 | Volume | **the wrapper** |
| `\rspd=` | 50 – 200 | 100 | Rate as a percentage of the current one | **the wrapper** |
| `\Rpit=` | 70 – 145 | 100 | Pitch as a percentage of the current one | **the wrapper** |
| `\pau=` | ≥ 0 | — | Insert a pause, in milliseconds | the engine |
| `\mrk=` | any integer | — | Bookmark; comes back out of `get_bookmark` | the engine |
| `\avprn=` | `word=phonemes` | — | Ad-hoc pronunciation for one word | the engine |
| `\chr=` | text | — | Character/spelling mode | the engine |
| `\ctx=` | `normal`, `business`, `calm`, `excited`, `monotone`, `emphatic`, `address`, `html`, `stock`, `unknown` | `normal` | Text type | parsed, then ignored |
| `\prt=` | `prop`, `pron`, `prep`, `interj`, `det`, `conj`, `aux`, `adv`, `adj`, `past`, `city`, `state`, `web`, `traffic`, `e-mail` | — | Part of speech for the next word | parsed, then ignored |
| `\emp\` | — | — | Emphasize the next word | parsed, then ignored |
| `\rst\` | — | — | Reset every parameter | parsed, then ignored |
| `\pro=`, `\rms=`, `\rmw=` | 0 or 1 | — | Front-end switches | parsed, then ignored |
| `\vce=`, `\eng=` | text | — | Voice and engine name | parsed, then ignored |

### Why the wrapper applies the prosody

`avcore.dll` parses `\spd=`, `\pit=` and `\vol=` out of the text, validates them against
the ranges above, and puts them on queues — and then never looks at them. Every rate,
every pitch and every volume renders **byte-identical audio**; the numbers come back out
of `speed_dequeue`, `pitch_dequeue` and `volume_dequeue` for the caller to act on. That
is not a bug and it is not a limitation of this wrapper: the 1999 SAPI 4 engine did
exactly the same thing, which is why `AcuEng.dll` imports all six dequeue functions and
carries its own `PitchConverter`, and why `Aspeed.dll` exists at all.

So this wrapper applies them, in `src/av_dsp.cpp`:

* **Rate** — WSOLA time scaling at the engine's own 8 kHz, following avcore's own
  words-per-minute curve so that 175 wpm is untouched audio, 85 doubles the duration and
  350 halves it.
* **Pitch** — cubic resampling, folded into the same pass that takes the engine's 8 kHz
  up to the 16 kHz handed to SAPI. Doing them together is why there is only one
  interpolation, and outputting at 16 kHz is what stops a raised pitch from folding the
  engine's 4 kHz band back on itself as aliasing.
* **Volume** — a linear scale with a `tanh` knee above about −2.7 dBFS, because an
  amplified vowel that hard-clips turns into a buzz.

`Aspeed.dll`, the engine's own rate helper, is not used: its whole-buffer entry point
`ChangeBufferSpeed16` access-violates for every factor except 1.0.

### The four pause lengths — the one thing the engine really does apply

| Setting | Default | What it is |
| --- | --- | --- |
| `PAUSE1` | 650 ms | After a sentence |
| `PAUSE2` | 500 ms | At a semicolon or colon |
| `PAUSE3` | 350 ms | At a comma |
| `PAUSE4` | 7 ms | Between words |

These are real: they change what the engine synthesizes, because it is the engine that
inserts the silence. They are settable at run time through `set_pausation`, which the
configuration utility and every voice preset use.

### Text handling

None of the number, date, money or abbreviation expansion is the wrapper's work. AcuVoice
has a large English front end of its own — the ordinals, `thousand` through
`quadrillion`, `o'clock`, the weekday names and abbreviations, a big suffix table, prefix
rules with phonetic respellings — and it does the job well. The wrapper narrows UTF-16
down to Windows-1252 (transliterating curly quotes, dashes, ligatures and the common
symbols rather than letting them become a spoken "question mark"), collapses control
characters, and otherwise gets out of the way.

---

## The configuration utility

`AcuVoiceConfig.exe` is a single keyboard-reachable window. Every control is a tab stop,
every control has an Alt shortcut, and every control has a static label immediately
before it in the dialog's own control order — which is where MSAA takes a control's name
from, so a screen reader reading a slider says what the slider is for.

Every slider runs **0 to 100**, and it means the same thing everywhere: **0 is the least
the engine will do and 100 is the most.** That is also what keeps the announcement
honest, because MSAA reports a trackbar as a percentage of its range, and for a 0–100
range that is the number on the slider.

* **Speaking rate** — 0 is 85 words a minute, 100 is 350.
* **Pitch** — 0 is six semitones below Roger, 50 is Roger, 100 is six above.
* **Volume** — 0 is silent, 50 is the recorded level, 100 is twice it.
* **Pause after a sentence / at a semicolon / at a comma** — 0 is none, 100 is three seconds.
* **Gap between words** — 0 is none, 100 is a hundred milliseconds.
* **Extra rate and extra volume for every voice** — applied on top of whatever the host
  asks for, so a screen reader's own rate slider keeps working.
* **Speak punctuation by name**, **obey AcuVoice control tags**, **use the custom user
  dictionary**, **write a diagnostic log**.
* **Start from one of the ready-made voices** — copies a preset's settings into the
  sliders as a starting point.
* **Speak it** — renders the test sentence with exactly the numbers the SAPI engine
  would compute for the Custom Voice, so the button tests what a screen reader will hear.
* **User dictionary…** — opens AcuVoice's own dictionary editor, which is installed into
  a directory the logged-on user can write to so it works without elevation.

Settings live in `HKCU\Software\AcuVoice SAPI5`, are re-read on every utterance, and take
effect on the next thing any program speaks — no restart.

If the voice ever ends up unintelligible and you cannot read the screen to fix it:

```bat
"C:\Program Files (x86)\AcuVoice SAPI5\AcuVoiceConfig.exe" /reset
```

---

## How it is put together

```
32-bit host (NVDA, JAWS, Balabolka x86)
    AcuVoiceSAPI.dll  ──►  avcore.dll                      in process

64-bit host (Narrator, NVDA x64, Balabolka x64)
    AcuVoiceSAPI.dll  ──►  AcuVoiceServer.exe (32-bit)  ──►  avcore.dll
                           over a named pipe
```

`avcore.dll` is 32-bit and there is no 64-bit build of it, so a 64-bit host reaches it
through a worker. The worker holds one copy of the engine, serves every 64-bit client,
and stays running between utterances — starting it costs a process launch and a pipe
connect, which is far too much to pay on a keystroke. Text preparation and the prosody
arithmetic both happen on the SAPI side, so the worker only ever receives finished
Windows-1252 bytes and three numbers, and hands back finished 16-bit PCM.

The two paths produce the same audio. Of the twelve wave files the diagnostics tool
writes through SAPI, nine are byte-identical between the 32-bit in-process path and the
64-bit worker path; the other three differ in exactly one sample out of a quarter of a
million, where an x87 and an SSE2 rounding split a WSOLA similarity tie differently.

### Responsiveness

The engine is very fast — about **1 ms of work per second of speech**. Measured on the
development machine:

```
short utterance    2.1 ms   for  2.6 s of audio
long  utterance    9.6 ms   for 14.0 s of audio
long, with rate, pitch and volume applied   24 ms
```

What that buys: a long fragment is split at sentence boundaries so the first audio starts
before the whole thing has been synthesized; audio reaches SAPI in 4 KB pieces with the
host's cancel flag checked between each, so stopping lands within an eighth of a second;
and the 64-bit worker sends 8 KB at a time and checks for a stop between chunks with a
non-blocking peek rather than a read, because a blocking read on a synchronous pipe
handle deadlocks against its own writer.

### Logging

Both the installer and the engine write logs.

* Setup: `%TEMP%\Setup Log *.txt` — Inno Setup's own, enabled in the script.
* Engine: `%LOCALAPPDATA%\AcuVoice SAPI5\acuvoice.log` — every utterance, the parameters
  it was given, the factors they turned into, how many bytes came back, and any engine
  status code. Each line carries the process name, its bitness and its pid, because one
  utterance from a 64-bit host crosses two processes. Capped at 4 MB with one rotation.
  Turn it off in the configuration utility, or with
  `HKCU\Software\AcuVoice SAPI5` → `Logging` = 0.

---

## What gets installed

```
C:\Program Files (x86)\AcuVoice SAPI5\
    AcuVoiceSAPI.dll            32-bit SAPI5 engine
    AcuVoiceServer.exe          32-bit worker for 64-bit hosts
    AcuVoiceConfig.exe          configuration utility
    AcuVoiceDiagnostics.exe     engine report, sample generator, self-test
    x64\AcuVoiceSAPI.dll        64-bit SAPI5 engine
    engine\Lib\avcore.dll       the engine
    engine\Ulaw08Sb\            the 154 MB recorded sound bank
    engine\UserDict\            AcuVoice's own dictionary editor

C:\ProgramData\AcuVoice SAPI5\
    Dictfls\                    the dictionary, writable so the editor works unelevated
    Temp\                       the engine's scratch directory

C:\Windows\acuvoice.ini         the only place avcore reads its directories from
```

`acuvoice.ini` has to be in the Windows directory: `avcore.dll` opens it by bare name
through `GetPrivateProfileStringA`, which Windows resolves against `%WINDIR%`, and there
is no other path it will look at. The installer gives that one file an ACL that lets the
logged-on user write to it, because `set_pausation` writes the pause lengths back to it.
If a copy of the original 1999 product is already installed, its `acuvoice.ini` is saved
as `acuvoice.ini.before-AcuVoiceSAPI5` first.

---

## The samples

`samples/` holds wave files that show what every voice and every parameter sounds like.
They are generated, not hand-picked — `AcuVoiceDiagnostics.exe all <dir>` writes the
whole set:

* `engine_02_speed_*` — 85, 120, 175, 260 and 350 words a minute.
* `engine_03_pitch_*` — 45, 54, 63, 77 and 91 on the engine's pitch scale.
* `engine_04_volume_*` — five points across 0 to 65535.
* `engine_05_pauses_*` — the same three sentences with no pauses, the engine's own, and
  long ones.
* `engine_06_voice_*` — each of the seven published voices.
* `engine_07`–`engine_14` — numbers, dates, money, abbreviations, punctuation, URLs and
  email, accented text, a long paragraph, spelling, and punctuation named aloud.
* `engine_15`–`engine_18` — the tag parser on and off, and the extremes of rate and pitch.
* `sapi_*` — every registered voice spoken through real SAPI 5, plus the host's own rate
  and volume controls at their extremes. These are the ones that prove the wrapper, not
  just the engine.

---

## Building

Needs Visual Studio 2022 Build Tools (C++ x86 and x64), CMake 3.15+, and Inno Setup 6 for
the installer.

```bat
build_all.bat
```

The AcuVoice engine files are **not in this repository** — see the licence note below.
Without `engine\Lib\avcore.dll` the build still produces working binaries; it just has
nothing for the installer to package.

To test the SAPI side without installing anything, `reg_test.exe register` writes the
voice tokens and the CLSID entries under `HKEY_CURRENT_USER`. SAPI's voice category is
rooted at `HKEY_LOCAL_MACHINE`, so those tokens do not appear in an enumeration — but
`ISpObjectToken::SetId` takes an explicit registry path, and the diagnostics tool falls
back to that, which makes the whole SAPI path testable without elevation.

### Layout

| Path | What it is |
| --- | --- |
| `src/av_engine.*` | the `avcore.dll` C API, the streaming synthesis loop, and the ini |
| `src/av_dsp.*` | WSOLA time scaling, cubic pitch resampling, mu-law expansion, gain |
| `src/text_prep.*` | UTF-16 to Windows-1252, spelling, punctuation, sentence splitting |
| `src/voices.hpp` | the voice table and the engine's parameter curves |
| `src/settings.hpp` | what the configuration utility saves and the engine re-reads |
| `src/ISpTTSEngineImpl.*` | the SAPI5 engine |
| `src/av_server.cpp` | the 32-bit worker |
| `src/pipe_*.{h,cpp}` | the wire protocol between them |
| `tools/acuvoice_config.*` | the configuration utility |
| `tools/acuvoice_diag.cpp` | the report, the sample generator and the self-test |
| `tools/av_probe.c`, `av_param.c`, `av_speed.c`, `av_pause.c` | the reverse-engineering probes this was built from, kept because they are the evidence for everything above |

---

## Licence

The wrapper — everything in `src/`, `tools/`, `installer/` and the build files — is MIT.
See [LICENSE.txt](LICENSE.txt).

The AcuVoice engine itself is not. `avcore.dll`, the sound bank, the dictionary and the
dictionary editor are © 1998–1999 AcuVoice, Inc. / Fonix Corporation. The product has not
been sold or supported for well over a decade and is abandonware. It is not in this
repository's source tree; it ships only in the release installer, for people who already
have a copy of something nobody sells any more. If you hold the rights and want the
binaries taken down, open an issue and they will be removed.
