# CyberpunkVR QuickBoot

Development-only CET helper for unattended renderer tests. It waits for the first main menu in a
process, then invokes the game's Continue/last-checkpoint path once. Returning to the menu later is
left alone.

`quickboot.json` controls `enabled` and the post-menu `delay` in seconds. The CET hotkey
`CyberpunkVR QuickBoot: toggle` changes the enabled state for the next launch. Runtime state is
written to `quickboot_state.txt` so a failed test can distinguish "menu never appeared" from
"Continue API changed."

For unattended runs, set `show_launcher=0` in `bin/x64/vrport-launcher.ini`.
The plugin then uses the saved resolution, HMD type and runtime. Set the value
back to `1` to show the resolution window at the next launch (the default).
`launcher_delay_ms=1000` gives startup/resize messages one second to settle when
the window is hidden; accepted range is 0..10000 ms. QuickBoot additionally waits
2.5 seconds after the first main menu before requesting the save.

QuickBoot owns the first main-menu resume only. Launch with the game's
`-skipStartScreen` argument when automating startup; loading is confirmed from
the live player/menu state, not only the `resumed` status message. Close the
game after each completed test session to release CPU/GPU/VRAM resources.

This folder is part of CyberpunkVRPort and follows the repository license. It is deliberately
excluded from tester/release packages.
