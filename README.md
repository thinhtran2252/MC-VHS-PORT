# MC VHS PORT

A native Android / Minecraft Bedrock port of the **screen-space post-processing stages** in [`thinhtran2252/mcvhs`](https://github.com/thinhtran2252/mcvhs). LeviPostFX is used only as a reference for the compatible Preloader SDK revision and the NativeMod / Mod Menu lifecycle. Its shader effects and render implementation are not copied into this project.

## Rebuild 0.2.0: first faithful multi-pass pass

The Java source is organized as several composite passes. This port now keeps the same relevant ordering instead of collapsing all math into one fragment shader:

1. Capture the final EGL surface into a mipmapped texture.
2. `composite2`: 4:3 crop, lens distortion, static noise, the original contrast curve, auto exposure and manual exposure.
3. `composite4`: tonemapping.
4. `composite5`: YUV / YIQ / YCbCr compression using mip levels from the tonemapped intermediate.
5. `composite6`: extra zoom, optional side jitter and mip-based sharpness.

Intermediate targets prefer RGBA16F so values above 1.0 can survive until tonemapping. RGBA8 is used only when RGBA16F render targets are unavailable. Each completed intermediate gets a mip chain before the next stage reads it.

The fullscreen triangle uses GLES 3.0 / GLSL ES 3.00 and is independent of Java/OptiFine runtime code. The native lifecycle and Mod Menu registration use the same pinned Preloader Android SDK revision used by the reference LeviPostFX project, but all MC VHS effect math is translated from the original `mcvhs` shader sources.

## Included effects

| Java source | Effect | Status in 0.2.0 |
|---|---|---|
| `composite2.fsh` | 4:3 horizontal crop | Implemented |
| `composite2.fsh` | Lens distortion | Implemented |
| `composite2.fsh` | Static noise in YUV channels | Implemented with the source PRNG |
| `composite2.fsh` | Contrast curve | Implemented |
| `composite2.fsh` | Auto / manual exposure | Implemented; capture mip chain used for auto exposure |
| `composite4.fsh` | Tanh / Beer / Reinhard tonemapping | Implemented |
| `composite5.fsh` | YUV / YIQ / YCbCr compression | Implemented from the source matrices and mip sampling |
| `composite6.fsh` | Extra zoom | Implemented |
| `composite6.fsh` | Side jitter | Implemented |
| `composite6.fsh` | Mip-based sharpness | Implemented |

## Deliberately deferred

These are not silently replaced with unrelated screen filters:

- **Interlacing (`composite2.fsh`)**: the Java offset scales with camera displacement (`cameraPosition - previousCameraPosition`). The final `eglSwapBuffers` callback does not expose those camera values. A time-only substitute would not be equivalent, so the control is not exposed yet.
- **White-noise bursts (`USE_MORE_STATIC`)**: Java samples the `Spotlight.png` texture bound to `colortex6`. The current stage does not load that shader texture asset, so this option is withheld until the asset path is implemented.
- **Bloom (`composite3.fsh`, `composite4.fsh`, `Bloom.glsl`)**: requires the separate horizontal and vertical blur stages plus an emission target. Planned for a later pass.
- **Torch and depth fog (`composite.fsh`)**: require depth, linearized world position, fog uniforms, held-item ID/light level and the spotlight texture.
- **SSR / sky reflection (`composite1.fsh`)**: requires depth, normals, roughness, projection matrices and camera position.
- **Water waves and material / lighting programs**: geometry and per-material stages are outside a final-frame post-process hook.

The EGL hook processes the final composed frame, so HUD and menus may receive the same image effects. World-only UI exclusion is not available from this hook.

## Levi integration

- SDK dependency pinned to Preloader Android commit `5d108da350ce3e7f13f51415f81e65344ecd5d14`, matching the reference LeviPostFX SDK revision.
- `load()` only loads and normalizes config.
- `enable()` registers the single Mod Menu module and installs the EGL hook if the master switch is enabled.
- `disable()` removes the hook and unregisters the module.
- `unload()` performs cleanup and releases config state.
- No `ModuleInfo` is manually constructed. Registration uses the SDK `ModuleBuilder` API.

This is an API/lifecycle alignment, not a copy of LeviPostFX's post-processing shader or effect implementation.

## Build

Requirements: Android SDK, Android NDK 28.2.13676358 (or compatible), CMake 3.22+, Ninja and PowerShell 7+.

```powershell
$env:ANDROID_HOME = "C:/Users/<you>/AppData/Local/Android/Sdk"
./scripts/package.ps1 -Abi arm64-v8a
```

Build both supported ABIs:

```powershell
./scripts/package.ps1 -Abi all
```

The generated `.levipack` is placed in the ABI-specific build directory. Import it into LeviLauncher.

## Validation status

Source-level port and project structure are prepared. This environment has not run an Android NDK build or an on-device Minecraft test, so this is **not yet a verified installable release**. The first device test should confirm: mod loads without a native crash, the Mod Menu page opens, the captured surface is valid, and each pass renders without GL errors. RGBA8 fallback and game/UI framebuffer behavior also need device validation.
