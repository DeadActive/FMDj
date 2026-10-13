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

The FM-1's own: MASTER is the volume; OCT- and OCT+ held together for 5 s enter UBOOT.

## The status line

Left: **SPD** is LSDj's speed against a Game Boy (100 % = real time). **LOAD** is how much of its time the FM-1
needs for the Game Boy: under 100 % it keeps up, and the lower the more is spare.

Right, with no key held: **GAPS** counts the seconds so far in which the sound ran dry, because the FM-1 was too
slow just then. A gap is heard as a short stutter. While the number stands still, nothing was lost.

Four switches for comparing; each is named on the status line only while it is off:

| Button | Off | What it is |
|---|---|---|
| FX | FULL | every frame drawn, not every second one: a smoother screen, a higher LOAD |
| LFO | SLOW | the plain ROM read |
| EDIT | SPIN | LSDj's wait loop is run instruction by instruction: the LOAD without the saving |
| ENV | OLD | the sound made once per Game Boy frame, as before: notes up to 17 ms off their beat |

**REC held for a second saves to the FM-1's flash** (the status line: SAVING, then SAVED and the size; SAVE FULL if
it does not fit). PLAY after power-on loads that save. GLO does nothing.

A knob turned fast is followed at up to 30 presses a second; no more than 16 steps wait.
