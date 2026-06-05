# Make an Adapter

* KeyIn: G13-Tip, G15-Ring - to paddle
* KeyOut: G3-Tip, G6-Ring - to radio
* Debug: G4-TX, G5-RX - not required for normal use

![Adaptor](mini-cw_adaptor.jpeg)

# Mini-CW User Manual

Mini-CW is a portable CW/Morse keyer and trainer for the M5 Cardputer ADV. The
current firmware has a working keyer plus LCWO-inspired trainer modes for
Lessons, Words, Callsigns, and Plain Text.

## Screen Layout

The display is a fixed 240 x 135 text UI:

```text
Top row: current mode
Green separator
6 content rows, 20 characters each
```

The last content row is often used as a compact status or command hint.

## Main Controls

```text
Opt       open/close mode select
Ctrl      open/close settings for the current mode
Enter     start or submit in trainer modes
Backspace edit typed copy
`         abort current trainer run, or stop playback/keyer activity
```

Settings screens use numbered rows. Press the row number to edit a value, type
digits or use `,` and `/` to decrease/increase, then press Enter to apply.
Backtick cancels an edit.

## Modes

Mode select currently shows:

```text
1 Keyer
2 Lessons
3 Words
4 Calls
5 Plain
6 System
```

### Keyer

Keyer mode starts on boot. Paddle and straight-key input are handled by
`keyer_service`. Keyboard CW characters typed on the Cardputer are queued on
line 6 and transmitted after `txDelay`, or immediately with Enter.

The Keyer top row is a fixed 20-character white status line:

```text
Keyer [keyIn] [keyOut] [WPM]
```

Lines 1-5 show decoded key input history in green. Line 6 shows the keyboard TX
buffer, selected message, or a short status such as `Mute:ON`.

Keyer shortcuts:

```text
Tab       enter/exit Tune mode
Alt       toggle M1-M5 shortcut overlay
1..5      select/send M1-M5 while Alt overlay is active
]         raise keyer WPM
[         lower keyer WPM
\         toggle Keyer Mute ON/OFF
Enter     send pending keyboard text or selected message now
Backspace edit pending keyboard text
Hold Backspace clear decoded history and TX buffer
`         cancel current TX/playback
```

Mute suppresses sidetone only; it does not disable keyOut. Message memories are
plain text. There is no bracket macro expansion or `qsocalls.txt` lookup.

Tune is a Keyer-only sub-mode. Press Tab from the Keyer main page to enter or
exit it. The top row changes to `Tune` and line 6 shows `Tune`, `Tune:T`, or
`Tune:Hold`. In Tune mode, paddle tip or ring true-holds tune output through the
current keyOut mode. Press `T` to start latched tune; press `T` again, press Tab,
press the paddle, or wait for `TuneTimeout` to stop it. `TuneTimeout:0` disables
the automatic timeout. Other keyboard keys are ignored while Tune is active.

Keyer settings:

```text
Page 1
1 Vol       0..99
2 Mute      ON/OFF, volatile and resets OFF on boot
3 Wpm       5..60
4 Tone      300..999 Hz
5 keyIn     Pdl, Pdl-R, SK-T, SK-R
6 keyOut    Pdl, Pdl-R, SK, SK-M, OFF

Page 2
1 M1        CQ SOTA DE AG6AQ
2 M2        TU UR CA CA BK
3 M3        BK TU 72 DE AG6AQ E E
4 M4        AG6AQ
5 M5        BK TU GM UR 599 599 CA CA BK
6 RepeatInt 1..99 seconds, used by repeating M1

Page 3
1 Paddle    IambicA, IambicB, Bug
3 txDelay   0..99 seconds
5 TuneTimeout 0..20 seconds, 0 means no timeout
```

M1 repeats at `RepeatInt` until canceled. M2-M5 are one-shot. A paddle or
straight-key press cancels active message/TX playback; the cancel element is
consumed.

### Lessons

Lessons mode is Koch-style receive practice. It generates random copy from the
active lesson character set, sends it at the configured code/effective speed,
and scores typed copy when submitted.

Settings:

```text
1 Lesson      1..40
2 Duration    1..5 minutes
3 Code WPM    5..40
4 Eff WPM     5..40, clamped to Code WPM
5 Group       Rand, or 2..7 characters
```

### Words

Words mode runs 25-word adaptive attempts from a built-in English word bank.
Correct answers raise WPM, wrong answers lower WPM, and the result tracks score,
max WPM, best score, and best max WPM.

Controls:

```text
Enter     start/check answer
.         replay current word
Backspace edit answer
`         abort
```

Settings:

```text
1 Speed      5..40
2 MinChar    5..40
3 Lesson     9..40
4 MaxLen     2..15
```

### Calls

Calls mode runs 25-callsign adaptive attempts from a prototype built-in bank.
It accepts letters, digits, and `/`. Correct answers raise WPM up to MaxWPM;
wrong answers lower WPM.

Controls:

```text
Enter      start/check answer
. or Space replay current callsign
Backspace  edit answer
`          abort
```

Settings:

```text
1 Speed      5..40
2 MinChar    5..40
3 MaxWPM     5..40
```

### Plain

Plain mode sends one randomly selected built-in plain-text message, accepts
typed copy, and scores the result with Levenshtein accuracy after whitespace
normalization.

Unlike Words and Calls, period is typed punctuation in Plain mode.

Settings:

```text
1 Code WPM   5..40
2 Eff WPM    5..40, clamped to Code WPM
```

### System

System mode contains device-wide settings and actions.

```text
1 Volume
2 KeyIn
3 KeyIn WPM
4 Sleep/Batt
5 USB Drive
6 Tone
```

`KeyIn` cycles through the available input modes, including paddle,
reverse-paddle, and straight-key modes. `USB Drive` exposes the FATFS settings
volume to a PC; eject safely, then turn USB Drive OFF or reboot so firmware can
mount the filesystem again.

## KeyIn Modes

```text
Pdl       G13/Tip = dit, G15/Ring = dah
Pdl-R     G13/Tip = dah, G15/Ring = dit
SK-T      G13/Tip straight key input, ignore ring
SK-R      G15/Ring straight key input, ignore tip
```

## KeyOut Modes

KeyOut uses active-low open-drain outputs: G3 is tip and G6 is ring.

```text
Pdl       dit to G3/Tip, dah to G6/Ring
Pdl-R     dit to G6/Ring, dah to G3/Tip
SK        key both G3/Tip and G6/Ring
SK-M      key G3/Tip and hold G6/Ring low for MTR
OFF       disable keyOut GPIO activity
```

## Data And Persistence

Lessons, Words, Calls, and Plain currently use firmware-generated or
firmware-built-in practice data.

Settings are saved in FATFS as `/fatfs/setting.txt`. Saved settings include
system volume/tone/keyIn/WPM; Keyer keyOut, paddle, txDelay, TuneTimeout,
RepeatInt, and M1-M5; and trainer mode configuration. Keyer Mute is
intentionally not persisted.

## Current Limitations

```text
No file-backed word/callsign/plaintext lists yet
Trainer result persistence is not enabled yet
No CW decoder display yet
No QSO/logging/statistics mode yet
Callsign bank is prototype data and will be replaced or generated later
```
