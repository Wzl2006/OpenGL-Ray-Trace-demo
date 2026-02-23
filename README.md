# OpenGL Compute Ray Tracing Demo

Realtime Cornell Box path tracing demo built with OpenGL 4.3 Compute Shader, Qt6, C++20, and CMake.

## Features

- Cornell Box (`3x3x3`) with front opening and embedded `1x1` top area light.
- Triangle BVH on CPU, BVH traversal + triangle test on GPU (SSBO).
- Analytic sphere intersections for three glass-like spheres.
- Reflection, refraction, total internal reflection, rough GGX response.
- Schlick Fresnel, Cauchy dispersion (`A=1.5`, `B=0.004`, 6 bands).
- NEE area-light sampling + BRDF importance sampling + MIS + Russian roulette.
- Accumulation buffer (`GL_RGBA32F`) with dynamic reset and static alpha blending.
- OIDN (OpenImageDenoise) CPU denoiser backend with color/normal/albedo guides.
- Async denoise scheduling: denoise jobs are launched only after scene settles to avoid UI stalls.
- Delayed high-quality rendering to avoid lockups at high SPP:
  - immediate low-SPP preview after edits,
  - then automatic recovery to higher quality.
- Debug monochromatic mode for chroma-noise analysis:
  - fixed wavelength (default 550nm),
  - dispersion and absorption disabled.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DRT_QT6_ROOT="D:/Qt/6.9.3/msvc2022_64" `
  -DRT_GLM_ROOT="D:/glm" `
  -DRT_OIDN_ROOT="D:/oidn-2.4.1.x64.windows"

cmake --build build --config Debug --target trace
```

Output:

- `build/Debug/trace.exe`
- shaders copied to `build/Debug/glsl/`

## Runtime Controls

### Camera

- Left mouse drag: orbit (`yaw ±60`, `pitch ±20`)
- Mouse wheel: zoom (`distance 2..10`)

### Render Panel

- `Max Bounces`
- `Static Alpha`
- `Area Light Samples`
- `SPP / Frame` (`1..2048`, supports `>1000`)
- `SPP Budget` (`1..256`, hard cap for per-frame work)
- `Settle Delay` (`0..2000 ms`)
- `Enable Denoise`
- `Denoise Sigma`
- `Debug Monochromatic (550nm)`

### Stats Panel (250ms update)

- GPU render time (Timer Query)
- FPS
- Accumulated frame count
- Effective SPP (actual per-frame samples)
- Dynamic/Static state
- BVH state
- Denoise state

## Denoise Toggle Behavior

The UI denoise toggle is wired to rendering logic:

- If **off**:
  - denoise job scheduling is skipped,
  - display uses raw `outputTexture`.
- If **on**:
  - OIDN denoise jobs run on CPU worker thread after scene settles,
  - `beauty/output + normal + albedo` are captured from the same frame snapshot,
  - display uses `mix(raw, denoised, denoiseBlend)` (default `0.85`).

Current default OIDN scheduling parameters:

- `minAccumFrames = 16`
- `denoiseIntervalMs = 200`
- `denoiseBlend = 0.85`

## Delayed Rendering Behavior

- Target quality is `SPP / Frame`.
- Actual work per frame is:

```text
EffectiveSPP = min(SPP / Frame, SPP Budget)
```

- During the settle window after any scene/parameter change:

```text
EffectiveSPP = 1
```

This prevents machine lockups when target SPP is high.

## Project Layout

```text
src/
  main.cpp
  app/
    MainWindow.h/.cpp
  render/
    RenderWidget.h/.cpp
    Renderer.h/.cpp
    glsl/
      raytrace.comp.glsl
      denoise.comp.glsl
      fullscreen.vert.glsl
      fullscreen.frag.glsl
  scene/
    SceneData.h/.cpp
    BvhBuilder.h/.cpp
```

## Known Limits

- UI and rendering run on the same thread.
- OIDN input still requires texture readback from OpenGL, so denoise cadence is throttled.
- If OIDN is not found at configure time, renderer falls back to built-in compute denoiser.
- No HDR output pipeline.
