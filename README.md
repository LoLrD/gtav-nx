# gtav-nx: GTA V on the Nintendo Switch (homebrew)

Grand Theft Auto V (Xbox 360, Title Update 27) statically recompiled to ARM64 with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and drawn with a Vulkan renderer on NVK, for the
original Nintendo Switch. No emulator. A Switch branch of
[gtav-recomp](https://github.com/andrewnakas/gtav-recomp).

> [!WARNING]
> **The port is not finished.** It boots and plays through the prologue and the first mission at
> 17-25 fps on stock clocks, with rendering problems. Performance is being worked on.

Nothing derived from the game is in this repository: the code, the shaders and the NRO are made on
your computer from your own copy.

## Status

| Area | State |
| --- | --- |
| Boot, menus | Work |
| Gameplay | Prologue and the first mission play through |
| Rendering | Vulkan renderer from the game's translated shaders, scene at 640x360 stretched to 720p. A flat strip of road sometimes covers the ground outdoors |
| Audio | Works, with quiet sources cut |
| Saving | Fixed in code, not confirmed on the console |
| Memory | Stable around 1.9 GB of the 3.2 GB a homebrew gets |

## Performance

Handheld, stock clocks (CPU 1020 MHz, GPU 460.8 MHz), three cores. The goal is a stable 30 fps
without overclocking.

| Place | fps | GPU per frame |
| --- | --- | --- |
| Prologue, bank | 22-25 | 39-44 ms |
| Prologue, street | 17-23 | 42-57 ms |
| Los Santos, downtown | 19-22 | 45-51 ms |
| Los Santos, road | 17 | 50 ms |

The GPU is the limit. In the bank the G-buffer takes 12.5 ms (8.8 of it vertex work, 595k indices),
deferred lighting 10.4 ms (7.8 of it pixel shaders), forward and post effects 6.9 ms.

## Building

On macOS or Linux with CMake, Ninja, clang, Python 3 and Docker:

1. Fetch the SDK's third-party sources: `python3 tools/fetch_thirdparty.py`.
2. Put your own `default.xex` and `default.xexp` (Title Update 27) in `gtav/assets/`.
3. Build the code generator from `sdk/` and run it in `gtav/`:
   `../sdk/out/mac-arm64/rexglue --force codegen gtav_manifest.toml`.
4. Build the desktop version, which also builds the renderer's own shaders:
   `cmake -S gtav -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR=$PWD/sdk`, then
   `cmake --build build/mac`.
5. Build the Switch Vulkan driver once: `mesa/build_mesa_docker.sh` (about an hour, 15 GB; see
   [mesa/README.md](mesa/README.md)).
6. Build the NRO: `LTO=ON tools/build_nro.sh`. Without `LTO` a renderer change relinks in under a
   minute.
7. Copy `tools/console-test/credentials.env.example` to `credentials.env`, set the console's
   address, then run `tools/console-test/upload_data.sh` (your game files) and
   `tools/console-test/deploy.sh` (the NRO and `gtav/gtav_switch.toml`).

`tools/console-test/bench.txt` is a script the game runs by itself (`gtav_bench_script`): it goes
to fixed spots, compares settings in an A/B sweep and quits. `tools/console-test/collect.sh NAME`
fetches the logs and prints the sweep as a table.

## Credits

- [gtav-recomp](https://github.com/andrewnakas/gtav-recomp) by Andrew Nakas: the recompilation and
  the renderer this port is built on.
- [ReXGlue](https://github.com/rexglue/rexglue-sdk) for the recompiler and SDK;
  [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) for the Switch fork of the SDK,
  with the Horizon layer by StevensND (`sdk/UPSTREAM.md`).
- Mesa NVK for Horizon: [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico,
  NaGaa95 and StevensND.
- [plume](https://github.com/renderbag/plume) and
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp).

## Legal

Unofficial fan port with no affiliation to Rockstar Games, Take-Two Interactive, Microsoft or
Nintendo. "Grand Theft Auto" is a trademark of Take-Two Interactive Software. You provide your own
legally obtained copy of the game. No prebuilt files are distributed: the NRO contains code
recompiled from the game, so it is made on your own computer from your copy. The leaked Rockstar
source code was never used. Running homebrew requires custom firmware.

Source code is under the GPL-3.0 license ([LICENSE](LICENSE)). Credits and the licenses of bundled
components are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
