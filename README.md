# Voxel

A performance-optimized build of **[VoxyEnginePlus](https://github.com/KosmosisDire/VoxyEnginePlus)**
by [KosmosisDire](https://github.com/KosmosisDire) — a C++/Vulkan voxel renderer with path-traced
global illumination, built on the [Daxa](https://github.com/Ipotrick/Daxa) abstraction layer.

The engine source in [`VoxyEnginePlus/`](VoxyEnginePlus/) is upstream's, taken from commit
[`5c2d8c9`](https://github.com/KosmosisDire/VoxyEnginePlus/commit/5c2d8c97bc1e8a428f7d4d954152e83dbdebdde9)
("Rollback to state shown in pictures in readme", 2026-06-14), with rendering optimizations applied
on top. All credit for the engine itself goes to the original author. Upstream does not publish a
licence, so treat the engine code as the author's.

## What changed

**2.3x faster at identical visual quality** — no resolution, sample count, bounce count or denoiser
pass was reduced. On an RTX 4070 Laptop GPU at 1920x1080, over a fixed 1500-frame scripted
flythrough:

| | before | after |
|---|---|---|
| mean | 19.5 ms (51 fps) | **8.3 ms (120 fps)** |
| median | 20.2 ms (50 fps) | 7.7 ms (130 fps) |
| p99 | 32.4 ms (31 fps) | 16.5 ms (61 fps) |
| 1% low | 38.9 ms (26 fps) | 17.7 ms (56 fps) |
| worst frame | 94 ms | 22 ms |

The headline changes: indirect lighting moved out of the full-res gbuffer pass into its own half-res
dispatch (it was an occupancy wall, not a cost), 1024-thread workgroups cut to 64, half-res packed
mirrors of the data the GI passes re-read, and a pile of work whose results nothing consumed.

Full write-up, including the experiments that were tried and rejected and the remaining bottleneck:
**[VoxyEnginePlus/PERFORMANCE.md](VoxyEnginePlus/PERFORMANCE.md)**.

## Running a release build

Download the zip from [Releases](https://github.com/tonytranrp/Voxel/releases), extract it, and run
`VoxyEngine.exe` from the extracted folder (it loads `resources/` relative to itself). You need a
Vulkan 1.3 capable GPU and current drivers — nothing else to install.

Controls: click to capture the mouse, `WASD` + `Space`/`Shift` to fly, `Esc` to release the mouse,
`F3` for the GPU profiler overlay, `F4` for the UI debugger, `1` to regenerate terrain, `2` to reset
lighting.

## Building from source

Needs the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home), CMake 3.19+, and MSVC with the C++
workload. vcpkg and Daxa are fetched automatically into `VoxyEnginePlus/lib/` on first configure.

```bash
git clone --recurse-submodules https://github.com/tonytranrp/Voxel.git
cd Voxel/VoxyEnginePlus
cmake -S . -B build/Release -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/Release --config RelWithDebInfo --parallel
```

`--recurse-submodules` matters: the shaders include [lygia](https://github.com/patriciogonzalezvivo/lygia),
which is a submodule. Use `-G "Visual Studio 18 2026"` if that is the Visual Studio you have.

## Benchmarking

```bash
# scripted flythrough, then frame-time percentiles and per-pass GPU timings
VOXY_BENCH=1500 ./VoxyEngine.exe
```

The camera path advances by frame index rather than wall time, so two builds render exactly the same
frames and only the timings differ. `VOXY_CAPTURE=<dir>` additionally dumps raw frames for comparing
two builds pixel by pixel. See PERFORMANCE.md for how to read the results.
