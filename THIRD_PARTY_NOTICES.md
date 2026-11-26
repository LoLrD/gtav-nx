# Third-party notices

| Project | Used for | License |
|---|---|---|
| [gtav-recomp](https://github.com/andrewnakas/gtav-recomp) by Andrew Nakas | `gtav/` (game hooks, manifest, the native renderer in `gtav/src/native/`), `tools/release/`, `tools/game_dll/`, `tools/scripttools/` | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 by Tom Clay, with portions from [Xenia](https://xenia.jp) | `sdk/` | BSD-3-Clause (`sdk/LICENSE`) |
| [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by NebadasSwifty | the Switch fork of the SDK in `sdk/` (see `sdk/UPSTREAM.md`), `tools/console-test/` | GPL-3.0; SDK changes under the SDK's BSD-3-Clause |
| Horizon (Switch) layer of the SDK by [StevensND](https://github.com/StevensND) | `sdk/src/**/*_switch.*`, `sdk/src/ui/switch_*.cpp`, `sdk/cmake/rexglue_switch.cmake` | BSD-3-Clause |
| Scripts by [StevensND](https://github.com/StevensND) | `tools/fetch_thirdparty.py` | GPL-3.0 |
| [mesa-switch](https://github.com/danfromtico/mesa-switch) (Mesa NVK and NAK for Horizon) by danfromtico, NaGaa95 and StevensND, with the masseffect-nx patch | `mesa/`; the driver is built from it and linked into the NRO | MIT |
| [plume](https://github.com/renderbag/plume) | `thirdparty/plume/` | MIT |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev | `thirdparty/XenosRecomp/` | MIT |
| [UnleashedRecomp-NX](https://github.com/ChanseyIsTheBest/UnleashedRecomp-NX) | shader translator optimizations in `thirdparty/XenosRecomp/` (constants from uniform buffers, plain moves, texture sizes from constants, colour channel masks, blend skip) | GPL-3.0 |

Everything outside `sdk/` and `thirdparty/` is GPL-3.0 (`LICENSE`).
