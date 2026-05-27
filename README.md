# Mini-CW

Mini-CW is an ESP-IDF project for a standalone CW/Morse trainer on M5Stack
 Cardputer ADV class devices.

The long-term goal is portable ham-radio practice with:

- RX practice: listen to generated Morse and type the copy
- TX practice: send with a straight key or paddle and receive feedback
- callsign, QSO, and SOTA/POTA-style exchange practice
- a few Morse games to help learning
- local profile, progress, and session logging

## Architecture

Mini-CW uses a top-down modular structure:

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

`app_core` owns the application state machine and initializes services. Services
own their feature boundaries. Hardware access is isolated behind the one module
that owns that resource.

## Hardware Ownership

Each hardware resource has exactly one owner:

| Hardware resource | Owner |
| --- | --- |
| Display | `ui_service` / private display port |
| Cardputer keyboard | `ui_service` / private input port |
| Speaker / tone output | `audio_service` |
| Paddle/key GPIO | `keyer_service` or its HAL owner |
| SD/SPIFFS/file access | `storage_service` |
| RTC/time | `platform_hal` |
| Battery/PMU | `platform_hal` |

`app_core` and trainer modules must not directly call GPIO, display, speaker,
SD/SPIFFS, RTC, or PMU APIs.

## Milestone 2 Status

Milestone 2 adds the first usable trainer path: pressing a Cardputer keyboard
key `A-Z` or `0-9` plays that character as Morse code through the speaker.
This is still a tone-test path, not a full lesson, scoring, or TX-practice mode.

Current boot behavior:

- initializes `platform_hal`, `ui_service`, `audio_service`, `keyer_service`,
  `storage_service`, and `cw_trainer_service`
- logs the service initialization order
- shows a Mini-CW Tone Test screen through `ui_service`
- keeps and displays the current app mode, last character, last Morse pattern,
  WPM, pitch, and status
- polls the Cardputer ADV TCA8418 keyboard through the UI/input owner
- routes character input as `ui_service -> app_core -> cw_trainer_service ->
  audio_service`
- plays CW through the `audio_service` speaker owner

Audio note:

- Cardputer ADV speaker output uses the copied `mic_test` `board_cardputer_adv`
  component, including the known-good ES8311 full-duplex codec setup.
- Mini-CW does not currently use the mic path.
- The Mini-CW audio output port only calls `board_audio_init()` and
  `board_audio_write()`; no app or trainer code touches codec/I2S APIs.
- Speaker defaults: 48000 Hz, mono, 16-bit PCM, no MCLK, I2C SDA/SCL GPIO8/9,
  I2S BCLK GPIO41, WS GPIO43, DOUT GPIO42.

UI note:

- Cardputer display and keyboard use the copied `mic_test` `M5Cardputer`,
  `M5GFX`, and `M5Unified` components through a private `ui_service` port.
- The display uses the verified `M5Cardputer.Display.setRotation(1)` path.
- Screen text follows the `mini_ft8_min` Cardputer convention: text size 2,
  six short 19-pixel lines.

Tone-test controls:

- `A-Z` or `0-9`: play the Morse character.
- `+` or `=`: increase WPM.
- `-`: decrease WPM.
- `[`: decrease pitch.
- `]`: increase pitch.
- Backtick, or ESC if available from the input backend: stop audio.

Timing:

- Default WPM: 20.
- Default pitch: 700 Hz.
- WPM bounds: 5 to 40.
- Pitch bounds: 400 to 1000 Hz.
- Dit length: `dit_ms = 1200 / WPM`.
- Dah length: 3 dits.
- Element gap: 1 dit.
- Character gap: 3 dits.
- Word gap: 7 dits.

Current limitations:

- Playback uses a simple one-slot audio task so the app loop can still process
  the stop key while a character is playing. A fuller queue remains future work.
- Farnsworth timing is stored but not applied yet.
- Paddle/key decoding, lessons, scoring, and persistence backends are still
  future milestones.

## Build

Use a normal ESP-IDF environment:

```powershell
idf.py set-target esp32s3
idf.py build
```

The exact target may change once the Cardputer ADV board support is selected.

## Planned Milestones

1. Project skeleton and hardware ownership boundaries.
2. CW tone engine and keyboard-to-Morse tone test.
3. Straight key and paddle input through `keyer_service`.
4. TX practice mode with decoded text and timing feedback.
5. RX practice mode with typed copy and scoring.
6. Callsign, QSO, SOTA/POTA exchange, and statistics modes.
7. 2 or 3 Morse games

See [develop_notes.md](develop_notes.md) for the current design notes.
