# Mini-CW Develop Notes

Project: **Mini-CW**  
Target hardware: **M5Stack Cardputer / Cardputer ADV class device**  
Project goal: build a standalone CW/Morse trainer for portable ham-radio operators, with both RX practice and early TX practice through paddle/key input.

This project follows the same design philosophy as Mini-FT8 / MiniPanel:

- top-down modular design
- clear application/service/HAL boundaries
- each hardware resource has exactly one owner
- no feature module should directly touch hardware it does not own
- keep the architecture portable to future devices

---

## 1. Project Purpose

Mini-CW is a portable CW/Morse trainer designed around the Cardputer form factor.

It should support:

- CW receive practice
- CW transmit practice
- Koch/Farnsworth lessons
- callsign copy
- ham-radio abbreviation practice
- SOTA/POTA-style exchange practice
- session logging
- future paddle/keyer and QSO simulation features

The project should not simply duplicate a web trainer like LCWO. Its unique value is:

> A standalone ham-radio CW trainer for portable operators: LCWO-style learning + RufzXP-style callsign practice + SOTA/POTA exchange simulation + paddle/key TX practice, all offline on Cardputer.

---

## 2. Major Existing CW Trainer Categories

Existing trainers fall into several functional categories:

| Category | Examples | Main Function |
|---|---|---|
| Character learning | LCWO, G4FON, Just Learn Morse Code, Morse Mania, IZ2UUF Koch CW | Learn letters, numbers, prosigns using Koch/Farnsworth |
| Head-copy / word practice | Morse Code Ninja, LCWO | Move from individual characters to words, phrases, and QSOs |
| Callsign reflex training | RufzXP, Call Sign Trainer | Fast recognition of amateur callsigns |
| On-air simulation | Morse Runner, Morserino-32 | Practice realistic QSO, contest, pileup, or hardware keyer behavior |

Mini-CW should borrow the best ideas from these, but focus on portable offline ham-radio use.

---

## 3. Core Design Direction

Initial project direction:

1. Add paddle/key support early.
2. Make TX practice a first-class feature, not a later add-on.
3. Use a clean top-down modular architecture.
4. Ensure every hardware resource has one owner.
5. Keep UI simple and Cardputer-friendly.
6. Make later porting possible.

---

## 4. Top-Down Architecture

High-level architecture:

```text
app_core
  |
  +-- cw_trainer_service
  |     +-- lesson_engine
  |     +-- rx_practice_engine
  |     +-- tx_practice_engine
  |     +-- score_engine
  |     +-- session_logger
  |
  +-- ui_service
  |     +-- screen_view
  |     +-- keyboard_input
  |
  +-- audio_service
  |     +-- cw_tone_generator
  |
  +-- keyer_service
  |     +-- paddle_decoder
  |     +-- straight_key_decoder
  |     +-- timing_analyzer
  |
  +-- storage_service
  |     +-- profile_store
  |     +-- lesson_store
  |     +-- log_store
  |
  +-- platform_hal
        +-- display_owner
        +-- keyboard_owner
        +-- speaker_owner
        +-- paddle_gpio_owner
        +-- sd_owner
        +-- rtc_owner
```

The application should be controlled from the top:

- `app_core` owns the application state machine.
- service modules own application features.
- HAL/port modules own hardware access.
- lower-level modules should not call back into high-level app logic directly unless through explicit event queues or callback interfaces.

---

## 5. Hardware Ownership Rule

Each hardware resource must have exactly one owner.

| Hardware Resource | Owner Module | Other Modules Access Through |
|---|---|---|
| Display | `ui_port` / `display_owner` | `ui_service` API |
| Cardputer keyboard | `keyboard_owner` | input events |
| Speaker / DAC / I2S | `audio_service` / `speaker_owner` | `audio_cw_*()` API |
| Paddle/key GPIO | `keyer_service` / `paddle_gpio_owner` | keyer events |
| SD card / SPIFFS | `storage_service` | profile/log/file APIs |
| RTC/time | `platform_hal` / `rtc_owner` | timestamp API |
| Battery/PMU | `platform_hal` / `power_owner` | status API |

Strict rule:

> No lesson engine, scoring engine, UI code, or app mode code should directly touch GPIO, speaker, display, SD, RTC, or PMU.

---

## 6. Proposed Application Modes

Initial modes:

```text
MODE_RX_PRACTICE
MODE_TX_PRACTICE
MODE_CALLSIGN
MODE_QSO
MODE_STATS
MODE_MENU
```

Possible Cardputer key mapping:

| Key | Mode |
|---|---|
| `R` | Receive practice |
| `T` | Transmit practice |
| `C` | Callsign trainer |
| `W` | Word/head-copy trainer |
| `Q` | QSO phrase trainer |
| `S` | Stats |
| `M` | Menu/settings |
| `L` | Lesson select |
| `Esc` | stop tone / cancel current action |

---

## 7. Major Modules

### 7.1 `app_core`

Responsibilities:

- own app mode/state machine
- initialize services
- route input events to the active mode
- request UI redraws
- coordinate trainer sessions
- never directly touch hardware

Example states:

```text
APP_MODE_RX_PRACTICE
APP_MODE_TX_PRACTICE
APP_MODE_CALLSIGN
APP_MODE_QSO
APP_MODE_STATS
APP_MODE_MENU
```

---

### 7.2 `cw_trainer_service`

Main trainer brain.

Responsibilities:

- start/stop lessons
- generate target text
- manage RX practice
- manage TX practice
- compare user answer with target
- update weak-character statistics
- request session logging

It should use other services through clean APIs:

- `audio_service` for playback
- `keyer_service` for TX/key events
- `ui_service` for display model updates
- `storage_service` for profiles/logs

---

### 7.3 `audio_service`

Owns CW tone generation and speaker output.

Responsibilities:

- generate sidetone
- play Morse text
- play individual symbols
- stop playback
- apply WPM, Farnsworth WPM, pitch, and volume

Example API:

```c
void audio_service_init(void);
void audio_cw_set_pitch(uint16_t hz);
void audio_cw_set_wpm(uint8_t wpm);
void audio_cw_set_farnsworth_wpm(uint8_t effective_wpm);
void audio_cw_play_text(const char *text);
void audio_cw_play_symbol(char symbol);
void audio_cw_start_sidetone(void);
void audio_cw_stop_sidetone(void);
void audio_cw_stop(void);
```

No other module should generate sidetone directly.

---

### 7.4 `keyer_service`

This should be added early.

Responsibilities:

- own paddle/key GPIO input
- debounce key input
- detect straight-key, single-paddle, or dual-paddle events
- later support iambic A/B behavior
- request sidetone through `audio_service`
- decode user-sent Morse into characters
- measure timing quality
- emit completed keyer events to the app/trainer

Possible event types:

```c
typedef enum {
    KEYER_EVENT_NONE = 0,
    KEYER_EVENT_DIT,
    KEYER_EVENT_DAH,
    KEYER_EVENT_CHAR_COMPLETE,
    KEYER_EVENT_WORD_SPACE,
    KEYER_EVENT_TIMING_WARNING,
    KEYER_EVENT_TIMING_ERROR
} keyer_event_type_t;
```

Important rule:

> TX practice receives decoded keyer events from `keyer_service`, not raw GPIO edges.

---

### 7.5 `tx_practice_engine`

Compares what the user sends against a target.

Example flow:

```text
Target: CQ SOTA DE AG6AQ
User sends through paddle/key
keyer_service decodes: CQ SOTA DE AG6AQ
score_engine compares text and timing
UI shows accuracy and timing feedback
```

Feedback examples:

```text
Accuracy: 92%
Timing:   78%
Weak:     word spacing, dah too short
```

This makes TX practice more useful than a simple sidetone keyer.

---

### 7.6 `rx_practice_engine`

Classic receive trainer.

Responsibilities:

- generate target text
- play Morse through `audio_service`
- collect typed copy from Cardputer keyboard
- compare typed answer with target
- show errors and weak characters

---

### 7.7 `score_engine`

Responsibilities:

- compare target vs user copy
- calculate character accuracy
- calculate word accuracy
- identify weak characters
- identify missed/extra characters
- later combine RX accuracy and TX timing quality

---

### 7.8 `storage_service`

Responsibilities:

- own persistent files
- read/write profile
- read/write lesson progress
- write session logs
- support SD/SPIFFS backend later

Possible files:

```text
Profile.ini
Lessons.dat
SessionLog.csv
WeakChars.csv
```

---

### 7.9 `ui_service`

Responsibilities:

- own display model
- draw current mode screen
- receive high-level UI events
- avoid touching trainer internals directly

UI should remain simple and Cardputer-friendly.

Example screen:

```text
CW TRAIN  Koch 12/25 WPM
Lesson 08: K M U R E S N A
Play:  K M U R S A N E
Copy:  K M U R S ? N E
Acc: 87%  Weak: S A
[Enter]=check [Space]=replay
```

---

## 8. Suggested Milestones

### Milestone 1: Skeleton

Create project skeleton and architectural boundaries.

Scope:

```text
app_core
ui_service
audio_service
keyer_service
storage_service
platform_hal
```

At this stage, functions can be mostly stubs, but ownership and API boundaries should be clear.

Expected result:

- project builds
- app boots
- display shows a simple Mini-CW home screen
- keyboard events can switch modes
- audio service has stub CW tone functions
- keyer service has stub paddle/key event API
- storage service has stub profile/log API
- no module violates hardware ownership rules

---

### Milestone 2: CW Tone Engine

Implement:

```text
text -> Morse symbols -> timed tone output
```

Support:

```text
WPM
Farnsworth WPM
pitch
volume
basic spacing
```

---

### Milestone 3: Paddle/Key Input

Add early TX support.

Support:

```text
straight key
single paddle
dual paddle GPIO abstraction
basic dit/dah detection
sidetone request through audio_service
decoded character event emission
```

Start simple. Iambic A/B can come later.

---

### Milestone 4: TX Practice Mode

Implement:

```text
show target
user sends with paddle/key
decode sent text
score accuracy and timing
show feedback
```

---

### Milestone 5: RX Practice Mode

Implement:

```text
play target
user types copy on Cardputer keyboard
score typed result
show errors
```

---

### Milestone 6: Ham-Radio Modes

Implement:

```text
callsign trainer
SOTA/POTA exchange trainer
QSO phrase trainer
mini pileup later
```

---

## 9. Milestone 1 Codex Prompt

Use this prompt for Codex to create the first project skeleton.

```text
You are helping me start a new ESP-IDF project called Mini-CW for the M5Stack Cardputer / Cardputer ADV class device.

Goal:
Create Milestone 1: project skeleton only.

The project is a standalone CW/Morse trainer with RX practice and early TX practice through paddle/key input. For this milestone, do not implement full Morse timing, scoring, or real training logic yet. Focus on clean architecture, module boundaries, buildability, and hardware ownership.

Important architecture rule:
Follow top-down modular design. Each hardware resource must have exactly one owner. No feature module should directly touch hardware that it does not own.

Hardware ownership rules:
- Display is owned by the UI port/service only.
- Cardputer keyboard is owned by the input/UI port only.
- Speaker / tone output is owned by audio_service only.
- Paddle/key GPIO is owned by keyer_service or its HAL owner only.
- SD/SPIFFS/file access is owned by storage_service only.
- RTC/time and battery/PMU are owned by platform_hal only.
- app_core and trainer engines must not directly call GPIO, display, speaker, SD, RTC, or PMU APIs.

Create these modules:

1. app_core
   - Owns application state machine.
   - Initializes services.
   - Routes high-level events.
   - Defines initial modes:
     APP_MODE_RX_PRACTICE
     APP_MODE_TX_PRACTICE
     APP_MODE_CALLSIGN
     APP_MODE_QSO
     APP_MODE_STATS
     APP_MODE_MENU
   - Should not directly touch hardware.

2. ui_service
   - Owns screen drawing and keyboard/input event abstraction.
   - For Milestone 1, draw a simple Mini-CW home/status screen.
   - Show current mode and a few help lines.
   - Expose high-level input events to app_core.
   - Do not put training logic here.

3. audio_service
   - Owns all speaker/tone output.
   - Provide stub APIs:
     audio_service_init()
     audio_cw_set_pitch()
     audio_cw_set_wpm()
     audio_cw_set_farnsworth_wpm()
     audio_cw_play_text()
     audio_cw_play_symbol()
     audio_cw_start_sidetone()
     audio_cw_stop_sidetone()
     audio_cw_stop()
   - These can be stubs or simple log messages for now.

4. keyer_service
   - Owns paddle/key input abstraction.
   - Provide stub APIs and event types for straight key, single paddle, and dual paddle future support.
   - Define keyer event types:
     KEYER_EVENT_NONE
     KEYER_EVENT_DIT
     KEYER_EVENT_DAH
     KEYER_EVENT_CHAR_COMPLETE
     KEYER_EVENT_WORD_SPACE
     KEYER_EVENT_TIMING_WARNING
     KEYER_EVENT_TIMING_ERROR
   - Provide init and poll/update functions.
   - Do not implement full iambic keyer yet.
   - Do not directly draw UI.
   - If sidetone is needed later, it must request it through audio_service instead of owning speaker output.

5. cw_trainer_service
   - High-level trainer service.
   - Provide placeholder functions for starting/stopping RX practice and TX practice.
   - Define simple target/copy buffers if useful, but keep logic minimal.
   - Should consume keyer events, not raw GPIO.

6. storage_service
   - Owns profile/log persistence.
   - Provide stub APIs:
     storage_service_init()
     storage_profile_load()
     storage_profile_save()
     storage_session_log_append()
   - Stubs can log actions for now.

7. platform_hal
   - Owns low-level board initialization and general platform services.
   - Provide init function.
   - Keep board-specific code isolated here or in ui/audio/keyer ports.
   - Do not let app_core call raw board APIs except through service init APIs.

Deliverables:
- Create a clear folder/module structure.
- Add .c/.h files for each module.
- Add comments at the top of each module describing its responsibility and hardware ownership.
- Make the project build.
- Add a README.md explaining:
  - project purpose
  - architecture
  - hardware ownership rule
  - Milestone 1 status
  - planned next milestones
- Add a develop_notes.md file containing the design notes and milestone list.
- Keep implementation minimal and clean.
- Prefer readable code over clever code.
- Do not add unnecessary dependencies.
- Do not implement the full CW trainer yet.

Expected boot behavior:
- Initialize platform, UI, audio, keyer, storage, and trainer services.
- Show a simple Mini-CW screen.
- Allow mode variable to exist and be displayed.
- Keyboard/input handling can be stubbed if board input is not ready.
- Log service initialization order.

Coding style:
- Use explicit module names.
- Keep APIs small.
- Use app_core as the orchestration layer.
- Avoid circular dependencies.
- Avoid global hardware access from feature modules.
- Use clear comments where a function is a Milestone 1 stub.
```

---

## 10. Open Design Questions

To decide later:

1. Project name: `Mini-CW`, `CardCW`, `CWputer`, or `MiniMorse`.
2. Paddle/key connector pin assignment.
3. Whether initial key input should support straight key first or paddle first.
4. Whether audio output uses speaker, DAC, I2S, or Cardputer speaker abstraction.
5. Whether to share code style with MiniPanel or start as a cleaner separate template.
6. Whether BLE terminal support is useful for this project.
7. Whether later versions should support rig keying output.

---

## 11. Near-Term Recommendation

Start Milestone 1 as architecture-only.

Do not implement full Morse timing yet. The first success condition should be:

> The project builds, boots, shows a Mini-CW screen, and has clean module boundaries for UI, audio, keyer, trainer, storage, and platform HAL.

After this is stable, move to CW tone timing and paddle/key input.
