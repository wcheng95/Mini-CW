# Make an Adapter

* KeyIn: G13-Tip, G15-Ring — to paddle
* KeyOut: G3-Tip, G6-Ring — to radio
* Debug: G4-TX, G5-RX — not required for normal use

# Mini-CW User Manual

Mini-CW is a CW/Morse practice and keyer project for the M5 Cardputer ADV. This manual describes the current early development version.

---

## 1. Screen Layout

The screen uses a compact text layout.

```text
Top bar
Green separator
Line 1
Line 2
Line 3
Line 4
Line 5
Bottom bar
```

Current layout:

```text
Keyer         700 40
--------------------





Paddle   Paddle   20
```

---

## 2. Top Bar

Top bar format:

```text
<mode:13> <tone:3> <vol:2>
```

Example:

```text
Keyer         700 40
```

Meaning:

```text
Keyer = current mode
700   = sidetone frequency in Hz
40    = volume
```

Tone and volume are global audio settings.

---

## 3. Middle Lines

The five middle lines are reserved for mode display, menus, or future decoded CW text.

For now, in Keyer mode, these lines are kept blank.

CW decoder display will be added later.

---

## 4. Bottom Bar

Bottom bar format:

```text
<KeyIn:8> <KeyOut:8> <KeyIn WPM:2>
```

Example:

```text
Paddle   Paddle   20
```

Meaning:

```text
KeyIn     = input mode
KeyOut    = output mode
KeyIn WPM = paddle/input timing speed
```

The bottom bar is drawn in cyan.

---

## 5. Modes

Current planned modes:

```text
Practice
Keyer
Lessons
```

For now, Keyer mode is the main working mode.

---

## 6. Global Setting/Menu

Press **Ctrl** to enter or exit the global setting/menu.

The global setting/menu uses the same style as Mini-FT8:

```text
1-5       select or edit the visible item
;         page up
.         page down
,         previous / decrease
/         next / increase
Ctrl      exit global setting/menu
```

### Global Menu Page 1

```text
1 Mode:Keyer
2 Tone:700Hz
3 Volume:40
4 KeyIn:Paddle
5 KeyIn WPM:20
```

### Global Menu Page 2

```text
1 KeyOut:Paddle
2 KeyOut WPM:20
3 Sleep/Batt 90%
4 Date
5 Time
```

---

## 7. Local Mode Setting/Menu

Press **Fn** to enter or exit the local setting/menu for the current mode.

This is separate from the global menu.

For now:

```text
Practice mode: no local settings
Keyer mode: local settings may be added later
Lessons mode: local settings may be added later
```

---

## 8. KeyIn Modes

Current KeyIn options:

```text
Paddle
PaddleR
SK
SK-Mono
```

### Paddle

```text
G13 / Tip  = dit
G15 / Ring = dah
```

### PaddleR

Reverse paddle mode:

```text
G13 / Tip  = dah
G15 / Ring = dit
```

### SK

Straight-key mode.

```text
G13 / Tip = straight key input
```

When G13 is held down, the sidetone stays on.

### SK-Mono

Mono straight-key mode.

```text
G13 or G15 = straight key input
```

Either input can key the sidetone.

---

## 9. Iambic-B Paddle Behavior

The current Keyer mode implements Iambic-B behavior for paddle input.

Use KeyIn WPM to control timing.

Morse timing:

```text
dit length = 1200 / WPM milliseconds
dah length = 3 * dit length
gap        = 1 * dit length
```

Example at 20 WPM:

```text
dit = 60 ms
dah = 180 ms
gap = 60 ms
```

---

## 10. Adjusting Global Settings

Enter the global menu with **Ctrl**.

Example:

### Change Tone

```text
Ctrl
go to Page 1
select item 2 Tone
use , or / to decrease/increase
Ctrl to exit
```

---

## 11. Current Limitations

This is an early development version.

Current limitations:

```text
No CW decoder display yet
No text buffer display yet
No FATFS setting.txt persistence yet
No USB flash-drive mode yet
No QSO/logging features yet
No full user manual for all future modes yet
```

---
