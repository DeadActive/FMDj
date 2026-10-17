# FMDj: saving to flash and kits: device check

Firmware: `build/lsdj-private.fwsc` (private: it holds your LSDj ROM). Pages, served from this Mac:
installer `http://localhost:8004/webapp/installer/`, songs `http://localhost:8004/webapp/lsdj/`. Use Chrome.
Controls: `docs/lsdj/CONTROLS.md`.

**Before the first save:** LSDj's save goes into the part of the FM-1's flash where Hortator keeps its user samples
(USR1..USR3). They are lost at the first save. Hortator's projects, presets and settings are not touched. If you want
those samples, take them off the FM-1 with Hortator first.

1. **Install and start.** Install from the installer page, press PLAY. Expected: LSDj with the demo song, as before
   (nothing is saved yet, and starting writes nothing).
2. **Controls.** Turn SELECT: the cursor moves left / right. Turn ALGORITHM: up / down (clockwise is down). Hold the
   top F and turn SELECT: the value under the cursor changes. Top G is B. SEQ is SELECT, PLAY is START.
   Turn PRESETS: LSDj's screens change left / right. OCT+ / OCT-: screens up / down.
3. **Save with REC.** Change something in the song, hold REC for a second. Expected: the status line says SAVING,
   then SAVED and a size; the picture and sound stop for a second or two. Please note how long.
4. **Power off and on, PLAY.** Expected: your change is there.
5. **Save from the page.** Connect the songs page. Expected: a line "Flash: the save takes N of 116 KiB. Kit banks:
   0 of 12". Upload a song (stock kits). Expected: "Uploaded ...", no "NOT saved". Power off and on, PLAY, LOAD the
   song in LSDj: it is there.
6. **A song with a custom kit.** Drop a `.lsdprj` that brings its own kit. Expected: the FM-1 makes a kit bank (a
   few seconds: the save is written again), the kit goes over (about 15 s a kit), the page lists it under "Kit
   bank 1", and LSDj starts again. Load the song: its kit instrument plays the
   kit; in LSDj's instrument screen the kit shows by its name. Power off and on: still there.
7. **Remove the kit.** Press Remove on its row. Expected: the row says (free). "Give unused kit banks back to the
   save" then returns the room.
8. **A save that does not fit** (optional, if you have a big `.sav`): with several kit banks, restore a save with
   many songs. Expected: the page says it is NOT saved on the FM-1 and why; LSDj still has it until power-off.
9. **Back to Hortator** (optional): install Hortator. Expected: it starts normally; projects, presets and settings
   as before; the user-sample slots show empty. Installing the LSDj build again: PLAY loads your LSDj save, unless
   Hortator wrote user samples in between.
10. **The way back.** The installer page still installs, and OCT- + OCT+ held 5 s enters UBOOT.

Tell me what each step did. From the console (`screen /dev/cu.usbmodem*`): `ble info` shows the flash state
(`kits`, `cap`, `used`, `saved`), `ble st` the result of the last flash request.
