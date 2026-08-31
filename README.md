# librw — GameCube fork

A fork of [librw](https://github.com/aap/librw) by aap that adds a native
**Nintendo GameCube (GX) render backend**. It is the renderer of
[gamecube-reVC](https://github.com/origami-ltd/gamecube-reVC), the GameCube
port of Grand Theft Auto: Vice City, and is consumed there as a git
submodule.

## What this fork adds

New platform driver, `src/gx/`:

- `gx.cpp` — the GX device: im2D/im3D paths, the atomic render pipeline
  (CPU lighting with prelight plus timecycle ambient, TEV stage
  configuration with optional env-map, rim-light, gloss and lightmap
  stages), packed int16 vertex-stream drawing, GP display-list caching,
  EFB frame grabs for post-processing, and VI/XFB presentation with
  progressive-scan output.
- `gxraster.cpp` — native rasters: GameCube texture formats (CMPR and
  RGB5A3) with tiling and conversion, camera textures fed by EFB copies,
  geometry packing (`gxPackGeometry`) and display-list storage.
- `rwgx.h` — the backend's public interface.

Changes to librw core, made to host the new platform:

- Platform registration and device hooks (`src/engine.cpp`,
  `src/base.cpp`, `src/plg.cpp`, `src/rwengine.h`, `src/rwbase.h`).
- Native raster and texture plumbing for GX formats (`src/raster.cpp`,
  `src/texture.cpp`, `src/image.cpp`).
- Geometry, frame, camera, matfx and skin adjustments used by the GX
  pipeline (`src/geometry.cpp`, `src/frame.cpp`, `src/camera.cpp`,
  `src/matfx.cpp`, `src/skin.cpp`).
- Extended D3D8 TXD reading (`src/d3d/d3d8.cpp`) so the ahead-of-time
  GameCube texture converter can consume PC texture dictionaries.

## Building

This fork is normally built as part of gamecube-reVC (`python3 build.py`
there configures everything). Standalone, with
[devkitPro](https://devkitpro.org/wiki/Getting_Started) installed:

```bash
cmake -G Ninja -B build \
    -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/GameCube.cmake \
    -DLIBRW_PLATFORM=GAMECUBE
ninja -C build
```

The default branch of this repository is `gamecube`; `master` tracks
upstream librw.

## License

librw is by aap and keeps its original license (see `LICENSE`). This
fork's contributions — the GX backend and the related core changes listed
above — are additionally released under the
[MIT License with Proof-of-Usage Condition](LICENSE.md), with the usage
ledger in [PROOF_OF_USAGE.md](PROOF_OF_USAGE.md).
