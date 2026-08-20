---
name: es2-debug
description: Read Everspace 2 co-op mod logs and diagnose crashes (symbolized callstacks) when the game closes or misbehaves during testing. Use after an instance dies, when a hook misfires, or to watch the mod's runtime output. Triggers: "why did it crash", "show the log", "symbolize the crash", "the game closed", "read the callstack".
---

# ES2 logs & crash diagnosis

## Mod logs
- Latest log, last N lines: `scripts/logs.sh [N]`  ·  follow: `scripts/logs.sh -f [N]`.
- Logs live in `<game>/ES2/Binaries/Win64/ES2Coop/logs/es2coop-<pid>.log` (one per instance). Filter with `grep '\[net\]'` / `grep '\[coop\]'`.
- The mod also writes to `OutputDebugString` (visible under WINEDEBUG=+debugstr if needed).

## Crashes
- Symbolized newest crash: `scripts/crash.sh [N]` — prints ErrorMessage, SecondsSinceStart, and a callstack where `ES2 +off` frames are resolved to functions and `MOD +off` frames to mod symbols.
- Raw crash dumps: `$ES2_SAVED/Crashes/UECC-*` (source `scripts/proton-env.sh` for `$ES2_SAVED`).
- Proton stdout/stderr per instance: `run/<name>.proton.log`.

## Mod-specific crash signatures already seen (and fixed)
- `Maximum number of UObjects exceeded` while a client joins -> a hook re-entered engine code that calls
  back into the hooked function (the GetCurrentShip/CreateShipDataFromState recursion). Add a re-entrancy guard.
- Fault inside `AESHUD::Tick` on the client -> the local player had no pawn (respawn/death/travel gap).
- Fault inside `FURL::FURL` during `UGameEngine::Tick` -> a level was opened without ES2's
  `ChangeLocation_Internal` having updated PlayerData first.
- Heap corruption in `FMallocBinned2` -> almost always a wrong parameter ABI: MSVC x64 passes
  `FString`/`TSubclassOf` "by value" params INDIRECTLY, and large struct returns use a hidden pointer
  (`(this, sret)` for member functions, `(sret)` for static ones).

## Common failure signatures (see the es2-modding-facts memory)
- `Pure virtual not implemented (UNetConnection::LowLevelGetRemoteAddress)` → you called a pure-virtual by its base RVA; call it through the vtable slot instead (`tools/pdb_types.py vtable`).
- `dwmapi + <off>` in a crash → symbolize with `python3 tools/symbolize_dll.py mod/build/dwmapi.pdb 0x<off>`.
- Instance won't start, `c0000135` in `run/<name>.proton.log` → path/DLL issue; ensure the `~/es2game` symlink exists and `WINEDLLOVERRIDES` is set (launch via the scripts).
- Access violation right after a hook installs → the hooked RVA may be ICF-folded or the arg ABI is wrong (structs >8 bytes / `TSubclassOf` pass by hidden pointer).
