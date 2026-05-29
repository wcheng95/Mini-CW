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

## 2. Screen Layout

Use a **16x12 font**.

A native 16x12 font is preferred instead of scaling up another font.

Screen is divided as:

```text
240 x 135

y=0..18      Top bar, 240 x 19
y=19         1 px separator

y=20..38     Line 1, 240 x 19
y=39..57     Line 2, 240 x 19
y=58..76     Line 3, 240 x 19
y=77..95     Line 4, 240 x 19
y=96..114    Line 5, 240 x 19

y=115        1 px separator
y=116..134   Bottom status bar, 240 x 19
```

Total:

```text
19 + 1 + 5*19 + 1 + 19 = 135
```

Text width:

```text
240 / 12 = 20 characters
```

So each text row should be treated as **20 visible characters max**.

Suggested constants:

```c
#define UI_W 240
#define UI_H 135

#define UI_FONT_W 12
#define UI_FONT_H 16
#define UI_ROW_H 19
#define UI_COLS 20

#define UI_TOP_Y 0
#define UI_TOP_H 19

#define UI_SEP1_Y 19

#define UI_LINE1_Y 20
#define UI_LINE2_Y 39
#define UI_LINE3_Y 58
#define UI_LINE4_Y 77
#define UI_LINE5_Y 96

#define UI_SEP2_Y 115

#define UI_BOTTOM_Y 116
#define UI_BOTTOM_H 19

#define UI_MODE_LINES 5
```

---

## 3. Top Bar

Current top bar:

```text
M:Keyer     Setting
```

Meaning:

```text
M:Keyer   = current mode
Setting   = literal text label
```

Important decision:

```text
"Setting" is shown literally.
It is not a summary of current setting values.
```

User action:

```text
S enters setting modification/list mode for the current mode.
```

Later we may replace `Setting` with icons, but for now use text only.

---

## 4. Mode-Dependent Lines

The middle five lines are mode-dependent:

```text
Line 1
Line 2
Line 3
Line 4
Line 5
```

Each mode owns how these five lines are displayed.

Some settings may be displayed in these lines when useful, but the top bar still keeps the literal word `Setting`.

Example Keyer screen:

```text
M:Keyer     Setting
CQ CQ DE AG6AQ
BUF:HELLO WORLD_
KEY:IAMBIC B
OUT:GPIO??
READY
TX:20 T:700Hz V:20
```

---

## 5. Bottom Status Bar

Current bottom status:

```text
TX:20 T:700Hz V:20
```

Meaning:

```text
TX:20     TX keyer WPM
T:700Hz   sidetone tone frequency
V:20      sidetone volume
```

This is **not general mode settings**.

It is live TX/keyer/audio status.

Ownership:

```text
TX:20     owned by keyer_service
T:700Hz   owned by audio_service
V:20      owned by audio_service
```

UI should only compose the display string.

Example:

```c
snprintf(bottom, sizeof(bottom),
         "TX:%u T:%uHz V:%u",
         keyer_get_tx_wpm(),
         audio_get_tone_hz(),
         audio_get_volume());
```

---

## 6. Bottom Quick Modification Mode

User presses:

```text
Fn
```

to enter bottom status modification mode.

In this mode:

```text
1 / 2  adjust TX WPM -/+
3 / 4  adjust Tone -/+
5 / 6  adjust Volume -/+
```

Volume adjustment should give audio feedback.

Exit bottom modification mode:

```text
Fn again
or
tick-mark / `
```

Ownership rule:

```text
1/2 dispatch to keyer_service
3/4 dispatch to audio_service
5/6 dispatch to audio_service and play feedback beep
```

Suggested display while editing:

```text
1/2 TX 3/4 T 5/6 V
```

---

## 7. Boot and Input Model

Default after boot:

```text
Mini-CW enters Keyer mode
```

Later this may become:

```text
Mini-CW enters previous saved mode
```

but this will be specified later.

In Keyer mode:

```text
All normal keystrokes are taken literally
```

That means typed characters are used for Morse/text input.

Exceptions:

```text
tick-mark / `   exits Keyer mode and waits for command
Fn              enters bottom quick modification mode
```

So in Keyer mode, keys like `M` and `S` are not commands. They are literal input unless the user first presses tick-mark.

---

## 8. Command-Wait State

Pressing tick-mark exits Keyer input and enters command-wait state.

Currently defined commands:

```text
M / m   open command/menu list
S / s   open current-mode settings list
```

Unknown commands can be ignored or show a short message later.

---

## 9. M/m Command List

After:

```text
` then M
```

Mini-CW shows the command/menu list.

The command list uses numbered choices:

```text
1 to 5
```

with page up/down support later.

For now, only show:

```text
1 Keyer
```

Example:

```text
M:Menu      Setting
1 Keyer




TX:20 T:700Hz V:20
```

Selecting:

```text
1
```

returns/enters Keyer mode.

Later, this can expand to:

```text
1 Keyer
2 Lessons
3 MorseMachine
4 Plain Text
5 Words
```

Page 2 could later hold Callsign, Files, About, etc.

---

## 10. S/s Settings List

After:

```text
` then S
```

Mini-CW shows the settings list for the **current mode**.

This should be similar to Mini-FT8 menu style:

```text
numbered items 1-5
page up/down if needed
```

Important distinction:

```text
S settings are mode-specific settings.
Fn bottom edit is for live TX/audio controls.
```

So `TX:20 T:700Hz V:20` should not be treated as normal mode settings.

Possible future Keyer settings:

```text
1 Key Type:IambicB
2 Auto Space:On
3 TX Buffer:On
4 Paddle Rev:Off
5 Save Mode:Keyer
```

Possible future Lesson settings:

```text
1 RX WPM:20
2 Code WPM:20
3 Eff WPM:12
4 Lesson:03
5 Repeat:On
```

---

## 11. Settings Separation

We now have three categories.

### A. Live TX/audio controls

Shown in bottom bar:

```text
TX:20 T:700Hz V:20
```

Owned by:

```text
TX WPM     keyer_service
Tone       audio_service
Volume     audio_service
```

Changed by:

```text
Fn quick modification mode
```

### B. Mode-specific settings

Stored in `setting.txt`.

Changed by:

```text
` + S
```

Examples:

```text
lesson number
word list
keyer behavior
callsign weighting
RX WPM
code WPM
effective WPM
```

### C. Global/app settings

Also likely stored in `setting.txt`, for example:

```text
boot mode
last mode
display behavior
USB storage behavior
```

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

### Step 1: Lock the display layout

Implement the fixed 240x135 UI layout:

```text
top bar
separator
five content lines
separator
bottom bar
```

Create a simple screen model:

```c
typedef struct {
    char top[21];
    char line[5][21];
    char bottom[21];
} mini_cw_screen_t;
```

Goal of Step 1:

```text
Render static demo screen correctly.
No input logic yet.
No settings yet.
```

Demo screen:

```text
M:Keyer     Setting
CQ CQ DE AG6AQ
BUF:
KEY:IAMBIC B
OUT:GPIO??
READY
TX:20 T:700Hz V:20
```

---

### Step 2: Add service ownership for bottom status

Create or refine:

```text
keyer_service
audio_service
```

Ownership:

```text
keyer_service owns TX WPM
audio_service owns tone and volume
```

UI asks services for values and composes:

```text
TX:20 T:700Hz V:20
```

Goal of Step 2:

```text
Bottom status is generated from services, not hardcoded in UI.
```

---

### Step 3: Implement Fn bottom quick-edit

Input behavior:

```text
Fn enters bottom edit mode
1/2 TX WPM -/+
3/4 Tone -/+
5/6 Volume -/+
Fn exits
tick-mark exits
```

Goal of Step 3:

```text
Live bottom values change on screen.
Volume change gives audio feedback.
```

This is a good early test because it checks:

```text
keyboard input
UI redraw
keyer_service ownership
audio_service ownership
audio feedback
```

---

### Step 4: Implement Keyer mode literal input

Default boot mode:

```text
Keyer
```

Input rule:

```text
normal keys go to keyer/text buffer
tick-mark enters command-wait state
Fn enters bottom edit state
```

Goal of Step 4:

```text
Typing letters appears in buffer or is passed to keyer logic.
M and S are literal characters unless tick-mark was pressed first.
```

---

### Step 5: Implement command-wait state

After tick-mark:

```text
M/m opens command list
S/s opens current-mode settings list
tick-mark cancels/returns
```

Goal of Step 5:

```text
vi-like command entry works.
```

---

### Step 6: Implement M/m command list

For now, command list only has:

```text
1 Keyer
```

Selecting `1` returns to Keyer mode.

Goal of Step 6:

```text
Menu page framework works with numbered 1-5 choices.
```

Page up/down can be stubbed for now.

---

### Step 7: Implement S/s settings list framework

For now, show current-mode settings page.

Even if not all settings are editable yet, implement the list style:

```text
1 ...
2 ...
3 ...
4 ...
5 ...
```

Goal of Step 7:

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

