# FMDj: song transfer: device check

Firmware: `build/lsdj-private.fwsc` (private: it holds your LSDj ROM). Pages, served from this Mac:
installer `http://localhost:8004/webapp/installer/`, songs `http://localhost:8004/webapp/lsdj/`. Use Chrome.

Nothing is saved on the FM-1 yet (that is the next step): what you upload lasts until power-off.

1. **Install.** Open the installer page and install. The FM-1 restarts into the FMDj start screen (FMDJ).
2. **Start LSDj.** Press PLAY. Wait for the song screen.
3. **Connect.** Open the songs page, press Connect, choose the FM-1 in Chrome's list (connecting takes a second or
   two). Expected: "No saved songs."
   and "0 of 32 songs, 191 of 191 blocks free." While the page reads, the status line on the FM-1 shows `USB`
   for a moment and the sound pauses.
4. **Save a song in LSDj.** PROJECT (SELECT + UP), LOAD/SAVE SONG, SAVE, choose slot 00, OK. Press Refresh on
   the page. Expected: slot 00 in the list.
5. **Download it.** Press Download. Expected: a `.lsdprj` file in Downloads. If you have LSDPatcher: it opens there.
6. **Upload it back.** Drop that file on the page. Expected: "Uploaded ...", LSDj starts again by itself, the list
   shows two songs (slots 00 and 01).
7. **Load it in LSDj.** PROJECT, LOAD/SAVE SONG, LOAD, slot 01. Press START. Expected: the same song plays.
8. **A song from elsewhere.** Drop any `.lsdsng` or `.lsdprj` you have. Expected: it uploads if its kits are
   LSDj's own; a song with custom kits is refused with their number (custom kits arrive with saving to flash).
9. **The whole save.** Press "Download the whole save (.sav)". Expected: a 131,072-byte file. It is not quick: the
   FM-1 answers one console line every 15 ms, so expect about 10 to 15 seconds, with the sound paused. Power the
   FM-1 off and on, press PLAY, connect again, "Restore a save…", choose that file. Expected: your songs are back,
   the edited song too. A restore sends only what differs, 20 bytes a line: up to a minute or more. Please note
   both times.
10. **Unplug in the middle** (optional): start uploading a song and pull the USB cable. Expected: the page reports
    an error or that the FM-1 was disconnected; after about 5 seconds LSDj runs again; after reconnecting, the list
    shows the songs that were there before. Do not try this with "Restore a save…": a save cannot be replaced in one
    step, a restore that stops leaves a mixed save, and the page then says "incomplete: restore it again".
11. **The way back.** The installer page still installs (try it once more), and OCT- + OCT+ held 5 s enters UBOOT
    (no countdown in this build).

Tell me what each step did. Useful extras from the console (`screen /dev/cu.usbmodem*`): `ble info`, `ble`.
