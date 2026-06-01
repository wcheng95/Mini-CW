# Mini-CW Development Notes

Project: Mini-CW
Target hardware: M5Stack Cardputer ADV class devices

Mini-CW is a standalone CW/Morse keyer and trainer. The current codebase has a
working service architecture, real Cardputer display/keyboard integration, an
audio CW output path, keyer input handling, and four LCWO-inspired trainer
modes: Lessons, Words, Callsigns, and Plain Text.

## Current Architecture

Mini-CW uses a top-down ESP-IDF component model:

```text
main
  |
  +-- app_core
        |
        +-- ui_service
        +-- audio_service
        +-- keyer_service
        +-- cw_trainer_service
        +-- storage_service
        +-- platform_hal
```

`app_core` owns application flow and routes high-level UI events to the service
that owns the affected state. Feature code and trainer modes must not directly
access hardware.

## Ownership Rules

| Resource or state | Owner |
| --- | --- |
| Application mode/routing | `app_core` |
| Display and keyboard UI behavior | `ui_service` |
| Fixed 240 x 135 rendering | private `ui_screen` helper |
| Cardputer display/keyboard port | private `ui_cardputer_port` helper |
| Speaker, sidetone, CW playback timing | `audio_service` |
| Paddle/straight-key mode and keyer WPM | `keyer_service` |
| Trainer configs, sessions, scoring, playback requests | `cw_trainer_service` |
| Persistence and files | `storage_service` |
| Battery, sleep, board/platform services | `platform_hal` |

Important boundary:

```text
ui_service interprets input and emits ui_input_event_t.
app_core applies non-UI changes by calling the owning service.
```

This keeps `ui_service` from becoming the application controller. UI code may
read public service state for rendering, but it must not call non-UI mutators or
storage APIs directly.

## Current Modes

Startup mode is Keyer. Mode select is opened with Opt and currently displays:

```text
1 Keyer
2 Lessons
3 Words
4 Calls
5 Plain
6 System
```

Internal app/UI enums still keep PlainText at enum value 0 for historical
compatibility, but the visible order is Keyer first and System last.

## Trainer Service Layout

`cw_trainer_service.h` remains the only public trainer API used outside the
component. The implementation is split by mode:

```text
components/cw_trainer_service/
  cw_trainer_service.c              coordinator and public wrappers
  include/cw_trainer_service.h      public API
  private_include/
    cw_trainer_internal.h           private shared helpers/constants
    cw_lesson_mode.h
    cw_word_mode.h
    cw_callsign_mode.h
    cw_plaintext_mode.h
  cw_lesson_mode.c
  cw_word_mode.c
  cw_callsign_mode.c
  cw_plaintext_mode.c
```

Each mode file owns its own private config, result, session state, copy buffer,
selection logic, and scoring. The coordinator initializes modes and dispatches
public `cw_trainer_*` calls to the private mode implementation.

## Implemented Trainer Modes

### Lessons

Koch-style receive practice. The trainer generates copy from the active lesson
character set, sends it at configured code/effective WPM, accepts typed copy,
and scores accuracy.

Default config:

```text
lesson=1
duration_min=1
code_wpm=20
effective_wpm=12
group_len=0 (random)
```

### Words

25-word adaptive attempts using a compact built-in English word bank. Candidate
words are filtered by required Koch lesson and max word length. Correct answers
raise current WPM up to 40 and add score. Wrong answers lower WPM down to 5.
Replay sends the current word at its recorded sent speed.

Default config:

```text
start_wpm=20
min_char_wpm=10
lesson=40
max_word_len=15
```

### Callsigns

25-callsign adaptive attempts using a prototype built-in callsign bank copied
from LCWO-style data. Correct answers raise current WPM up to configured MaxWPM
and score using the WPM actually sent for that callsign. Wrong answers lower WPM
down to 5. Replay sends the current callsign at its recorded sent speed.

Default config:

```text
start_wpm=20
min_char_wpm=10
max_wpm=40
```

Prototype note: the static callsign bank is intentionally temporary. Replace it
with procedural generation or FATFS-loaded `callsigns.txt` before public
release.

### Plain Text

One-message receive practice using a compact built-in plaintext bank. Playback
uses configured code/effective WPM. On submit, target and copy are normalized
case-insensitively, whitespace is collapsed, and Levenshtein distance produces
accuracy in tenths of a percent.

Default config:

```text
code_wpm=20
effective_wpm=12
```

Prototype note: the static plaintext bank is intentionally temporary. Replace
it with FATFS-loaded `plaintext.txt` or larger SQL-derived collections later.

## UI Event Model

`ui_service` emits explicit events for user intent:

```text
UI_INPUT_EVENT_MODE_CHANGED
UI_INPUT_EVENT_VOLUME_CHANGED
UI_INPUT_EVENT_KEY_IN_WPM_CHANGED
UI_INPUT_EVENT_KEY_IN_MODE_CHANGED
UI_INPUT_EVENT_LESSON_CONFIG_CHANGED
UI_INPUT_EVENT_WORD_CONFIG_CHANGED
UI_INPUT_EVENT_CALLSIGN_CONFIG_CHANGED
UI_INPUT_EVENT_PLAINTEXT_CONFIG_CHANGED
UI_INPUT_EVENT_SELECT
UI_INPUT_EVENT_CHAR_INPUT
UI_INPUT_EVENT_BACKSPACE
UI_INPUT_EVENT_REPLAY
UI_INPUT_EVENT_CANCEL
UI_INPUT_EVENT_SLEEP_REQUEST
```

`ui_input_event_t` carries the key, setting target, value, and delta. `app_core`
handles these events and calls `audio_service`, `keyer_service`,
`cw_trainer_service`, `storage_service`, or `platform_hal` as appropriate.

## Storage Status

`storage_service` exposes load/save APIs for Lessons, Words, Callsigns, and
PlainText configs/results:

```text
storage_lesson_*
storage_word_*
storage_callsign_*
storage_plaintext_*
```

These are currently disabled stubs. `app_core` already calls them at init,
settings changes, start, and result completion so a real backend can be added
without changing UI or trainer ownership.

## Hardware Notes

Audio hardware path from the Cardputer ADV work:

```text
Codec: ES8311 through esp_codec_dev
Format: 48000 Hz, mono, 16-bit PCM
I2C: SDA GPIO8, SCL GPIO9
I2S speaker TX: BCLK GPIO41, WS/LRCK GPIO43, DOUT GPIO42
MCLK: disabled, GPIO_NUM_NC, use_mclk=false
```

Adapter pins:

```text
KeyIn:  G13-Tip, G15-Ring
KeyOut: G3-Tip, G6-Ring
Debug:  G4-TX, G5-RX
```

## Build

Use the ESP-IDF flow:

```text
idf.py build
```

On the configured Windows development machine, the full ESP-IDF environment may
need to be exported first before running `idf.py`.

## Useful Ownership Checks

`ui_service.c` should not directly call non-UI mutators or storage APIs. Useful
checks:

```text
rg "audio_service_set_|audio_service_adjust_|audio_service_play_" components/ui_service/ui_service.c
rg "keyer_service_set_|keyer_service_adjust_|keyer_service_cycle_" components/ui_service/ui_service.c
rg "cw_trainer_.*set_config|storage_" components/ui_service/ui_service.c
```

Trainer private headers should stay inside `components/cw_trainer_service`.

## Current Limitations And Next Work

The current firmware is functional, but several pieces are still prototype or
stubbed:

```text
FATFS-backed settings/results persistence
USB mass-storage mode for editing user files
File-backed word, callsign, and plaintext lists
Procedural callsign generation
CW decoder display
QSO/exchange practice and logging
Long-term statistics/high scores
Public-release polish for built-in practice banks
```

When adding these, preserve the current ownership boundaries: storage owns file
access, trainer modes own training state and scoring, UI emits intent, and
`app_core` routes changes to the owning service.
