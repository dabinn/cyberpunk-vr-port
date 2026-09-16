# Building CyberpunkVR_Stereo from source

This covers going from a clean clone of this repo to a working `CyberpunkVR_Stereo.dll`,
testing it in your own game install, and packaging a shareable release zip.

## Prerequisites (install once)

- **Visual Studio 2022**, with the **"Desktop development with C++"** workload. This gets you
  the MSVC v143 compiler and the Windows SDK — the project is built with MSVC specifically, not
  clang or gcc.
- **CMake 3.24+** — already bundled with VS2022, no separate install needed.
- **Git**.
- **Internet access** for the very first configure step (see below) — CMake fetches the OpenXR
  SDK and Dear ImGui from GitHub the first time.

## One-time setup

Run these once per clone, from the repo root (the folder containing `CMakeLists.txt`,
`src\`, `scripts\`) in PowerShell:

```powershell
git submodule update --init --recursive
cmake -B build -A x64
```

**`git submodule update --init --recursive` is not optional.** `externals\RED4ext.SDK` and
`externals\minhook` are git submodules, and a plain clone leaves both folders *empty* — the
build cannot proceed without this step. If you ever see build errors about missing RED4ext or
minhook headers, this is almost always why.

`cmake -B build -A x64` configures the project into a `build\` folder. The first run reaches out
to GitHub to pull the OpenXR SDK and ImGui sources — after that, it's cached and this step
doesn't need to touch the network again.

## Every time you change the C++ and want to test it

Two commands:

```powershell
cmake --build build --config Release --target cyberpunkvrport_stereo
pwsh scripts\deploy_stereo.ps1 -GameRoot "<path to your Cyberpunk 2077 folder>"
```

- The first line compiles **only the plugin** (there's also a test target and a separate probe
  tool in this repo you don't need — `--target cyberpunkvrport_stereo` skips those).
- The second line drops the freshly built DLL straight into your installed game so you can
  launch and try it. **Close the game first** — the script checks for a running
  `Cyberpunk2077.exe` and refuses if it finds one, since the DLL file would be locked.

This is the loop for iterating: edit `.cpp`, build, deploy, launch, test, repeat.

## Packaging a shareable release (the drop-in zip)

Once you're happy with a change and want the same folder/zip format as an official release
(FOMOD wrapper, `INSTALL.txt`, `UNINSTALL.bat`, the full `Cyberpunk 2077\` payload tree):

```powershell
pwsh scripts\build_dist.ps1 -Version "0.1.6-Tofu-Express-5-yourfix" -Zip
```

This does **not** compile anything — it only packages whatever is already sitting in
`build\bin\red4ext\plugins\CyberpunkVR_Stereo\Release\CyberpunkVR_Stereo.dll`, so always run the
`cmake --build` step above first. Pick any version string that makes sense to you for tracking
your own build; it doesn't need to match upstream's naming.

The output lands in `dist\CyberpunkVRPort-<version>\` (plus a matching `.zip` next to it since
`-Zip` was passed).

One caveat from the script's own comments: if you've hand-edited any `.yaml`, archive, or grip
pose file directly inside your **installed game folder** rather than in this repo, run
`scripts\sync_assets.ps1` first to pull those changes back into the repo — otherwise the
packaged release won't include them. Pure C++ or Lua/redscript changes inside the repo don't
need this step.

## Quick reference: which script do I run?

| I want to...                                  | Run this |
|---|---|
| Set up the repo for the first time             | `git submodule update --init --recursive` then `cmake -B build -A x64` |
| Recompile after editing C++                    | `cmake --build build --config Release --target cyberpunkvrport_stereo` |
| Try my change in my own game                   | `scripts\deploy_stereo.ps1 -GameRoot "..."` |
| Make a shareable release zip                   | `scripts\build_dist.ps1 -Version "..." -Zip` |
| Undo a deployed test build                     | `scripts\revert_stereo.ps1` |

## Troubleshooting

- **"Plugin not built" error from `deploy_stereo.ps1`** — the Release build hasn't been run yet,
  or failed. Run the `cmake --build` command above and check its output for errors.
- **"Cyberpunk2077.exe is running" error** — close the game before deploying; the DLL file is
  locked while the game holds it open.
- **Build errors mentioning RED4ext or minhook headers not found** — the submodules weren't
  initialized. Run `git submodule update --init --recursive` from the repo root.
- **`build_dist.ps1` can't find `CyberpunkVR_Stereo.dll`** — same root cause as the
  `deploy_stereo.ps1` version: build first.
