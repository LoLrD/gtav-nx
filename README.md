# gtav-nx: GTA V on the Nintendo Switch (homebrew)

Grand Theft Auto V (Xbox 360, Title Update 27) statically recompiled to ARM64 with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and drawn with a Vulkan renderer on NVK, for the
original Nintendo Switch. No emulator. A Switch branch of
[gtav-recomp](https://github.com/andrewnakas/gtav-recomp).

> [!WARNING]
> **The port is not finished.** It boots and plays through the prologue and the first mission at
> 17-25 fps on stock clocks, with rendering problems. Performance is being worked on.

**No game is included, ever.** Not in this repository, not in any release. You bring your own
Xbox 360 discs and title update; everything made from them (the recompiled code, the game files,
the translated shaders, the NRO) is made on your own computer and stays there.

> [!WARNING]
> **Forking, mirroring or publishing your own builds?** Read
> [Keeping it clean](#keeping-it-clean-rules-for-forks-and-downstream-releases) first. The
> recompiled game code, the game's shaders and anything else made from the discs are **Rockstar
> Games' copyrighted work**. They must never be in a repository, release, NRO, NSP, mirror or
> download link. This project's GPL license covers only our own code and cannot give anyone the
> right to share Rockstar's.

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

## Keeping it clean: rules for forks and downstream releases

This project only works because it **never distributes Rockstar Games' or Microsoft's work**, the
same way [gtav-recomp](https://github.com/andrewnakas/gtav-recomp) does. Every player recompiles
their own copy from their own discs. If you fork this repository, mirror it or share builds with
friends, the same rules apply to you.

### Never distribute any of these

| What | Why it is Rockstar's / Microsoft's |
|---|---|
| Disc images (ISO), extracted disc folders, `default.xex` / `default.xexp`, `.rpf` archives, install packages | the game itself |
| Title updates (`tu00000009_00000000`, `update.rpf`), or links to sites that host them | the game's executable and data |
| Extracted assets: models, textures, audio, maps, scripts, text | the game's data |
| **The generated C++** (`gtav/generated/`, the files the codegen writes) | a mechanical translation of the game's code is still the game's code |
| **The NRO or an NSP**, or any other binary with the game linked in | compiled game code |
| **Game shaders in any form**: the originals, HLSL, SPIR-V (`spirv/`), pipeline caches, `shader_cache` | translated game code |
| Your game folder on the SD card, saves, memory dumps, GPU captures, traces, and logs that contain game memory | game data |

### Rules for working on the code

- **Never use the leaked Rockstar source code**, not even for names, comments or structure. Work only
  from your own analysis of your own copy of the game.
- Don't commit anything game-derived, even temporarily. Git history is permanent, and a deleted
  file stays downloadable from old commits.
- Function addresses, sizes and hook names (the manifest, the codegen partition, the hook lists)
  are our own analysis and are fine. Code copied from the game is not, including code pasted from
  a disassembly or from the generated C++.

### Why this matters

- **The GPL can't help you here.** The GNU GPL covers *our* code, and you may share and modify that
  freely under its terms. It gives no rights to Rockstar's or Microsoft's work, because nobody here
  owns them. Sharing a build with the game inside is copyright infringement by whoever shares it,
  whatever license file sits next to it.
- **Recompiling isn't a clean room.** The output of the recompiler is the game's code in another
  form. It is legal for you to make it from a copy you own. It isn't legal to hand it to someone else.
- **One bad mirror can end the project for everyone.** A takedown aimed at a build with game content
  can take this repository with it.

If you find game content in this repository or in a fork that links here, please open an issue
(without attaching the content) so it can be removed.

## License and legal

This project is free software under the **GNU GPL, version 3 or later** (`LICENSE`). Bundled
third-party components keep their own licenses; they are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

No Rockstar Games or Microsoft code, data, shaders or title-update files are in this repository,
and the project never used leaked source code. What you make from your own discs is for you alone:
don't share your game folder or your NRO. The GPL applies to this project's own code only; it
grants no rights to Rockstar Games' or Microsoft's work.

Grand Theft Auto V is a trademark of Take-Two Interactive Software. This project is not affiliated
with or endorsed by Rockstar Games, Take-Two Interactive, Microsoft or Nintendo.
