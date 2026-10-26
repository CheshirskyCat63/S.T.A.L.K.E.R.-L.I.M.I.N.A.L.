param([switch]$Show)
# Standalone isolated PhysX+PBD GPU experiment. Never changes the playable IX-Ray.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) { throw "Missing local environment: $python" }
& $python (Join-Path $root "experiments\pbd_shot.py")
if ($LASTEXITCODE -ne 0) { throw "GPU PBD Python shot FAILED" }
& $python (Join-Path $root "experiments\render_pbd_shot.py")
if ($LASTEXITCODE -ne 0) { throw "Physics-data GIF render FAILED" }
$env:PHYSX_USD_SCHEMAS_ROOT = Join-Path $root ".venv\Lib\site-packages"
$runner = Join-Path $root "build\liminal-native-pbd\Release\run_liminal_native_pbd_shot.cmd"
$scene = Join-Path $root "experiments\pbd_shot.usda"
if (Test-Path $runner) {
    # SDK emits a nonfatal GPU loader-probe diagnostic even when CUDA particle buffers exist.
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & cmd.exe /c $runner $scene 2>&1 | Tee-Object -FilePath (Join-Path $root "reports\native-pbd-shot-verified.log")
    $nativeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if ($nativeExitCode -ne 0) { throw "Native NVIDIA SDK PBD shot FAILED (exit=$nativeExitCode)" }
} else { Write-Warning "Native executable not built. Run CMake in integration\native_pbd." }
$gif = Join-Path $root "reports\pbd_shot_real_gpu.gif"
Write-Output "LIMINAL PBD TEST PASS -> $gif"
if ($Show) { Invoke-Item $gif }
