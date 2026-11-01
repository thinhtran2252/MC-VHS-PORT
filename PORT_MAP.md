# MC VHS Java -> Levi native port map (0.2.0)

Reference source: https://github.com/thinhtran2252/mcvhs
Levi API/lifecycle reference: https://github.com/thinhtran2252/LeviPostFX

| Java source | Effect | 0.2.0 status | Why / implementation |
|---|---|---|---|
| `composite2.fsh` | Aspect ratio | Implemented | Same horizontal UV scale `1.2` and crop boundary. |
| `composite2.fsh` | Lens distortion | Implemented | Same radial UV warp equation and strength control. |
| `composite2.fsh` | Static noise | Implemented | Ports the `RandomValue` / normal-distribution / `RandomDirection` PRNG and YUV channel offset. |
| `composite2.fsh` | Contrast curve | Implemented | Same `(x + x) / (2 - x)` curve. |
| `composite2.fsh` | Auto exposure | Implemented | Samples LOD 8 from the captured scene mip chain and applies `averageLuma + 0.5`. |
| `composite2.fsh` | Manual exposure | Implemented | Same multiply after optional auto exposure. |
| `composite2.fsh` | Interlacing | Deferred | Requires previous/current camera position delta, not available at final EGL swap. |
| `composite2.fsh` | White-noise bursts | Deferred | Requires the Java `Spotlight.png` / `colortex6` sample. |
| `composite3.fsh` + `Bloom.glsl` | Horizontal bloom | Deferred | Needs a dedicated bloom target and blur pass. |
| `composite4.fsh` | Vertical bloom | Deferred | Needs bloom target from the preceding pass. |
| `composite4.fsh` | Tonemapping | Implemented | Tanh-equivalent, Beer/exponential and Reinhard methods. |
| `composite5.fsh` | YUV / YIQ / YCbCr compression | Implemented | Reads LODs from the tonemapped intermediate mip chain and uses the source transforms. |
| `composite6.fsh` | Extra zoom | Implemented | Same centred coordinate scaling. |
| `composite6.fsh` | Side jitter | Implemented | Same seeded random distribution and amount. |
| `composite6.fsh` | Sharpness | Implemented | Same negative-strength mix with the requested mip LOD. |
| `composite.fsh` | Torch | Deferred | Needs world depth, held light/item data and spotlight texture. |
| `composite.fsh` | Fog | Deferred | Needs depth reconstruction, near/far, fog color and underwater state. |
| `composite1.fsh` | SSR / sky reflection | Deferred | Needs depth, normal/specular targets and camera matrices. |
| `gbuffers_water.fsh`, `Vertex_Lit.glsl`, `Fragment_Lit.glsl` | Water / material lighting | Deferred | Geometry/material passes cannot be faithfully produced from the final framebuffer alone. |

## Render order implemented

`capture -> composite2 -> mipmap -> composite4 tonemap -> mipmap -> composite5 compression -> mipmap -> composite6 -> EGL surface`

Bloom is omitted from the chain for now; it is not replaced with a fake glow filter.
