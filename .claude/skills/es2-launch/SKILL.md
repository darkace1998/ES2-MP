---
name: es2-launch
description: Launch, stop, and screenshot Everspace 2 instances under Proton with the mod, and bring up a 2-instance co-op test. Use to start/restart the game for testing, run a host+client session, or capture what's on screen. Triggers: "launch the game", "start host and client", "coop session", "restart es2", "screenshot the game", "kill the game".
---

# Launch & test ES2 instances

The install path contains `™`, which breaks `proton run`; everything launches through the ASCII symlink `~/es2game` set up by `scripts/proton-env.sh`. The mod needs `WINEDLLOVERRIDES="dwmapi=n,b"`, which the launch scripts export.

## One instance
`scripts/launch.sh <name> [--port N] [--res WxH] [--nullrhi] [-- extra game args]`
- e.g. `scripts/launch.sh p1 --port 27100 --res 1280x720`
- `--nullrhi` = headless (cheap, for a host that doesn't need to render; adds `-nosound -unattended`).
- Games default to `-dx11` (DXVK is much lighter than DX12/vkd3d here).
- Each instance opens a TCP console on `127.0.0.1:<port>` (see `es2-console`). Port + pid are written to `run/<name>.port` / `run/<name>.pid`.
- Startup to the menu takes ~50–70 s; poll readiness with `python3 scripts/console.py <port> ping`.

## Full co-op session (host + client)
`scripts/coop-session.sh --kill`
- Kills stragglers, launches host (27100) + client (27101), loads the same save in each, host `listen 7777`, client `connect 127.0.0.1:7777`, prints client status + host net table.
- Flags: `--host-only`, `--no-connect`, `--nullrhi-host`, `--save <NAME>`.
- A save must exist. **Do not hardcode a save name**: ES2 rotates its autosaves, so an old name eventually
  disappears and the game asserts `Savegame load failed ... may be missing or corrupt` (it dies during load,
  before the console world check). The script defaults to the newest `ES2__AUTO_*` present and reads the
  location out of its `ES2__PREVIEW__*` sidecar (`CurrentLocation` / `NameProperty` / `<id>`), so it waits
  for the right world automatically.
- For the regression suite pin a save known to pass: `--save ES2__AUTO_2026.08.20-12.44.41` (S01ML01).
  A copy of it plus `MetaData.sav` is kept in `run/backup/SaveGames/` — restore from there if rotation eats it.

## Stop / observe
- Stop all game instances: `scripts/kill.sh` (matches the wine process, never this shell).
- Screenshot every ES2 window → `run/shots/`: `scripts/screenshot.sh <name>` (uses `magick import`; KDE/Wayland has XWayland at :0). Read the PNGs to see the game.
- Memory is tight (~15 GB box, ~3 GB/instance) — prefer 720p and a `--nullrhi-host` when running two.
