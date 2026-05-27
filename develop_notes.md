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


## LCWO-Inspired Training Modes

Mini-CW will implement several LCWO-inspired practice modes, but each mode
should remain above the shared trainer/keyer/audio architecture. No mode should
directly own hardware.

Planned modes:

1. **Lessons**
   - Structured Koch-style character progression.
   - Each lesson enables a defined character set.
   - The trainer generates random groups or short copy items using only the
     active lesson characters.

2. **MorseMachine / Game Mode**
   - Character-by-character adaptive practice.
   - Characters answered incorrectly receive higher selection weight.
   - Characters answered correctly receive lower selection weight.
   - This may later become a game-style mode with score, streak, and lives.

3. **Plain Text Training**
   - Practice text should be preloaded from files.
   - Initial file model: one line equals one practice item.
   - Example file locations:

```text
/spiffs/text/beginner.txt
/spiffs/text/qso_short.txt
/spiffs/text/sota_pota.txt
/sdcard/MiniCW/text/beginner.txt
```

4. **Word Training**
   - Word lists should also be preloaded from files.
   - Initial file model: one word or phrase per line.
   - Example categories: common words, ham-radio words, QSO words, SOTA/POTA
     words.

5. **Callsign Training**
   - Support both preloaded callsign lists and on-the-fly generated callsigns.
   - Preloaded lists can include local calls, SOTA/POTA calls, and DX calls.
   - Generated callsigns should use realistic-looking amateur-radio patterns.
   - Local callsigns should receive extra weighting, especially prefixes useful
     for the user, such as W6/K6/N6/AG6/KO6/KI6.

Shared design rule:

```text
training mode -> cw_trainer_service -> keyer_service/audio_service/ui_service/storage_service
```

The training modes produce practice items. The shared trainer/session layer
handles playback, typed copy, scoring, and statistics. The keyer and audio
services remain the only path to sidetone and key output.

Suggested common training item type:

```c
typedef enum {
    CW_ITEM_CHAR,
    CW_ITEM_WORD,
    CW_ITEM_CALLSIGN,
    CW_ITEM_TEXT_LINE,
} cw_item_type_t;

typedef struct {
    cw_item_type_t type;
    char text[64];
} cw_training_item_t;
```

## Planned Milestones

1. Milestone 1: Project skeleton and hardware ownership boundaries.
2. Milestone 2: CW tone engine and keyboard-to-Morse tone test.
3. Milestone 3: Straight key and paddle input through `keyer_service`.
4. Milestone 4: Keyer mode with keyboard TX buffer and shared key output path.
5. Milestone 5: TX practice mode with decoded text and timing feedback.
6. Milestone 6: RX practice mode with typed copy and scoring.
7. Milestone 7: Lessons and MorseMachine adaptive training.
8. Milestone 8: File-backed Plain Text and Word Training.
9. Milestone 9: Callsign, QSO, SOTA/POTA exchange, and statistics modes.

## Near-Term Notes

- Keep APIs small and explicit.
- Add board-specific code only under the owning service or HAL.
- Prefer stubs with clear logs over half-implemented training behavior.
- Avoid circular dependencies between services.

## UI / Mode Model

Mini-CW uses a vi/vim-inspired modal UI.

On boot, the app enters Keyer mode.

### Keyer Mode

In Keyer mode:

- Paddle or straight-key input is treated as live manual keying.
- Manual keying drives:
  - the key output GPIO pin, not yet assigned
  - the Morse sidetone speaker
- Keyboard input is appended to a TX text buffer.
- The TX text buffer allows the keyboard to act as a handicap/accessibility method for sending Morse.
- Characters, including spaces, are preserved in the buffer.
- A keyboard-CW sender converts buffered characters into Morse timing and sends them through the same keying path as manual input.

### Escape / Mode Selection

The tilde/backtick key exits the current mode and enters a one-character mode selection state.

Accepted keys:

| Key | Mode |
|---|---|
| `k` / `K` | Keyer mode |
| `m` / `M` | Menu mode |

Other modes will be added later.

Implementation note: accept both backtick `` ` `` and tilde `~` as the escape key, because they usually share the same physical keyboard key.

### Architecture Rule

The key output GPIO must have exactly one owner.

Keyboard-generated CW, paddle CW, straight-key CW, practice playback, and future lesson modes must all route through `keyer_service`, which then calls the hardware abstraction layer for key output and sidetone.
