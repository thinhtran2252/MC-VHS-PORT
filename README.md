# MC VHS PORT

Native Android / Minecraft Bedrock port of the screen-space post-processing ideas in the Java shader repository [`thinhtran2252/mcvhs`](https://github.com/thinhtran2252/mcvhs). This is an independent LeviLauncher mod. It does not reuse LeviPostFX code.

## First port: single-pass screen effects

The current implementation hooks `eglSwapBuffers` through the public Preloader Android memory-hook API. On a current GLES context it copies the final default framebuffer to a texture, generates a mip chain, and draws a fullscreen GLES 3 pass back to the EGL surface.

Implemented first:

- Lens distortion and the Java horizontal aspect-ratio crop
- Time-based screen interlacing approximation
- Static chroma noise and white-noise bursts
- Auto exposure and manual exposure
- Tonemapping methods: none, tanh, exponential / Beer, Reinhard
- YUV, YIQ and YCbCr chroma compression controls
- Extra zoom, side jitter and mip-based sharpening
- Java-style `Default` and `DeepFried` profiles

## Config and Mod Menu

The native config is stored at `config/config.json`, with a generated `config.schema.json`. The config field IDs intentionally retain the Java shader option names where practical. The Levi Mod Menu exposes separate `Lens`, `Compression`, `Post Processing`, and `User Suggestions` modules, plus the master switch and profile selector.

The profile selector uses integer values because the public Levi Mod Menu config API exposes a numeric slider but does not expose a choice-list argument for its radio control in this SDK version:

- `0`: Default
- `1`: DeepFried

Changing the profile applies the corresponding values from `shaders.properties` for the effects currently ported. Other fields can then be edited individually and are saved to `config.json`.

## Important limitations in this first pass

- `eglSwapBuffers` sees the final composed screen. Effects therefore also affect HUD, menus, and other UI drawn into that framebuffer. This first implementation does not have a world-only render target.
- Java interlacing scales its offset by camera displacement. The final EGL hook does not expose camera movement, so this port uses a time-varying scanline offset as an approximation.
- Static noise is generated procedurally rather than sampling the Java pack's `4ChannelNoise.png` texture.
- This is a combined single-pass port. Bloom's horizontal/vertical blur needs intermediate render targets and is intentionally not enabled yet.
- SSR, world-space torch lighting, depth fog, water waves, and material passes need depth, normal, camera, or per-material data that this final-frame hook does not provide. They are not faked as if they were equivalent.
- Mipmaps are generated from the captured screen each frame to support the Java shader's LOD-based exposure, compression, and sharpening. Their performance cost needs to be measured on-device.
- The hook skips frames where a non-default framebuffer is still bound or the viewport dimensions are invalid. Device/game-specific framebuffer behavior still needs runtime testing.

## Build

Requirements: Android SDK, Android NDK 28.2.13676358 (or compatible), CMake 3.22+, Ninja, and PowerShell 7+.

```powershell
$env:ANDROID_HOME = "C:/Users/<you>/AppData/Local/Android/Sdk"
./scripts/package.ps1 -Abi arm64-v8a
```

For both supported ABIs:

```powershell
./scripts/package.ps1 -Abi all
```

The generated `.levipack` can then be imported into LeviLauncher. The package version and Minecraft version range remain configurable through CMake cache variables.
