# LSDj on the FM-1: controls

| Game Boy | FM-1 |
|---|---|
| LEFT / RIGHT | SELECT knob (a step = a press; clockwise = RIGHT). Also the HOME / ARP buttons, which can be held. |
| UP / DOWN | ALGORITHM knob (clockwise = DOWN). Also the SCL / SAVE buttons, which can be held. |
| A | the top F of the keyboard (F5) |
| B | the top G of the keyboard (G5) |
| START | PLAY (the first press after power-on starts LSDj) |
| SELECT | SEQ |

Shortcuts for LSDj's screens:

| LSDj | FM-1 |
|---|---|
| SELECT + LEFT / RIGHT | PRESETS knob |
| SELECT + UP | OCT+ |
| SELECT + DOWN | OCT- |

Editing as in LSDj: hold A (the top F) and turn SELECT to change a value by one, ALGORITHM by an octave or 0x10.

The FM-1's own: MASTER is the volume; FX switches drawing every frame / every second frame, LFO the fast / slow
ROM read (both shown on the status line); OCT- and OCT+ held together for 5 s enter UBOOT.

**REC held for a second saves to the FM-1's flash** (the status line: SAVING, then SAVED and the size; SAVE FULL if
it does not fit). PLAY after power-on loads that save. GLO, EDIT and ENV do nothing.

A knob turned fast is followed at up to 30 presses a second; no more than 16 steps wait.
