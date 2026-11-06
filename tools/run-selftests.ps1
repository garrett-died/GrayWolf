# tools/run-selftests.ps1 - the GrayWolf self-tests that need no game data, run against one exe.
#
# Usage: pwsh tools/run-selftests.ps1 [-Exe <GrayWolf.exe>] [-Out <scratch dir>]
#
# Each test runs with --report <file> (the exe's console output does not reach a pipe) and passes on
# exit code 0. The CI build (.github/workflows/build.yml) runs this on the Release exe; run it on
# x64/DevAsan/GrayWolf.exe (tools/build.sh DevAsan) to run the same tests under AddressSanitizer:
# an ASan report goes to <Out>/asan.<pid> and fails the test.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\src\GrayWolf\x64\Release\GrayWolf.exe'),
    [string]$Out = (Join-Path ([IO.Path]::GetTempPath()) 'graywolf-selftests')
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Get-ChildItem $Out -Filter 'asan.*' -ErrorAction SilentlyContinue | ForEach-Object { $_.Delete() }
# ASan's option separator is ':', so the log path is quoted (a no-op for a non-ASan exe)
$env:ASAN_OPTIONS = "log_path='" + (Join-Path $Out 'asan') + "'"

$tests = @(
    @{ Name = 'self-test';         Args = @('self-test') },
    @{ Name = 'seanim-selftest';   Args = @('seanim-selftest') },
    @{ Name = 'cast-selftest';     Args = @('cast-selftest') },
    @{ Name = 'gltf-selftest';     Args = @('gltf-selftest', (Join-Path $Out 'selftest.glb')) },
    @{ Name = 'salsa20-selftest';  Args = @('salsa20-selftest') },
    @{ Name = 'cerberus-selftest'; Args = @('cerberus-selftest') },
    @{ Name = 'ui-selftest'; Args = @('ui-selftest') },
    @{ Name = 'exr-selftest';      Args = @('exr-selftest') },
    # the shell rendered hidden with a made-up list: no window, a PNG written (ui-shot exits 5 when none was)
    @{ Name = 'ui-shot';           Args = @('ui-shot', (Join-Path $PSScriptRoot 'ui-shots\extract.json'), '--out', (Join-Path $Out 'ui-shot'), '--size', '900x600') }
)

$failed = @()
foreach ($t in $tests) {
    $report = Join-Path $Out "$($t.Name).txt"
    if (Test-Path $report) { [IO.File]::Delete($report) }
    $argList = @($t.Args) + @('--report', $report) | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $Exe -ArgumentList $argList -Wait -PassThru -NoNewWindow
    $asan = @(Get-ChildItem $Out -Filter 'asan.*' -ErrorAction SilentlyContinue)
    $text = if (Test-Path $report) { (Get-Content $report -Raw) } else { '' }
    $ok = ($p.ExitCode -eq 0) -and ($asan.Count -eq 0)
    '{0,-18} {1} (exit {2})' -f $t.Name, $(if ($ok) { 'ok' } else { 'FAIL' }), $p.ExitCode
    if (-not $ok) {
        $failed += $t.Name
        if ($text) { $text.TrimEnd() -split "`n" | Select-Object -Last 15 | ForEach-Object { "    $_" } }
        foreach ($a in $asan) { Get-Content $a.FullName | ForEach-Object { "    $_" }; $a.Delete() }
    }
}
if ($failed.Count) { "FAILED: $($failed -join ', ')"; exit 1 }
'all self-tests passed'
