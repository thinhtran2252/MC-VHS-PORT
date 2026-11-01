param(
    [ValidateSet("arm64-v8a", "armeabi-v7a", "all")]
    [string]$Abi = "arm64-v8a",
    [string]$BuildRoot = "build",
    [string]$AndroidPlatform = "android-28",
    [string]$NdkHome = "",
    [string]$Generator = "Ninja"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")

function Resolve-NdkHome {
    if ($NdkHome -and (Test-Path $NdkHome)) { return (Resolve-Path $NdkHome).Path }
    if ($env:ANDROID_HOME) {
        $preferred = Join-Path $env:ANDROID_HOME "ndk/28.2.13676358"
        if (Test-Path $preferred) { return (Resolve-Path $preferred).Path }
        $root = Join-Path $env:ANDROID_HOME "ndk"
        if (Test-Path $root) {
            $candidate = Get-ChildItem $root -Directory | Sort-Object Name -Descending | Select-Object -First 1
            if ($candidate) { return $candidate.FullName }
        }
    }
    if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) { return (Resolve-Path $env:ANDROID_NDK_HOME).Path }
    throw "Android NDK not found. Set ANDROID_NDK_HOME or ANDROID_HOME."
}

function Invoke-ConfigGeneration {
    $configBuild = Join-Path $repoRoot "$BuildRoot-config"
    & cmake -S $repoRoot -B $configBuild -G $Generator
    if ($LASTEXITCODE -ne 0) { throw "Config CMake configure failed" }
    & cmake --build $configBuild --target levi_generate_config
    if ($LASTEXITCODE -ne 0) { throw "Config generation failed" }
    return (Resolve-Path (Join-Path $configBuild "generated-config")).Path
}

function Invoke-ModBuild([string]$TargetAbi) {
    $ndk = Resolve-NdkHome
    $toolchain = Join-Path $ndk "build/cmake/android.toolchain.cmake"
    $buildDir = Join-Path $repoRoot "$BuildRoot-$TargetAbi"
    $configDir = Invoke-ConfigGeneration
    & cmake -S $repoRoot -B $buildDir -G $Generator `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
        -DANDROID_ABI="$TargetAbi" `
        -DANDROID_PLATFORM="$AndroidPlatform" `
        -DANDROID_STL="c++_shared" `
        -DLEVI_PACKAGE_CONFIG_DIR="$configDir"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $TargetAbi" }
    & cmake --build $buildDir --target levi_package
    if ($LASTEXITCODE -ne 0) { throw "Build/package failed for $TargetAbi" }
}

if ($Abi -eq "all") {
    Invoke-ModBuild "arm64-v8a"
    Invoke-ModBuild "armeabi-v7a"
} else {
    Invoke-ModBuild $Abi
}
