param(
    [string]$Profile = 'D:\Development\LIMINAL-IX\play-liminal-audio-rc0',
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Profile -ErrorAction Stop).ProviderPath
$exe = Join-Path $root 'bin\xrEngine.exe'
$config = Join-Path $root 'fsgame.ltx'
$stamp = Join-Path $root 'LIMINAL-LOCAL-AUDIO-BASELINE.json'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing IX-Ray: $exe" }
if (!(Test-Path -LiteralPath $config -PathType Leaf)) { throw "Missing fsgame.ltx: $config" }
if (!(Test-Path -LiteralPath $stamp -PathType Leaf)) { throw "This is not a staged LIMINAL audio profile: $stamp" }
$m = Get-Content -LiteralPath $stamp -Raw -Encoding utf8 | ConvertFrom-Json
if ($m.schema -ne 'LIMINAL_LOCAL_AUDIO_BASELINE_V1') { throw "Unknown stage receipt" }
if ($m.target -ine $root) { throw "Profile/receipt identity mismatch" }
if ($m.copied -ne $true) { throw "Stage not reported complete" }
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -ne $m.checked_core_sha256.'bin/xrEngine.exe') { throw "Engine binary hash drift" }
$path = Join-Path $root 'gamedata\levels\pripyat_full\level.snd_rooms'
if (!(Test-Path -LiteralPath $path)) { throw "Missing active room catalog" }
if ($VerifyOnly) {
    Write-Output ("LIMINAL RC0 verified for native menu launch: " + $root)
    Write-Output "Note: game-world QA and physical v6 atlas not yet accepted."
    exit 0
}
Write-Output ("Launching LIMINAL audio profile: " + $root)
Push-Location $root
try {
    & $exe -fsltx fsgame.ltx -r4
} finally {
    Pop-Location
}
