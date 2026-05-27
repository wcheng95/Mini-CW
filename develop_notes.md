# Mini-CW Development Notes

Project: Mini-CW
Target hardware: M5Stack Cardputer / Cardputer ADV class devices

Mini-CW is a standalone CW/Morse trainer for portable ham-radio practice. It is
intended to support receive practice, callsign copy, QSO/exchange practice, and
early transmit practice through a straight key or paddle.

## Architecture Direction

Mini-CW follows a top-down ESP-IDF architecture:

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

`app_core` owns application mode/state and service orchestration. Feature and
trainer modules must not directly access hardware.

## Hardware Ownership Rule

Each hardware resource has exactly one owner:

| Hardware resource | Owner |
| --- | --- |
| Display | ui_service / private UI port |
| Cardputer keyboard | ui_service / private input port |
| Speaker / tone output | audio_service |
| Paddle/key GPIO | keyer_service or keyer HAL owner |
| SD/SPIFFS/files | storage_service |
| RTC/time | platform_hal |
| Battery/PMU | platform_hal |

No app mode, trainer engine, or feature module should directly touch GPIO,
display, speaker, SD/SPIFFS, RTC, or PMU APIs outside its ownership boundary.

## Initial App Modes

```text
APP_MODE_TONE_TEST
APP_MODE_RX_PRACTICE
APP_MODE_TX_PRACTICE
APP_MODE_CALLSIGN
APP_MODE_QSO
APP_MODE_STATS
APP_MODE_MENU
```

## Milestone 1 Status

Milestone 1 is a buildable skeleton only.

Implemented:

- ESP-IDF project layout and component boundaries
- `app_core` state machine and service initialization order
- `ui_service` home/status screen stub and high-level input event API
- `audio_service` CW tone API stubs
- `keyer_service` key/paddle API stubs and keyer event types
- `cw_trainer_service` RX/TX placeholder session functions
- `storage_service` profile/log persistence stubs
- `platform_hal` board/platform init stub

Audio hardware note from `C:\MiniFT8-idf\mic_test\components\board_cardputer_adv`:

- Working codec path used ES8311 through `espressif/esp_codec_dev` v1.5.9.
- Working format was 48000 Hz, mono, 16-bit PCM.
- I2C pins: SDA GPIO8, SCL GPIO9.
- I2S speaker TX pins: BCLK GPIO41, WS/LRCK GPIO43, DOUT GPIO42.
- I2S mic RX was DIN GPIO46, but Mini-CW does not need mic input for now.
- MCLK was disabled: `GPIO_NUM_NC`, `use_mclk=false`.
- Mini-CW keeps this under `audio_service` via a private audio output port so
  no trainer or app module touches codec, I2S, I2C, or speaker APIs directly.

Not implemented in Milestone 1:

- Board-specific Cardputer display/keyboard drivers
- Real CW timing or Morse encoding
- Scoring, lessons, statistics, or training logic
- Real paddle/key GPIO decoding
- Real ES8311/I2S speaker output
- Persistent storage backend

## Milestone 2 Status

Milestone 2 implements the CW tone engine and keyboard-to-Morse test path.

Implemented:

- `APP_MODE_TONE_TEST` as the startup mode.
- Real Cardputer/Cardputer ADV ST7789 screen drawing under `ui_service`.
- Real Cardputer ADV TCA8418 keyboard polling under `ui_service`.
- Morse lookup for `A-Z` and `0-9` inside `audio_service`.
- Default CW settings: 20 WPM, 700 Hz pitch, 12 WPM Farnsworth placeholder.
- Safe bounds: WPM 5 to 40, pitch 400 to 1000 Hz.
- One-slot audio task behind `audio_cw_play_char()` and
  `audio_cw_play_text()`, keeping the app loop responsive enough for stop.
- ES8311/I2S speaker output through the private `audio_output_port`, with
  log/timing fallback if the backend is unavailable.
- High-level event routing:
  `ui_service -> app_core -> cw_trainer_service -> audio_service`.
- UI state showing last played character, Morse pattern, WPM, pitch, and status.
- Simple controls: `A-Z`/`0-9` play Morse, `+`/`-` adjust WPM, `[`/`]` adjust
  pitch, backtick/ESC requests stop.

Milestone 2 timing:

```text
dit_ms = 1200 / WPM
dit = 1 unit
dah = 3 units
element gap = 1 unit
character gap = 3 units
word gap = 7 units
```

Current limitations:

- Playback is a one-slot command path, not a full queue. A new character
  replaces any pending command and asks the current tone to stop.
- Farnsworth WPM is stored and logged but not applied to spacing yet.
- No lessons, scoring, copy checking, paddle decoding, or TX feedback yet.
- Storage APIs remain persistence stubs.

Hardware fix note:

- The display, keyboard, and speaker paths now reuse copied components from
  `C:\MiniFT8-idf\mic_test\components`.
- `ui_service` owns `M5Cardputer.beginDisplayOnly(true)`, display drawing, and
  `M5Cardputer.Keyboard.keysState()` polling.
- `ui_service` now uses the `mini_ft8_min` screen scale: text size 2 with
  short six-line screens.
- `audio_service` owns speaker output through `board_audio_init()` and
  `board_audio_write()`.
- `board_audio` still initializes the ES8311 in full-duplex mode internally,
  matching the working mic/speaker test, but Mini-CW does not expose mic input.

## Planned Milestones

1. Milestone 1: Project skeleton and hardware ownership boundaries.
2. Milestone 2: CW tone engine and keyboard-to-Morse tone test.
3. Milestone 3: Straight key and paddle input through `keyer_service`.
4. Milestone 4: TX practice mode with decoded text and timing feedback.
5. Milestone 5: RX practice mode with typed copy and scoring.
6. Milestone 6: Callsign, QSO, SOTA/POTA exchange, and statistics modes.

## Near-Term Notes

- Keep APIs small and explicit.
- Add board-specific code only under the owning service or HAL.
- Prefer stubs with clear logs over half-implemented training behavior.
- Avoid circular dependencies between services.
