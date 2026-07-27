[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$PicoHome = Join-Path $HOME ".pico-sdk"
$SdkPath = Join-Path $PicoHome "sdk\2.3.0-full"
$ToolchainPath = Join-Path $PicoHome "toolchain\15_2_Rel1"
$CmakeExe = Join-Path $PicoHome "cmake\v4.3.4\cmake-4.3.4-windows-x86_64\bin\cmake.exe"
$NinjaExe = Join-Path $PicoHome "ninja\v1.13.2\ninja.exe"
$PioasmPath = Join-Path $PicoHome "tools\2.3.0\pioasm"
$PicotoolPath = Join-Path $PicoHome "picotool\2.3.0\picotool"
$BuildPath = Join-Path $PSScriptRoot "build\$($Configuration.ToLowerInvariant())"

$RequiredPaths = @(
    (Join-Path $SdkPath "pico_sdk_init.cmake"),
    (Join-Path $ToolchainPath "bin\arm-none-eabi-gcc.exe"),
    $CmakeExe,
    $NinjaExe,
    (Join-Path $PioasmPath "pioasmConfig.cmake"),
    (Join-Path $PicotoolPath "picotoolConfig.cmake")
)

foreach ($Path in $RequiredPaths) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing Pico build dependency: $Path"
    }
}

$env:PICO_SDK_PATH = $SdkPath
$env:PICO_TOOLCHAIN_PATH = $ToolchainPath
$env:Path = "$(Join-Path $ToolchainPath 'bin');$(Split-Path $CmakeExe);$(Split-Path $NinjaExe);$env:Path"

& $CmakeExe `
    -S $PSScriptRoot `
    -B $BuildPath `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DPICO_BOARD=pico" `
    "-DPICO_SDK_PATH=$SdkPath" `
    "-DPICO_TOOLCHAIN_PATH=$ToolchainPath" `
    "-Dpioasm_DIR=$PioasmPath" `
    "-Dpicotool_DIR=$PicotoolPath"

if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed."
}

& $CmakeExe --build $BuildPath --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

$Uf2Path = Join-Path $BuildPath "smartknob_rp2040.uf2"
if (-not (Test-Path -LiteralPath $Uf2Path)) {
    throw "Build completed without producing $Uf2Path"
}

Write-Host ""
Write-Host "UF2 ready: $Uf2Path"
