# MC VHS PORT phase 1 patch

This is an overlay patch for the existing `thinhtran2252/MC-VHS-PORT` repository.

1. Make a backup or commit your current working tree.
2. Extract this archive directly into the repository root.
3. Allow the listed files to be overwritten when prompted.
4. Build with the repository's `scripts/package.ps1 -Abi arm64-v8a` command.

The patch does not modify the Java source repository `mcvhs` and does not use LeviPostFX code.

The first pass adds a GLES 3 screen-space post-processing pipeline and Java-style settings/configuration. Bloom, world-space lighting, fog, SSR, water and material passes are not part of this phase.
