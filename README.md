# Make an adaptor
- KeyIn: G13-Tip G15-Ring (to paddle)
- KeyOut: G3-Tip G6-Ring (to radio)
- Debug: G4-Tx G5-Rx (not required for nornal use)

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

## Current Step 1 UI Status

Mini-CW is currently locking the Cardputer UI layout and ownership boundary
before continuing service-owned keyer/audio behavior.

Current boot behavior:

- initializes `platform_hal`, `ui_service`, `audio_service`, `keyer_service`,
  `storage_service`, and `cw_trainer_service`
- logs the service initialization order
- shows the fixed 240x135 Mini-CW demo screen through `ui_service`
- uses a field-based top row: `[mode:13] [tone:3] [vol:2]`
- uses five 20-character middle display/menu lines
- uses a cyan-on-black bottom row: `[KeyIn:8] [KeyOut:8] [KeyInWPM:2]`
- polls the Cardputer ADV TCA8418 keyboard through the UI/input owner
- keeps global and local menu view state inside `ui_service`

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
- Screen text uses text size 2 for 12x16 cells and 20 visible characters.
- `app_core` calls only public `ui_service` APIs. Fixed layout rendering stays
  private in `ui_screen`, and low-level display/keyboard calls stay private in
  `ui_cardputer_port`.

Menu controls:

- `Ctrl`: enter/exit global menu.
- `Fn`: enter/exit local mode-specific menu.
- `1` to `5`: select visible menu item, currently stubbed.
- Up/Down paging uses the Mini-FT8-style `;` / `.` key mapping for now.
- Left/Right value changes use the Mini-FT8-style `,` / `/` key mapping for
  now and are currently stubbed.

Expected normal demo screen:

```text
Keyer         700 40
CQ CQ DE AG6AQ
BUF:
KEYIN:Paddle
KEYOUT:Paddle
READY
Paddle   Paddle   20
```

Current limitations:

- FATFS, `setting.txt`, USB mass storage, persistence, real paddle/straight-key
  I/O, full keyer behavior, and service-owned bottom status remain future work.
- Audio/keyer services still exist, but the Step 1 UI demo values are framework
  values until the next ownership pass connects service-owned data.

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
