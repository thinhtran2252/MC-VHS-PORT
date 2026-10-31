# Port map: Java shader to Bedrock native pass

| Java source | Effect | Port state | Notes |
|---|---|---|---|
| `composite2.fsh` | Aspect-ratio crop, lens distortion | Implemented | Same horizontal crop factor and radial warp formula. |
| `composite2.fsh` | Interlacing | Approximation | Uses a time-varying scanline offset because the final EGL hook does not receive camera displacement. |
| `composite2.fsh` | Static, exposure, manual exposure, contrast curve, white noise | Implemented / approximated | Procedural noise replaces the Java `4ChannelNoise.png` texture. The exposure samples the generated framebuffer mip chain. |
| `composite4.fsh` | Tonemapping | Implemented | Supports none, tanh-equivalent, Beer/exponential, and Reinhard methods. Bloom remains deferred. |
| `composite5.fsh` | YUV / YIQ / YCbCr compression | Implemented | Reads LODs from the captured screen mip chain and applies the preceding composite color operations to those samples. |
| `composite6.fsh` | Extra zoom, side jitter, sharpness | Implemented / approximated | Runs in the combined final pass; exact Java intermediate-buffer ordering is not available in this first pipeline. |
| `composite3.fsh`, `Bloom.glsl` | Horizontal / vertical bloom | Deferred | Requires dedicated intermediate targets and a separate blur pass. |
| `composite.fsh` | Torch, held-item light, distance fog | Deferred | Requires held-item/world position and other game-side data. |
| `composite1.fsh` | SSR, reflection, surface Fresnel | Deferred | Requires depth, normal, roughness, and camera matrices. |
| `gbuffers_water.fsh`, `Vertex_Lit.glsl`, `Fragment_Lit.glsl` | Material, water, lighting effects | Deferred | These are geometry/material stages, not final-screen-only effects. |

## Config parity

`PROFILE=0` applies the Java `Default` values for effects currently ported. `PROFILE=1` applies the Java `DeepFried` values. The Mod Menu page names mirror the Java `Lens`, `Compression`, `Post Processing`, and `User Suggestions` screens. Option IDs retain their Java names in `config.json` and in the Mod Menu config entries, including `USE_LENS_DISTORTION`, `LENS_DISTORTION_STRENGTH`, `ASPECT_RATIO`, `USE_INTERLACING`, `STATIC_STRENGTH`, `AUTO_EXPOSURE`, `MANUAL_EXPOSURE`, `TONEMAPPING_METHOD`, `COMPRESSION_METHOD`, `LUMA_COMPRESSION`, `CHROMA_COMPRESSION`, `CHROMA_BOOST`, `USE_SHARPNESS`, `SHARPNESS_STRENGTH`, `SHARPNESS_RADIUS`, `EXTRA_ZOOM`, and `SIDE_JITTER`.
