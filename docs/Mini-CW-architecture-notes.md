# Mini-CW Architecture Notes

This note documents the current Mini-CW architecture at a learning level.  
The goal is to make the block structure, data flow, and ownership boundaries easy to understand and maintain.

## 1. Big Picture

Mini-CW is organized around a simple rule:

> Each hardware resource or major behavior should have one clear owner.

At the highest level, `app_core` is the conductor. It initializes services, runs the main loop, tracks the active mode, and routes events between services.

```text
                           Mini-CW Application Architecture
                           ================================


                    +--------------------------------------+
                    |               app_core               |
                    |--------------------------------------|
                    | Owns application state machine        |
                    | Owns current mode                     |
                    | Initializes all services              |
                    | Runs main 5 ms loop                   |
                    | Routes events between services        |
                    |                                      |
                    | Does NOT own hardware                 |
                    +------------------+-------------------+
                                       |
              main loop calls         |          routes UI/keyer events
                                       v

+--------------------+     +--------------------+     +--------------------+
|     ui_service     |     |   keyer_service    |     | cw_trainer_service |
|--------------------|     |--------------------|     |--------------------|
| Owns screen UI     |     | Owns paddle input  |     | Owns trainer modes |
| Owns keyboard UI   |     | Owns key output    |     | Lessons            |
| Produces UI events |     | Owns key timing    |     | Words              |
| Mode/menu editing  |     | Owns iambic logic  |     | Callsigns          |
| Rows 1-6 display   |     | Owns SK decoding   |     | PlainText playback |
+---------+----------+     | Owns tune behavior |     +---------+----------+
          |                | Produces keyer     |               |
          |                | events             |               |
          |                +----+----------+----+               |
          |                     |          |                    |
          |                     |          |                    |
          v                     v          v                    v

+--------------------+   +----------------+   +--------------------+
| Cardputer display  |   | paddle / SK in |   |   audio_service    |
| Cardputer keyboard |   | G13 tip        |   |--------------------|
+--------------------+   | G15 ring       |   | Sidetone           |
                         +----------------+   | CW audio playback  |
                                              | Tone Hz / volume   |
                         +----------------+   | Dit/dah audio      |
                         | key output     |   +--------------------+
                         | G3 tip         |
                         | G6 ring        |
                         | open-drain     |
                         +----------------+


+--------------------+     +--------------------+
|  storage_service   |     |    platform_hal    |
|--------------------|     |--------------------|
| Owns FATFS files   |     | Owns DS3231 RTC    |
| setting.txt        |     | Owns software time |
| trainer text/data  |     | Owns battery read  |
| qso/op CSV files   |     | Owns deep sleep    |
+--------------------+     | Board-level setup  |
                           +--------------------+
```

## 2. Main Modules

### `app_core`

`app_core` is the top-level coordinator.

It owns:

- Current application mode.
- Main loop timing.
- Event routing.
- High-level state transitions.
- Service initialization order.

It should not own:

- Display drawing details.
- Keyboard scanning details.
- Paddle GPIO details.
- Sidetone generation details.
- FATFS file implementation details.

A useful mental model:

```text
app_core decides what should happen next,
but each service decides how its own job is done.
```

### `ui_service`

`ui_service` owns the user interface.

It owns:

- Cardputer display UI.
- Cardputer keyboard UI.
- Rows 1-6 display model.
- Top bar / mode display.
- Menu rendering.
- Mode selection UI.
- UI event generation.

It should not directly modify non-UI service internals. It should report user intent to `app_core`, and `app_core` should route that intent to the correct service.

### `keyer_service`

`keyer_service` owns the keyer domain.

It owns:

- Paddle input GPIO.
- Straight-key input GPIO.
- Key output GPIO.
- Paddle mode behavior.
- Iambic A / Iambic B / Bug behavior.
- Straight-key decoding.
- Key timing.
- Tune behavior.
- Keyboard-to-CW transmit timing.
- Keyer event generation.

Current GPIO ownership:

```text
KEYER_GPIO_TIP       GPIO_NUM_13
KEYER_GPIO_RING      GPIO_NUM_15
KEYER_GPIO_OUT_TIP   GPIO_NUM_3
KEYER_GPIO_OUT_RING  GPIO_NUM_6
```

This means paddle/key input ownership is currently clear:

```text
keyer_service owns both:
  1. physical GPIO access for paddle/key input/output
  2. Morse/keyer behavior built on top of those pins
```

This is acceptable under the one-owner rule because no other module should touch those pins directly.

### `audio_service`

`audio_service` owns audio output.

It owns:

- Sidetone generation.
- Tone frequency.
- Volume.
- Dit/dah playback.
- Future sine-wave tone and envelope behavior.

`keyer_service` may request sidetone actions, but it should not generate speaker samples directly.

### `cw_trainer_service`

`cw_trainer_service` owns trainer content and trainer-mode state.

It owns:

- Lessons mode.
- Words mode.
- Callsigns mode.
- PlainText mode.
- Trainer text generation.
- Trainer playback state.
- Trainer progress state.

It should not own paddle GPIO or display hardware.

### `storage_service`

`storage_service` owns persistent storage.

It owns:

- FATFS access.
- `setting.txt`.
- Trainer files.
- Callsign/name lookup files.
- Save/load behavior.

Other services should ask `storage_service` for data instead of directly touching FATFS.

### `platform_hal`

`platform_hal` owns board-level platform features.

It owns:

- DS3231 RTC access.
- Software time.
- Battery readout.
- Deep sleep.
- General board initialization.

In the current design, `platform_hal` does **not** own paddle/key GPIO. Those are keyer-domain GPIOs and are owned by `keyer_service`.

## 3. Paddle / Key Input Flow

The paddle and straight-key path is one of the most important parts of Mini-CW.

Current design:

```text
Paddle / straight key adapter
        |
        |  physical GPIO input
        |  G13 = tip
        |  G15 = ring
        v
+-------------------------------+
|          keyer_service        |
|-------------------------------|
| Reads GPIO directly           |
| Applies KeyIn mode:           |
|   Paddle / PaddleR / SK-T/R   |
| Applies paddle mode:          |
|   Iambic A / Iambic B / Bug   |
| Measures timing               |
| Decodes Morse patterns        |
| Produces keyer_event_t        |
| Drives keyOut GPIO            |
| Requests sidetone/audio       |
+-------+--------------+--------+
        |              |
        |              |
        v              v
+---------------+   +----------------+
| key output    |   | audio_service  |
| G3 / G6       |   | sidetone       |
| active-low    |   | dit/dah tone   |
| open-drain    |   +----------------+
+---------------+
        |
        v
External radio key jack
```

Learning point:

```text
Hardware ownership:
  Who is allowed to touch the physical pins/peripheral?

Behavior ownership:
  Who understands what those pins mean?
```

For paddle/key handling in the current Mini-CW code:

```text
keyer_service owns hardware access
+
keyer_service owns Morse/keyer behavior
```

That is simple and clean because these pins exist only for the keyer function.

## 4. Main Loop / Event Flow

Mini-CW runs a short periodic loop. The exact implementation may evolve, but the current learning model is:

```text
Every 5 ms, app_core_run() does roughly this:

+------------------------------------------------------+
| app_core main loop                                   |
+------------------------------------------------------+
        |
        v
  keyer_service_update()
        |
        | reads G13/G15
        | updates paddle/SK/tune/TX state
        | may push KEYER_EVENT_*
        v
  cw_trainer_service_update()
        |
        v
  app_core_drain_keyer_events()
        |
        | KEYER_EVENT_CHAR_COMPLETE
        | KEYER_EVENT_WORD_SPACE
        | KEYER_EVENT_BACKSPACE
        | KEYER_EVENT_ENTER
        | KEYER_EVENT_DIT / DAH
        v
  app_core routes decoded intent
        |
        +--> Keyer mode: update keyer display / OP lookup / TX buffer
        |
        +--> Trainer modes: update copied input text
        |
        v
  ui_service_poll_input()
        |
        | keyboard/menu/mode events
        v
  app_core_handle_ui_event()
        |
        +--> change mode
        +--> change settings
        +--> start replay/training
        +--> start keyer TX
        +--> sleep
        |
        v
  save dirty config / refresh time / delay 5 ms
```

The important lesson is that `app_core` should remain a router/coordinator. It should avoid becoming the owner of every detail.

## 5. Ownership Table

| Thing | Current owner | Why |
|---|---|---|
| Application mode | `app_core` | It maps UI mode to app mode and routes behavior. |
| Main loop | `app_core` | It calls update/poll functions for services. |
| Paddle input GPIO G13/G15 | `keyer_service` | It configures and reads those GPIOs directly. |
| Key output GPIO G3/G6 | `keyer_service` | It configures output open-drain GPIO and controls keying. |
| Iambic A/B/Bug logic | `keyer_service` | This is Morse/keyer timing logic, not HAL logic. |
| Straight-key timing/adaptive WPM | `keyer_service` | It measures key-down duration and adapts SK WPM. |
| Decoded paddle/SK events | `keyer_service` | It emits `KEYER_EVENT_*` through its event ring. |
| Sidetone generation | `audio_service` | `keyer_service` requests tone on/off or dit/dah playback. |
| Display and keyboard UI | `ui_service` | UI owns human interaction, menus, rows, and mode selection. |
| Trainer content/state | `cw_trainer_service` | Lessons, Words, Callsigns, and PlainText belong here. |
| FATFS/settings/files | `storage_service` | Persistent data belongs here. |
| DS3231/time/battery/sleep | `platform_hal` | General board/platform services belong here. |

## 6. Good Dependency Direction

A clean dependency direction is:

```text
app_core
  calls services

services
  own their own internal state
  expose public APIs
  do not freely modify each other

hardware-facing services
  own specific hardware resources
```

Preferred pattern:

```text
UI input happens
    |
    v
ui_service reports an event
    |
    v
app_core interprets the event based on current mode
    |
    v
app_core calls the right service API
```

Avoid this pattern:

```text
ui_service directly changes keyer_service internal state
ui_service directly changes cw_trainer_service internal state
random modules directly read/write FATFS
random modules directly touch GPIO pins owned by keyer_service
```

## 7. Simple Mental Model

A compact way to remember the design:

```text
app_core is the conductor.
ui_service is the face.
keyer_service is the hand.
audio_service is the voice.
storage_service is the memory.
platform_hal is the clock, battery, and sleep manager.
cw_trainer_service is the teacher.
```

## 8. Notes for Future Refactoring

The current design is already clear enough for Mini-CW. Future refactoring should preserve the one-owner rule.

Possible future improvement:

```text
platform_hal could expose generic GPIO helper functions,
but it should not understand Morse timing or paddle semantics.
```

If that happens, the ownership should still remain clear:

```text
platform_hal:
  low-level pin read/write helpers only

keyer_service:
  owns paddle/key meaning, timing, iambic behavior, and key output policy
```

Do not move Morse behavior into `platform_hal`; that would blur the architecture boundary.

## 9. Summary

Mini-CW can be understood as a small event-driven embedded application:

```text
Inputs:
  keyboard
  paddle / straight key
  files / settings
  RTC / battery

Core coordinator:
  app_core

Behavior services:
  keyer_service
  cw_trainer_service

Output services:
  ui_service
  audio_service
  key output GPIO

Persistence/platform:
  storage_service
  platform_hal
```

The most important architecture rule is:

> One resource, one owner. Other modules communicate through that owner's public API.
