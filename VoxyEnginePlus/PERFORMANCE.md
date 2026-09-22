# Performance work

Measured on an RTX 4070 Laptop GPU at 1920x1080, over a fixed 1500-frame scripted flythrough
(`VOXY_BENCH`, see below). Before/after runs were **interleaved** — this laptop's clocks drift
enough over a session that two runs taken minutes apart are not comparable.

| | before | after | |
|---|---|---|---|
| mean | 19.5 ms (51 fps) | 8.3 ms (120 fps) | **2.3x** |
| median | 20.2 ms (50 fps) | 7.7 ms (130 fps) | 2.6x |
| p95 | 28.5 ms (35 fps) | 14.5 ms (69 fps) | 2.0x |
| p99 | 32.4 ms (31 fps) | 16.5 ms (61 fps) | 2.0x |
| 1% low | 38.9 ms (26 fps) | 17.7 ms (56 fps) | 2.2x |
| worst frame | 94 ms | 22 ms | 4.3x |
| GPU time | 18.4 ms | 7.0 ms | 2.6x |

On a cool GPU the same run measures ~6.7 ms mean (150 fps) / 12.7 ms p99. The *ratio* is the stable
number; absolute values move with GPU temperature.

No rendering setting was lowered: same resolution, same GI sample counts, same bounce counts, same
5-pass denoiser, same full-res hard shadows.

## What was done

### Render graph

- **Indirect lighting moved to its own half-res pass** (`gi-trace.slang`). It used to be a
  `pixel % 2 == 0` branch inside the full-res gbuffer pass. That is expensive in a way the branch
  hides: the bounce code's register footprint applies to the whole shader, so the 3/4 of lanes that
  skipped the work still paid for it in lost occupancy. Deleting the block entirely took the gbuffer
  pass from 4.6 ms to 1.2 ms, while deleting any individual *part* of it changed almost nothing —
  the signature of an occupancy wall rather than an instruction-count problem. Splitting it cost one
  extra primary trace per GI pixel and was still a large net win.
  It is scheduled **before** the gbuffer pass so its history lookup still reads last frame's
  position buffer.
- `[numthreads(32,32,1)]` → `(8,8,1)` on the gbuffer pass. 1024-thread groups badly limit occupancy
  for a divergent raymarcher.
- Compute group sizes now live in `const.inl` and are used by both `[numthreads]` and `dispatch()`,
  so the two cannot drift apart. (They had to be edited in two places before; getting that wrong
  silently shades the wrong number of pixels instead of failing.)
- Split barriers enabled, task-graph debug recording disabled.

### Memory access

- **Half-res mirrors of the data the GI passes sample repeatedly.** `giGeometry` packs
  (position.xyz, normal) and `giIDs` packs (voxelID, faceID). The denoiser reads geometry 25x per
  pass across 5 passes, and `combine_gi` walks runs of pixels re-reading ids at every step — both
  were striding through full-res targets at `halfResPixel * 2`. Denoise: 3.0 ms → 0.9 ms.
- **Dead work removed.** `composite` was running a full voxel-hashmap probe per pixel whose result
  only fed a commented-out debug line, plus ~70 bytes/pixel of texture reads nothing used. The
  denoiser computed colour edge-stopping weights — 125 `exp()` per pixel per frame — and then did
  not apply them. The GI bounce fetched a material through the ~1.8 GiB voxel material buffer even
  when the ray missed.
- **Visibility-only ray variant** (`traceVoxelRayAnyHit`). Shadow and sun-visibility rays only read
  `.hit`; the full version's hit-point bookkeeping was held in registers across the whole loop.

### Frame pacing

- Shader hot-reload was calling `reload_all()` **every frame**, stat-ing every shader file including
  all of lygia. Now polled twice a second.
- Present mode is MAILBOX where supported instead of FIFO. FIFO blocks on vblank, so a frame that
  misses the interval costs a whole extra one — that is what read as stutter.
- `glfwSwapBuffers` was being called on a `GLFW_NO_API` window, raising `GLFW_NO_WINDOW_CONTEXT`
  every frame.

### Other

- Device selection now prefers a discrete GPU. daxa's `choose_device` takes the *first* device that
  satisfies the request, which on a laptop can be the integrated one.
- Blue-noise textures upload in one batch rather than 64 separate submit + `wait_idle` round trips.
- Slang's plugin DLLs (`slang-glslang` etc.) are copied next to the executable; without them shader
  compilation fails at startup.
- `main()` reports shader compilation errors instead of dying with a bare `bad optional access`.

## Things that were tried and rejected

Kept here so they are not re-tried:

- **5x5x5 chunk tiling** to improve locality in the brick occupancy array (65 = 13*5, so it tiles
  exactly with no memory overhead). ~20% **slower**: `chunkIndex()` sits in the innermost DDA loop
  and the integer div/mod by 5 costs more than the locality buys.
- **`[noinline]` on the traversal.** Measured as a wash over 4 interleaved repeats.
- **Packing the GI lanes by pixel parity** into the first waves of a group. Slower — it costs more
  in primary-ray coherence than it saves in divergence.
- **16x16 groups for the GI pass.** No better than 8x8 once the dispatch size was corrected.

## Known remaining bottleneck

The traversal is bound on scattered misses into the 562 MB brick-occupancy array. Folding that array
into a 2 MB window (a correctness-breaking probe, purely to measure) cuts GPU time by ~22%.
The real fix is to store brick bitmasks compactly — indexed by a per-chunk prefix sum over the chunk
occupancy mask, so only occupied bricks take space — which would shrink the hot set enough to sit in
L2. That needs a prefix-sum pass and a compaction pass after terrain generation.

## Measuring

```bash
# scripted flythrough, then a frame-time and per-pass summary
VOXY_BENCH=1500 ./VoxyEngine.exe
```

The camera path is driven by **frame index**, not wall time, so every build renders exactly the same
frames and the only thing that differs is how long they took.

```bash
# dump the rendered image at every 50th frame, as raw RGBA, to compare two builds
VOXY_BENCH=1500 VOXY_CAPTURE=/some/dir ./VoxyEngine.exe

# per-second console breakdown during normal play
VOXY_PROFILE_LOG=1 ./VoxyEngine.exe
```

**F3** toggles an in-game overlay with fps, 1% low, and per-pass GPU times.

When comparing two builds, alternate them (A,B,A,B,…) and take medians. And when checking that a
change did not alter the image, always capture a same-build control pair first: the GI accumulates
temporally through a data race, so even two runs of an unmodified build differ by a few hundredths
of a mean absolute level, with occasional single-pixel differences of 200+.
