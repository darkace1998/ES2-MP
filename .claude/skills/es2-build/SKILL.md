---
name: es2-build
description: Build and deploy the Everspace 2 co-op mod DLL (dwmapi.dll proxy). Use whenever mod C++ under mod/src changed and you need it running in the game — compiles with zig for x86_64-windows-gnu and copies into the game folder. Triggers: "build the mod", "rebuild", "deploy the dll", "compile es2coop".
---

# Build & deploy the ES2 co-op mod

The mod is a Windows x64 DLL built **from Linux with zig** and loaded into the game as a `dwmapi.dll` proxy.

## Steps
1. Build: `scripts/build.sh` (release) or `scripts/build.sh debug`.
   - Output: `mod/build/dwmapi.dll` (+ `.pdb`). On failure it prints errors and the full log path (`mod/build/build.log`); fix and rerun.
   - The script filters libc++ build noise; only real `error:`/`warning:` lines are shown.
2. Deploy into the game: `scripts/deploy.sh` (atomic copy — safe while instances run; they keep the old mapped copy until restarted).
3. To pick up changes you must **restart the game instances** (`scripts/kill.sh` then relaunch — see `es2-launch`). A running instance keeps the previously-loaded DLL.

## Notes
- Sources are `mod/src/*.cpp` + vendored MinHook in `mod/third_party/minhook`. Headers `sdk/gen/*.h` come from the PDB (see `es2-sdkgen`).
- zig lives at `$HOME/.local/opt/zig/zig` (override with `ZIG=...`).
- New engine functions/offsets → add them in `tools/gen_sdk.py` and run `es2-sdkgen` before building.
- The mod refuses to init if the game's PE timestamp ≠ the value baked into `sdk/gen/rvas.h` (guards against a silent game update). If that fires, run `es2-sdkgen`.
