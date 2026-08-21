<p align="right"><a href="README-TC.md">中文說明</a></p>

# CyberpunkVR Port - Tofu Express X

**Tofu Express X** is an enhanced version of CyberpunkVR Port. Its main goal is to make the whole of *Cyberpunk 2077* playable entirely in VR as soon as possible, without VR issues forcing players back to flat-screen mode, while continually polishing controls, visuals, and the overall experience.

The original CyberpunkVR Port by [dariulone](https://github.com/dariulone) is the best *Cyberpunk 2077* VR mod available today. It offers excellent stereo rendering and performance, backed by a strong technical foundation with plenty of room to grow. It is well worth helping maintain and improve, and I will continue sharing improvements from Tofu Express X with the original author so that more players can benefit.

## Support for Every Control Style

The mod is not limited to VR controllers. If you prefer a gamepad or even keyboard and mouse, you can choose whichever control scheme suits you best.

## VR Controller Improvements

- **Complete mapping for every Xbox controller button.** The inputs missing from the original mod have been added, so every Xbox controller button can now be accessed through VR controllers.
- **Flexible chord activation.** Choose L3, R3, or the Right Thumbrest touch sensor according to your preference, while retaining the normal in-game functions of L3 and R3.
- **Optional chord shortcuts for VR recentering and the F10 menu.** A keyboard is no longer required for these actions.
- **Context-sensitive Right Grip routing.** This fixes the original mod's missing RB input: use immersive draw/holster actions in the shoulder or hip holster zones, or send RB everywhere else. Holster gestures are disabled while driving to prevent accidental activation.
- **Automatic Trigger/Grip swapping while driving.** Right Trigger can remain Fire both on foot and in vehicles, while the analog VR grips control acceleration and braking. Physical Xbox controller input is unaffected.

[Watch the vehicle combat demonstration on YouTube](https://www.youtube.com/watch?v=n6bx6JbvSgs)

- **A toggle for full-stick sprint and crouch.** The original mod always sprints when the left stick is pushed fully forward and crouches when the right stick is pushed fully down. These behaviors can now be enabled or disabled from the F10 menu.

## View Control and More Reliable Weapon Aiming

- **Fixed vertical view control.** Turning off **Disable Mouse Y** now correctly restores vertical view control through the mouse or right stick, giving advanced players more control.
- **Decoupled VR Head Aim** is designed for players who prefer a gamepad. Unlike traditional head aiming—sometimes jokingly called "gun-face"—head and body rotation are independent. You can freely aim the weapon with your head without changing the body's facing direction.
- **Bullets now originate from the live muzzle.** Head Aim no longer uses the original "shooting with your eyes" calculation. Both Head Aim and Hand Aim now use the weapon's current muzzle position and direction, so the laser dot, weapon sight, and actual point of impact share the same firing reference.
- **Hip-fire and ADS aim stay aligned.** Raising the weapon into ADS no longer shifts it away from the original aiming point. Even with VRIK disabled, the mod tries to preserve the aiming direction from before entering ADS.
- **The external muzzle dot is more stable during fast head turns.** Long trails and multiple separated dots are greatly reduced.
- **Weapon sights stay aligned with the external dot.** Reflex reticles, sniper-scope crosshairs, the external muzzle dot, and the magnified ADS view no longer drift apart as the head turns.
- **ADS uses the right eye as the aiming eye.** When using Head Aim instead of Hand Aim, ADS automatically moves the weapon sight to the right eye rather than leaving it between both eyes. This is especially noticeable at high magnification.
- **Physical body rotation has been rewritten.** The character now follows head rotation without the uncomfortable view jumps caused by the original implementation. It also preserves `45°` of free look to either side, so turning your head does not directly lock the body's forward movement direction.

## Controller Setup

Press **F10**, open **Controls**, and adjust the options to your preference.

![New controller settings in the F10 menu](images/v0.1.1-controller-enhancements.png)

### Emulate D-pad and Additional Controls

Choose a **Chord Activation Method**. The original L3 method is selected by default:

| Activation method | D-pad | Back / Select | VR Recenter | F10 Menu |
|---|---|---|---|---|
| Hold **L3** (left thumbstick click) | Right stick | Left Menu | A | B |
| Hold **R3** (right thumbstick click) | Left stick | Left Menu | X | Y |
| Touch **Right Thumbrest** | Left stick | Left Menu | X | Y |

Hold the activation control first, then move the stick or press another button. L3 and R3 retain their normal functions.

Left Menu sends Start; chord + Left Menu sends Back / Select. D-pad and Back / Select are always enabled. **Extra Chord Actions**, enabled by default, adds shortcuts for VR Recenter and the F10 menu.

**Right Thumbrest** is the touch-sensitive area beside the face buttons where your thumb naturally rests. It can activate the chord without being clicked. Only some VR controllers support it, including Quest 3; use L3 if your controller does not.

The original keyboard shortcuts remain available: **F7** for VR Recenter and **F10 / Insert** for the menu.

### Right Grip and Visual Holsters

- Press Right Grip inside a shoulder or hip holster zone: draw or holster a weapon.
- Press Right Grip anywhere else: send **RB**.
- In a vehicle: holster gestures are disabled.

### Swap Triggers / Grips While Driving

Enable this option to keep firing on Right Trigger while driving:

| VR input | Emulated gamepad input |
|---|---|
| Left Trigger | LB |
| Right Trigger | RB |
| Left analog Grip | LT |
| Right analog Grip | RT |

The analog grips become brake and accelerator. Only VR controller input is swapped; physical gamepads are unaffected. This option is **off by default**.

### Other Options

| Setting | Behavior |
|---|---|
| **Disable Mouse Y (Pitch)** | On: vertical view follows the HMD only. Off: mouse and right-stick vertical view are enabled. |
| **Full-stick Sprint / Crouch** | Push the left stick fully forward to Sprint or the right stick fully down to Crouch. Enabled by default. |

## Original Author's Documentation

The original author's README continues below.

---

A 6-DoF **VR mod for Cyberpunk 2077**, built as a **RED4ext plugin** — there is no
`dxgi.dll` proxy any more. `CyberpunkVR_Stereo` drives OpenXR head tracking, real
stereo and the in-headset overlay; `CyberpunkVR_Hands` drives a **full-body VR
avatar with motion-controlled hands**; and a set of CET / redscript mods add VR
weapon aiming, motion melee, hand-to-holster equipping, a VR-friendly HUD and
more. Everything is configured from an in-headset **F10** overlay.

Repository: <https://github.com/dariulone/cyberpunk-vr-port>

> ⚠️ Experimental community mod. Not affiliated with CD PROJEKT RED. Use at your
> own risk and keep backups of your saves.

## Features

- **Real stereo, not reprojection.** The second eye is an actual engine view — a
  render-to-texture camera on the player entity that runs the frame graph for its
  own eye, from its own position, with its own projection. It falls back to mono
  automatically whenever that view has nothing fresh to give (menus, loading).
- **OpenXR head tracking** injected into the REDengine render path, with the
  submitted frustum matching the one the engine actually rendered on both axes,
  plus world-scale / IPD controls.
- **The game HUD in both eyes** — the engine's own HUD composite is ported
  shader-for-shader for the second eye, and placed at a finite distance so icons
  fuse instead of splitting.
- **Full-body VR avatar** (VRIK) — body under the HMD, arm-length calibration,
  leg IK, real-life squat. Hands are with the controllers.
- **Decoupled VR weapon aim** — bullets follow the real weapon muzzle, not the
  camera; optional barrel dot in both eyes, scope-zoom aware.
- **Collimated reflex sights** — the reticle is placed by angle along the sight's
  own optical axis, so it stays on the bore instead of sliding across the glass
  when you look at the sight from the side.
- **VR motion melee** — real swings trigger the game's native melee along the
  blade (native damage/reaction/stamina).
- **Hand-to-holster** equip/unequip on a grip squeeze — *immersive* (by visual
  holster) or *simple* (fixed weapon slots).
- **VR smoking** — cigarette and lighter as real props, with a captured
  finger grip, a hands-free mouth anchor and the game's own FX and audio.
- **VR controller mapping** merged into XInput: full-forward = sprint,
  full-down = crouch, snap or smooth turn, HMD/hand-relative locomotion, D-pad
  chord.
- **VR HUD** with per-element placement & scale, **world-map head-lock**, CAS
  sharpening, and DLSS/NGX handling (the second view gets its own upscaler
  viewport automatically).
- **In-headset F10 overlay** with tabbed, live, persisted settings.
- SteamVR (OpenVR) runtime supported alongside OpenXR; pre-launch resolution
  selector; quiet-by-default logging with a DEBUG toggle in the launcher.

See [`docs/`](docs/) for engineering notes, and
[`docs/RELEASE-0.1.0.txt`](docs/RELEASE-0.1.0.txt) for how the stereo path is
actually built.

## Requirements

- Cyberpunk 2077 (PC, 2.31).
- Cyber Engine Tweaks
- RED4ext
- ArchiveXL
- TweakXL
- redscript
- Codeware (**1.20 or newer** — older builds fail script compilation)
- Visual Holsters (Automatic Clothes Swap)
- Visible Bullets (Projectile Restoration)
- Equipment-EX
- Nova Optics

Install RED4ext, CET and redscript first (the usual Nexus dependencies).

## Installation (drop-in)

Download the release archive and extract its contents into your **Cyberpunk 2077
game root** (the folder that contains `bin\`, `r6\`, `red4ext\`). The files land
as:

```
red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_Stereo.dll     # the VR plugin: OpenXR, stereo, overlay
red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_Sight*.dxil    # sight shaders, loaded by name at PSO swap
red4ext\plugins\CyberpunkVR_Hands\CyberpunkVR_Hands.dll       # native plugin (avatar/hands, weapon aim, shared bridge)
bin\x64\plugins\cyber_engine_tweaks\mods\CyberpunkVRPort_*\   # CET mods: Stereo (VRCAM select), HUD, Holster, VRIK, Weapon, WorldMap
r6\scripts\CyberpunkVRPort_*\                                 # redscript: HUD, Holster, Melee, NoAnims, WeaponUp, WorldMap
```

Then **start your OpenXR runtime first**, and launch the game.

> There is no `dxgi.dll` any more — this is a RED4ext plugin. Anything else that
> proxies dxgi (R.E.A.L. VR, for one) must be out of `bin\x64` or the two fight
> over the same engine hooks; `scripts\deploy_stereo.ps1` moves one aside for you.

> Keep only one `.dll` in each `red4ext\plugins\CyberpunkVR_*` folder. RED4ext
> loads **every** DLL it finds there, so a renamed backup beside the real build
> loads as a second copy of the plugin and the two fight over the same hooks.

From a source tree, install with:

```
cmake --build build --config Release --target cyberpunkvrport_stereo
pwsh scripts\deploy_stereo.ps1 -GameRoot "<game root>"
```

## Controls

VR controller input is merged into the native CP2077 gamepad, so the in-game
"Controller" key bindings apply. Default VR mapping:

| Input | Action |
|---|---|
| Left stick | Walk / strafe — **push fully forward = sprint** |
| Right stick X | Turn camera (snap or smooth) |
| Right stick **fully down** | **Crouch** (R3) |
| Right trigger / Left trigger | Fire / Aim |
| Right grip | Hand-to-holster equip / unequip; melee power modifier |
| Left grip | Crouch (shoulder) |
| A / B | Jump / Dodge |
| X / Y | Reload·interact / Weapon switch |
| Right thumb click | Crouch (R3) |
| Left menu button | Pause menu |
| Swing a melee weapon | VR motion melee (native attack along the blade) |

**D-pad chord.** Hold the **left stick clicked in**, then pick the direction with
the **right stick** — up / down / left / right. While the chord is held the right
stick is taken out of the camera, so selecting a direction cannot snap-turn you.
Release the left stick *without* having chosen a direction and it emits the normal
L3 (sprint) press instead, so nothing is lost by using it.

Buttons follow each runtime's interaction profile (Touch / Index / Vive / WMR);
customise the actual actions in the game's *Settings → Key Bindings → Controller*.

Hotkeys:

- `F7` — recenter HMD
- `F10` / `Insert` — open the in-headset settings overlay

## In-headset overlay (F10)

Five tabs, live, and saved to `vrport.ini` — nothing here needs a restart.

- **General** — world scale, IPD scale, stereo separation, VR menu FOV and quad
  size, motion prediction, reuse-last-clean-frame, pose pair-lock, and the head
  offset (X right / Y forward / Z up).
- **Controls** — decoupled weapon aim and its laser dot, locomotion source
  (Game / HMD / left hand / right hand), snap turn and angle, immersive holsters.
- **Stereo** — the second eye itself: which eye VRCAM is sent to, how stale its
  last frame may get before the submit falls back to mono, the HUD composite, and
  the live counters that say whether the second view is producing, being captured
  and reaching the headset.
- **VRIK** — start/stop tracking, IK calibration (reach scale, height, elbow
  swing/pole, wrist offset), diagnostics.
- **HUD** — per-element X / Y / scale for every HUD group.

The launcher (before the game starts) picks the render resolution and carries a
**DEBUG** tick-box that arms every diagnostic probe at once. Leave it off for
play: it is for diagnosis and it costs both frame time and a very large log.

## Mod components

| Component | Type | Purpose |
|---|---|---|
| `CyberpunkVR_Stereo.dll` | RED4ext plugin | OpenXR head tracking, the second engine view, HUD composite, sight shaders, F10 overlay, XInput merge |
| `CyberpunkVR_Hands.dll` | RED4ext plugin | Full-body avatar / hand IK, weapon-aim orientation override, smoking poses, shared-memory bridge |
| `CyberpunkVRPort_Stereo` | CET | Enables the VRCAM component the launcher picked |
| `CyberpunkVRPort_VRIK` | CET | Starts hand tracking, bridges calibration |
| `CyberpunkVRPort_Weapon` | CET | Decoupled weapon aim + VR motion-melee detection |
| `CyberpunkVRPort_Holster` | CET + reds | Hand-to-holster equip/unequip (immersive / simple) |
| `CyberpunkVRPort_Smoking` | CET + reds | Cigarette / lighter props, FX, audio, auto-puff |
| `CyberpunkVRPort_HUD` | CET + reds | VR HUD layout |
| `CyberpunkVRPort_WorldMap` | CET + reds | World-map head-lock |
| `CyberpunkVRPort_Melee` | reds | Native melee along the blade segment |
| `CyberpunkVRPort_WeaponUp` | reds | Stops auto-lower / auto-unequip of drawn weapons |
| `CyberpunkVRPort_NoAnims` | reds | Disables VR-fighting animations (keeps gameplay systems) |

## Logs

- `Cyberpunk 2077\bin\x64\cyberpunkvrport.log` — the plugin's own log, and the
  right file for a bug report. Quiet by default; tick **DEBUG** in the launcher
  for per-frame diagnostics.
- `Cyberpunk 2077\red4ext\logs\` — script validation and plugin load errors. If
  redscript compilation fails, *every* redscript mod is off, not just the one that
  failed, so check here first when something stops working all at once.
- Per-mod CET logs live in each mod folder; they follow the same DEBUG switch.

## Test hardware used during development

- Headset: PICO 4 (via VDXR)
- CPU: AMD Ryzen 7 5800X
- GPU: NVIDIA RTX 5070 Ti
- RAM: 32 GB DDR4
- OS: Windows 11 Pro 25H2 (26200)

## Donations

Donating is your personal choice. It speeds up development and makes new features
possible — nobody is forcing you to do it.

- <https://boosty.to/dariulone>
- <https://dalink.to/dariulone>

| | |
|---|---|
| USDT TRC20 | `TRgmDeRcFumXvsSRqYV5kQAqRAvoFKXJCt` |
| USDT BEP20 | `0x4638c6580d1e684bdc60a1c415e5cb1522b66942` |
| TRX | `TRgmDeRcFumXvsSRqYV5kQAqRAvoFKXJCt` |
| BTC | `13AfpBwZvaezf36FmpjtENHTXjYcnzEsze` |
