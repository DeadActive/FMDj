# FMDj: checking the install page

The page: `http://localhost:8004/webapp/lsdj-install/` (Chrome or Edge). What it does is in
`docs/superpowers/specs/2026-10-13-lsdj-browser-install-design.md`.

What is already proven on the Mac: from your ROM, the page's code makes the same package, byte for byte, as the Mac
build (`tests/lsdj/lsdj_install_test.py`). What only the FM-1 can show is below.

1. **Open the page.**
   Expect: "Firmware ... loaded. Choose your LSDj ROM file (.gb)."

2. **Choose a file that is not a ROM** (any other file).
   Expect: a line saying it is not an LSDj ROM, and Install stays grey.

3. **Choose your LSDj ROM.**
   Expect: the Game Boy's screen appears, LSDj starts and the demo song is typed in while a bar fills, for some
   seconds; then "it runs. Its ... bytes fit ... Ready to install."

4. **Install**, with the FM-1 on USB.
   Expect: Starting, Verifying, Switching to update mode, Writing, Restarting, then "Done".

5. **On the FM-1: PLAY, then PLAY again.**
   Expect: LSDj starts, and the demo song plays. If a save is in the FM-1's flash from before, PLAY loads that one
   instead of the demo.

6. **The song page** (`../lsdj/`): Connect.
   Expect: your songs and kits as before.

Not needed, but it shows the firmware alone: the plain installer (`webapp/installer/`) with the base package
would give an FM-1 whose PLAY says `NO LSDJ ROM`.
