# Next stage candidates

1. Load `Spotlight.png` as a real GLES texture and implement `USE_MORE_STATIC` from the exact two-channel threshold test in `composite2.fsh`.
2. Implement the two-pass horizontal/vertical bloom path from `composite3.fsh`, `composite4.fsh` and `Bloom.glsl` with a dedicated emission render target.
3. Investigate whether Preloader/Bedrock exposes previous and current camera position safely; only then implement Java interlacing.
4. Investigate world render/depth hooks before attempting fog, torch or SSR. Keep them disabled until the required depth/matrix/normal inputs exist.
