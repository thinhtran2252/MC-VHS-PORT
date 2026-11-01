# Applying MC VHS PORT rebuild 0.2.0

This is a source overlay, not an installable `.levipack`.

1. Download `MC-VHS-PORT-rebuild-overlay-0.2.0.zip`.
2. Extract it into the root of the `MC-VHS-PORT` repository.
3. Allow the extracted files to replace files with matching paths.
4. Commit the replacement as one rebuild commit.
5. Run the Android build workflow or `./scripts/package.ps1 -Abi arm64-v8a` from a Windows/PowerShell build environment.

The new CMake file pins Preloader Android to the same commit as the reference LeviPostFX project. The previous phase-one archive in the repository is not used by this source tree and can be removed separately after confirming the rebuild commit.
