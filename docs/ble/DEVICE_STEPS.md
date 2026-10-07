# BLE MIDI test: device steps

Each step is one package, installed by the user from the previous one through the Felucca web installer. Every
package passed, before install:
- the loader hash (e671adf0…);
- gate 2a/2b (update path unchanged);
- gate 4 (no BT start-up at boot);
- the host tests.

The way back in every step: OCT- + OCT+ held 5 s gives UBOOT at once (no countdown). If the app does not start,
use the UBOOT recovery (Hortator `docs/RECOVERY.md`).

| Step | Build | Package sha256 (first 16) | What to check | Result |
| --- | --- | --- | --- | --- |
| A | `FELUCCA_BLE_STEP=1` (linked, never started) | f4fc6aabded6b3bb | home screen; then install B from it | pending |
| B | `FELUCCA_BLE_STEP=2` (start, scan) | eddb600b263ec96b | PLAY: `STARTING` → `IDLE`; PLAY again: `REPORTS` counts up; console `ble` | pending |
| C | `FELUCCA_BLE_STEP=3` (list) | 103f82b4ad38d610 | the SMC-Mixer in the list with `MIDI` | pending |
| D | `FELUCCA_BLE_STEP=4` (connect) | f2950bb00622a6ae | SELECT it, REC: the state reaches `SUBSCRIBED` | pending |
| E | `FELUCCA_BLE_STEP=5` (monitor, USB) | 43a2ece3c5fa3bf0 | move a fader: `CC` lines on screen and in a Mac MIDI monitor | pending |

## Results
