# The screenshot regression: renders the shell hidden and offscreen (ui-shot) with a made-up asset
# list, one PNG per scripted step, at several window sizes. No window is shown, no mouse or keyboard
# is read, no settings file is written. Usage:
#   tools\ui-shots.ps1 -Exe <GrayWolf.exe> -Out <folder> [-Sizes 1280x760,1920x1080,900x600] [-Dpi 1.0]
# Each shot lands in <Out>\<size>\<script>\<name>.png; the exit code is 0 when every run wrote its shots.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$Sizes = @('1280x760', '1920x1080', '900x600'),
    [double]$Dpi = 1.0
)
$ErrorActionPreference = 'Stop'
$scripts = Get-ChildItem -Path (Join-Path $PSScriptRoot 'ui-shots') -Filter '*.json'
$failed = 0
foreach ($size in $Sizes) {
    foreach ($s in $scripts) {
        $dir = Join-Path $Out (Join-Path $size $s.BaseName)
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        & $Exe ui-shot $s.FullName --out $dir --size $size --dpi $Dpi | Out-Null
        $shots = (Get-Content $s.FullName -Raw | ConvertFrom-Json).steps | Where-Object { $_.shot } | ForEach-Object { $_.shot }
        foreach ($shot in $shots) {
            if (-not (Test-Path (Join-Path $dir "$shot.png"))) { Write-Host "MISSING $size $($s.BaseName) $shot"; $failed++ }
        }
        if ($LASTEXITCODE -ne 0) { Write-Host "exit $LASTEXITCODE for $size $($s.BaseName)"; $failed++ }
    }
}
if ($failed -eq 0) { Write-Host "ui-shots: ok" } else { Write-Host "ui-shots: $failed failure(s)" }
exit ([int]($failed -ne 0))
