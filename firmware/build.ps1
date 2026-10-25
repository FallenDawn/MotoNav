param(
    [ValidateSet('185b')][string]$Board = '185b',
    [ValidateSet(0,1,2)][int]$PanelVariant = 0,
    [string]$PioPath = 'pio',
    [ValidateRange(1,32)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$environment = switch ($Board) {
    '185b' { 'esp32s3-185b' }
}

& $PioPath run -e $environment -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed ($LASTEXITCODE)" }

$taskBuild = Join-Path $taskRoot ".pio/build/$environment"
$taskAppPath = Join-Path $taskBuild 'firmware.bin'
$taskFullPath = Join-Path $taskBuild 'firmware.factory.bin'
if (-not (Test-Path -LiteralPath $taskAppPath) -or -not (Test-Path -LiteralPath $taskFullPath)) {
    throw 'PlatformIO did not produce the application and factory images.'
}

$taskDest = Join-Path $taskRoot "dist/$Board/panel$PanelVariant"
New-Item -ItemType Directory -Force $taskDest | Out-Null
$taskLabel = '1.85B'
$taskName = "MotoNav_${taskLabel}_v7.12.1-qweather_panel$PanelVariant"
Copy-Item -LiteralPath $taskAppPath -Destination (Join-Path $taskDest "${taskName}_app_0x10000.bin") -Force
Copy-Item -LiteralPath $taskFullPath -Destination (Join-Path $taskDest "${taskName}_full_0x0.bin") -Force
Get-ChildItem -LiteralPath $taskDest -File | Get-FileHash -Algorithm SHA256 | Format-Table -AutoSize
