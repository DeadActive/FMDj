# FMDj: controls

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

## The menu (GLO)

**GLO** opens the menu over the lower half of the picture; GLO (or B) closes it. The Game Boy plays on while it is
open, and the keys are the menu's: UP / DOWN (the ALGORITHM knob, SCL / SAVE) choose a line, LEFT / RIGHT (the
SELECT knob, HOME / ARP) or A change it. What changed is kept in the FM-1's flash when the menu closes, and comes
back at the next PLAY.

| Line | Values (first: as it starts) | What it is |
|---|---|---|
| SCREEN | DMG, POCKET, LIGHT, SGB, DARK | the screen's colours: the first Game Boy's green, the Pocket's grey, the Light's backlit teal, the Super Game Boy's 1-A, light on black |
| STATUS BAR | OFF, ON | the line under the picture (below) |
| DRAW | EVERY 2ND, EVERY | every frame drawn: a smoother screen, a higher CPU |
| ROM READ | FAST, PLAIN | the plain ROM read |
| WAIT LOOP | SKIP, RUN | LSDj's wait loop run instruction by instruction: the CPU without the saving |
| SOUND | IN FRAME, PER FRAME | the sound made once per Game Boy frame, as before: notes up to 17 ms off their beat |

The last four were the FX, LFO, EDIT and ENV buttons; those buttons do nothing now.

## The status bar

Off at first (the menu's STATUS BAR). Left: **FPS** is the Game Boy's frames a second (59.7 is real time). **CPU**
is how much of its time the FM-1 needs for the Game Boy: under 100 % it keeps up, and the lower the more is spare.

Right, with no key held: **GAPS** counts the seconds so far in which the sound ran dry, because the FM-1 was too
slow just then. A gap is heard as a short stutter. While the number stands still, nothing was lost. After it, an
emulator option that is off is named: FULL (DRAW), SLOW (ROM READ), SPIN (WAIT LOOP), OLD (SOUND).

**REC held for a second saves to the FM-1's flash** (under the picture, with or without the status bar: SAVING,
then SAVED and the size; SAVE FULL if it does not fit). PLAY after power-on loads that save.

A knob turned fast is followed at up to 30 presses a second; no more than 16 steps wait. A screen shortcut (the
PRESETS knob, OCT- / OCT+) holds SELECT two frames before its direction: LSDj's PROJECT screen takes nothing less.
