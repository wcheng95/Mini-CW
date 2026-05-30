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
APP_MODE_PRACTICE
APP_MODE_KEYER
APP_MODE_LESSONS
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

## Current Step 1 UI Status

Step 1 now focuses on the UI contract before continuing keyer/audio behavior.

Implemented:

- `APP_MODE_KEYER` as the startup app mode.
- Real Cardputer/Cardputer ADV ST7789 screen drawing behind `ui_service`.
- Real Cardputer ADV TCA8418 keyboard polling behind `ui_service`.
- `ui_service` owns UI view/menu state.
- `ui_screen` is private to `ui_service` and owns fixed 240x135 layout.
- `ui_cardputer_port` owns all low-level `M5Cardputer.Display` and
  `M5Cardputer.Keyboard` access.
- Normal demo view plus global menu page 1, global menu page 2, and local menu
  framework views.
- Ctrl toggles global menu; Fn toggles local menu.

Current limitations:

- Menu item editing is stubbed/minimal.
- FATFS, `setting.txt`, USB mass storage, persistence, real paddle/straight-key
  I/O, full keyer behavior, and service-owned bottom status are not part of
  this step.
- Audio and keyer services remain present, but Step 1 UI demo values are not
  yet sourced from service-owned state.


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

# Mini-CW Current Design Notes

This note captures the current Mini-CW/Cardputer design decisions and the initial step-by-step implementation plan. The goal is to keep the project simple, modular, and easy to implement one step at a time.

---

## 1. Target Hardware

```text
Device: M5 Cardputer ADV
Screen: 240 x 135
Flash: 8 MB
Filesystem: 4 MB FATFS
```

Flash layout decision:

```text
8 MB total flash
- 4 MB FATFS
- remaining flash for app code, NVS, partition table, bootloader, and housekeeping
- no unused reserved flash space
```

FATFS requirement:

```text
FATFS should be accessible from PC as a USB flash drive.
```

First user-visible file:

```text
setting.txt
```

Purpose:

```text
setting.txt stores settings for each mode.
```

Important filesystem rule:

```text
When FATFS is mounted as USB mass storage for PC access,
Mini-CW firmware should not write to FATFS at the same time.
```

---

## 2. UI Ownership Boundary

The Step 1 UI boundary is:

```text
app_core
  -> ui_service
      -> ui_screen
          -> ui_cardputer_port
```

Rules:

- `app_core` owns app flow only and calls public `ui_service` APIs.
- `ui_service` owns UI behavior/state, including normal/global/local view state.
- `ui_screen` is private to `ui_service` and owns fixed 240x135 layout.
- `ui_cardputer_port` owns low-level Cardputer display/keyboard access.
- `app_core` must not include `ui_screen.h` or call `ui_screen_*`.

## 3. Screen Layout

Screen: 240 x 135, 20 visible characters per text row.

```text
y=0..18      Top bar, 240 x 19
y=19..20     2 px green separator
y=21..39     Line 1, 240 x 19
y=40..58     Line 2, 240 x 19
y=59..77     Line 3, 240 x 19
y=78..96     Line 4, 240 x 19
y=97..115    Line 5, 240 x 19
y=116..134   Bottom bar, 240 x 19, cyan text on black
```

Total:

```text
19 + 2 + 5*19 + 19 = 135
```

Screen model:

```c
typedef struct {
    char mode[14];
    char tone[4];
    char vol[3];
    char line[5][21];
    char key_in[9];
    char key_out[9];
    char key_wpm[3];
} mini_cw_screen_t;
```

`ui_screen` formats top and bottom rows internally. `app_core` and
`ui_service` should not preformat those full rows.

## 4. Top, Middle, Bottom Rows

Top row format:

```text
[mode:13] [tone:3] [vol:2]
```

Example:

```text
Keyer         700 40
```

Middle lines are used for normal mode display, global menu display, or local
mode-specific menu display.

Bottom row format:

```text
[KeyIn:8] [KeyOut:8] [KeyInWPM:2]
```

Example:

```text
Paddle   Paddle   20
```

The bottom row is drawn as cyan text on black. Step 2 will decide how service
owned keyer/audio values feed this row.

Expected Step 1 normal demo screen:

```text
Keyer         700 40
CQ CQ DE AG6AQ
BUF:
KEYIN:Paddle
KEYOUT:Paddle
READY
Paddle   Paddle   20
```

There is a graphical 2 px green separator below the top bar.

## 5. Global and Local Menus

The UI has three view states:

```c
typedef enum {
    UI_VIEW_NORMAL = 0,
    UI_VIEW_GLOBAL_MENU,
    UI_VIEW_LOCAL_MENU,
} ui_view_t;
```

Global menu:

- `Ctrl` enters/exits global menu mode.
- Number keys `1` to `5` select visible items.
- Up/Down page through global menu pages.
- Left/Right may change values later.

Local menu:

- `Fn` enters/exits local mode-specific menu mode.
- It is separate from the global menu.
- Practice mode has no local settings.
- Keyer and Lessons local settings may remain stubbed for Step 1.

Mini-CW uses the Mini-FT8 menu interaction style, but keeps global and local
menus as separate UI states.

Global menu page 1:

```text
1 Mode:Keyer
2 Tone:700Hz
3 Volume:40
4 KeyIn:Paddle
5 KeyIn WPM:20
```

Global menu page 2:

```text
1 KeyOut:Paddle
2 KeyOut WPM:20
3 Sleep/Batt 90%
4 Date
5 Time
```

No local settings view:

```text
No local settings
```

## 6. Ownership Separation

Current Step 1 UI values are demo/framework values only.

Later ownership:

```text
keyer_service owns keyer speed/key input/output data
audio_service owns tone and volume
storage_service owns persistence
```

Do not implement FATFS, `setting.txt`, USB mass storage, persistence, real
audio ownership transfer, real keyer ownership transfer, paddle/straight-key
I/O, or full keyer behavior in this step.

---

## 12. Proposed First `setting.txt`

Initial version can be simple and user-editable:

```ini
# Mini-CW setting.txt

[global]
boot_mode=keyer
last_mode=keyer

[keyer]
key_type=iambic_b
auto_space=on
tx_buffer=on
paddle_reverse=off

[lesson]
rx_wpm=20
code_wpm=20
effective_wpm=12
lesson=1

[words]
rx_wpm=20
code_wpm=20
effective_wpm=15
word_list=common_words.txt

[callsign]
rx_wpm=20
code_wpm=20
effective_wpm=15
local_weight=3
```

Separate TX/audio live values could be stored either in NVS or in a separate section if we want them persistent:

```ini
[tx]
tx_wpm=20
tone_hz=700
volume=20
```

But architecturally, ownership still belongs to:

```text
tx_wpm    keyer_service
tone_hz   audio_service
volume    audio_service
```

---

## 13. Suggested Implementation Plan

We should do this in small steps.

### Step 1: UI layout and menu framework

Implement the fixed 240x135 UI layout and ownership boundary:

```text
app_core -> ui_service -> ui_screen -> ui_cardputer_port
```

Create the field-based screen model:

```c
typedef struct {
    char mode[14];
    char tone[4];
    char vol[3];
    char line[5][21];
    char key_in[9];
    char key_out[9];
    char key_wpm[3];
} mini_cw_screen_t;
```

Goal of Step 1:

```text
Render the normal demo screen correctly.
Keep ui_screen private to ui_service.
Add separate UI_VIEW_GLOBAL_MENU and UI_VIEW_LOCAL_MENU framework.
Ctrl toggles global menu; Fn toggles local menu.
Menu item editing can remain stubbed.
```

Demo screen:

```text
Keyer         700 40
CQ CQ DE AG6AQ
BUF:
KEYIN:Paddle
KEYOUT:Paddle
READY
Paddle   Paddle   20
```

### Step 2: Add service ownership for bottom status

Move top/bottom displayed values to their future service owners without
changing the UI contract:

```text
keyer_service owns key input/output mode and WPM data
audio_service owns tone and volume
ui_service composes public UI state from service APIs
```

Do not put low-level display or keyboard calls outside `ui_cardputer_port`.

---

### Step 3: Implement Keyer mode literal input

Default boot mode:

```text
Keyer
```

Input rule:

```text
normal keys go to keyer/text buffer
tick-mark enters command-wait state
Ctrl enters global menu
Fn enters local mode menu
```

Goal of Step 3:

```text
Typing letters appears in buffer or is passed to keyer logic.
M and S are literal characters unless tick-mark was pressed first.
```

---

### Step 4: Implement command-wait state

After tick-mark:

```text
M/m opens command list
S/s opens current-mode settings list
tick-mark cancels/returns
```

Goal of Step 4:

```text
vi-like command entry works.
```

---

### Step 5: Implement M/m command list

For now, command list only has:

```text
1 Keyer
```

Selecting `1` returns to Keyer mode.

Goal of Step 5:

```text
Menu page framework works with numbered 1-5 choices.
```

Page up/down can be stubbed for now.

---

### Step 6: Expand settings list framework

For now, show current-mode settings page.

Even if not all settings are editable yet, implement the list style:

```text
1 ...
2 ...
3 ...
4 ...
5 ...
```

Goal of Step 6:

```text
Settings page framework exists and looks like Mini-FT8 menu style.
```

---

### Step 8: Add FATFS partition

Use 8 MB flash with 4 MB FATFS.

Partition idea:

```csv
# Name,     Type, SubType, Offset,   Size,     Flags
nvs,        data, nvs,     0x9000,   0x6000,
phy_init,   data, phy,     0xf000,   0x1000,
factory,    app,  factory, 0x10000,  0x3F0000,
fatfs,      data, fat,     0x400000, 0x400000,
```

Goal of Step 8:

```text
Firmware boots with 4 MB FATFS mounted.
```

---

### Step 9: Create/load `setting.txt`

Boot behavior:

```text
mount FATFS
if setting.txt exists: load it
if setting.txt missing: create default setting.txt
```

Goal of Step 9:

```text
User-visible setting.txt exists and can be edited later.
```

---

### Step 10: Add USB flash-drive mode

Expose FATFS to PC as USB mass storage.

Important rule:

```text
When PC owns FATFS, firmware must not write to it.
```

Goal of Step 10:

```text
User can connect Cardputer to PC and edit setting.txt.
```

---

## Recommended Next Step

Start with **Step 1 only**:

```text
Implement fixed UI layout and static demo rendering.
```

Do not add settings, FATFS, command mode, or Fn editing yet. Once the screen layout is solid, every later feature has a stable place to display itself.
