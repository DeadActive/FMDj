# FMDj: checking the sound timing and the speed

What changed, and why, is in `docs/lsdj/SOUND_TIMING.md`. The status line and the four switches are in
`docs/lsdj/CONTROLS.md`.

Play a song with drums or a fast arpeggio: the difference is in how even the beat is.

1. **PLAY** starts LSDj; **PLAY** again plays the song.
   Expect on the status line: `SPD 100%`, a `LOAD` clearly lower than the 90 % it was, and `GAPS 0` on the right.

2. **Listen for half a minute.** Expect an even beat, with no stutter.

3. **ENV** switches to the old sound (the status line shows `OLD`); **ENV** again switches back.
   Expect the old sound to be the uneven one. Switch a few times while the song plays.

4. **EDIT** runs LSDj's wait loop again (the status line shows `SPIN`); **EDIT** again switches back.
   Expect `LOAD` to rise to about the old value with `SPIN`, and the sound to stay the same.

5. **Move through LSDj's screens while the song plays** (PRESETS knob, OCT- / OCT+), then leave it playing for a few
   minutes.
   Expect `GAPS` to stay where it is. Each second with a dropout adds one.

What helps most if something is off: the `LOAD` with and without `SPIN`, the `GAPS` number after a few minutes, and
whether a stutter you still hear comes with `GAPS` going up or not.
