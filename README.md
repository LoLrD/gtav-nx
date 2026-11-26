# gtav-nx: GTA V (Xbox 360) recompiled for the Nintendo Switch

A port of the Xbox 360 version of Grand Theft Auto V (Title Update 27) to the original Nintendo
Switch, made by **static recompilation**: the console's PowerPC executable is translated to native
ARM64 code and runs on an Xbox 360 runtime with a new Vulkan renderer.

It is a Switch branch of [andrewnakas/gtav-recomp](https://github.com/andrewnakas/gtav-recomp)
(the same recompilation for Windows, Linux, macOS, iPhone and Android), built with
[ReXGlue](https://github.com/rexglue/rexglue-sdk).

**No game is included, ever.** Not in this repository and not in any download. You bring your own
Xbox 360 discs and title update; everything made from them is made on your own computer and stays
there. There are no prebuilt NRO files and there will not be any: an NRO contains the recompiled
game code, which is Rockstar Games' work. See [Keeping it clean](#keeping-it-clean).

## Status: work in progress

This is under active development and not ready to play. If you are thinking about starting the same
port, please open an issue first so the work is not done twice.

The goal is a stable 30 fps on **stock clocks** (CPU 1020 MHz, GPU 460.8 MHz, three cores): no
overclocking and no fourth core.

| | Today |
|---|---|
| Boots, prologue and the first mission | yes |
| Frame rate indoors (prologue bank) | 22-25 fps |
| Frame rate outdoors and in Los Santos | 17-22 fps |
| Scene resolution | 640x360, upscaled to 720p |
| Memory | stable around 1.9 GB of the 3.2 GB available |
| Saving | fixed in code, not confirmed on the console |

Where the time goes, measured on the console in the prologue bank (44 ms of GPU per frame):

| Pass | Total | Pixel shaders | Vertices |
|---|---|---|---|
| G-buffer | 12.5 ms | 3.5 | 8.8 (595k indices, about 11 ns each) |
| Deferred lighting | 10.4 ms | 7.8 | 2.5 |
| Forward and post effects | 6.9 ms | nearly all | |
| Resolve copies and the rest | about 13 ms | | |

Known problems:

- The GPU is the limit everywhere in the prologue; in Los Santos the CPU is close behind.
- A flat strip of road sometimes covers the ground outdoors, near the boundary of the two G-buffer
  tiles.
- The automated benchmark run ends with an error screen on exit.

Help is welcome with the vertex cost on NVK and Maxwell, cheaper translated shaders, and resolve
copies.

## How it differs from gtav-recomp

- `sdk/`: the Switch fork of the ReXGlue SDK from
  [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) (Horizon layer by StevensND), with
  local changes listed in `sdk/UPSTREAM.md`.
- `mesa/`: the patch and build scripts for the Vulkan driver the NRO carries (Mesa NVK for Horizon,
  from [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch)).
- `gtav/gtav_switch.toml`: the console settings (render scale, level of detail, audio cuts, frame
  pacing).
- Renderer and runtime work for the console: guest memory budget, store cards instead of hashing,
  address waits, audio cuts, frame pacing without the audio frame wait, GPU pass timing and A/B
  sweeps.
- `tools/console-test/`: deploy over FTP, log collection, and an unattended benchmark script.

## Building

The build is not packaged yet; expect to read the scripts. On macOS or Linux you need CMake, Ninja,
clang, Python 3 and Docker.

1. Put your own `default.xex` and `default.xexp` (Title Update 27) in `gtav/assets/`.
2. Build the code generator from `sdk/` and run it in `gtav/`:
   `../sdk/out/mac-arm64/rexglue --force codegen gtav_manifest.toml`.
3. Build the desktop version (it also builds the renderer's own shaders, which the Switch build
   reuses): `cmake -S gtav -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR=$PWD/sdk`
   and `cmake --build build/mac`.
4. Build the Switch Vulkan driver once: `mesa/build_mesa_docker.sh` (about an hour and 15 GB; the
   result is `out/mesa-sdk`).
5. Build the NRO in Docker: `LTO=ON tools/build_nro.sh`. Leave `LTO` off for a fast development
   build: after the first build a renderer change relinks in under a minute.
6. Copy `tools/console-test/credentials.env.example` to `credentials.env`, set the console's
   address, then `tools/console-test/upload_data.sh` (your game files) and
   `tools/console-test/deploy.sh` (the NRO and settings).

Measuring on the console:

- `tools/console-test/bench.txt` is a script the game runs by itself: it goes to fixed spots,
  toggles settings in an A/B sweep and quits. Set `gtav_bench_script` in the settings file to use it.
- `tools/console-test/collect.sh NAME` fetches the logs; `sweep_table.py` prints the sweep as a
  table.
- GPU times in the log are real milliseconds (NVK's timestamp tick on the Switch is 625/384 ns).

## Keeping it clean

This project works only because it never distributes Rockstar Games' or Microsoft's work. The rules
of [gtav-recomp](https://github.com/andrewnakas/gtav-recomp#keeping-it-clean-rules-for-forks-and-downstream-releases)
apply here in full. In short, never commit or share:

- disc images, `default.xex`, `default.xexp`, `.rpf` archives, title updates, or links to them;
- the generated C++ (`gtav/generated/`) or anything compiled from it, **including the NRO**;
- game shaders in any form (originals, HLSL, SPIR-V, pipeline caches);
- game folders, saves, memory dumps, GPU captures and logs that contain game memory.

Never use the leaked Rockstar source code, not even for names or structure. Function addresses,
sizes and hook names are our own analysis and are fine; code copied from the game is not.

## Credits

- [andrewnakas/gtav-recomp](https://github.com/andrewnakas/gtav-recomp): the recompilation project
  this is based on, with its renderer.
- [ReXGlue](https://github.com/rexglue/rexglue-sdk), and the Switch fork of its SDK in
  [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) with the Horizon layer by
  StevensND.
- Mesa NVK for Horizon: [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch),
  NaGaa95 and StevensND.
- [plume](https://github.com/renderbag/plume) and
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp).

## License and legal

This project's own code is free software under the **GNU GPL, version 3 or later** (`LICENSE`).
Bundled third-party components keep their own licenses.

No Rockstar Games or Microsoft code, data, shaders or title-update files are in this repository.
The GPL applies to this project's own code only; it grants no rights to Rockstar Games' or
Microsoft's work.

Grand Theft Auto V is a trademark of Take-Two Interactive Software. This project is not affiliated
with or endorsed by Rockstar Games, Take-Two Interactive, Microsoft or Nintendo.
