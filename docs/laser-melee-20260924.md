# Melee laser-dot visibility

The live Gorilla Arms report was reproduced on PID29948. The active item was
`Items.AdvancedStrongArmsLegendary`, item type `Cyb_StrongArms`;
`WeaponObject.IsMelee(itemID)` returned true. The native weapon class was0,
the equipped flag was1 and the barrel-dot timestamp was being published.
Read-only native evidence is in `build/laser-melee-29948/before.json` with symbols
resolved from the exact loaded DLL/PDB.

The Lua classifier now uses the game's melee predicate in addition to the
existing item-type fallback. This covers Gorilla Arms, Mantis Blades, Monowire,
fists and modded melee types without a list of localized weapon names. It runs
with the existing per-weapon classification, not as another per-frame query.
These weapons also correctly use the existing melee hand-filter class.

`DrawBarrelCrosshair` rejects melee class5. It clears the previous dot timestamp
before its early returns, so the mirror/second-eye consumers cannot retain a gun's
dot for their250ms stale-data interval after switching to melee or holstering.
The generic equipped flag still represents all equipped weapons.

Validation: Release plugin builds. `tools/weapon_tests/classification.lua` runs
the production weapon module's update callback with lexical game/native mocks
through WolvenKit, without modifying live game state. The previous Lua source
fails on Gorilla Arms (expected5, actual0); the corrected source passes22 cases,
including cyberware, conventional melee, unknown melee records, all firearm
classes, a non-melee launcher, predicate fallback and firearm selection after
melee. Physical headset validation remains for the next user launch.

Installed DLL SHA256:
`a90606f2514ebec72cc567c024a1f45173a6e79336fba37bd817250d55bf1a9d`.
Installed Weapon Lua SHA256:
`dd189ab6cd6c32ca15a3e292fdca435e6f8c69f1598ba993d4f62c949a8a036a`.
Backup/deployment report:
`build/roomscale-plugin-deploy-20260924-005453/deployment.json`.
Only the DLL and Weapon/init.lua were replaced. INI and calibration preserved;
the game was closed for installation and was not launched.
