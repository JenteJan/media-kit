# Build lib/windows_coverage.dart in Release mode. Prepare the test clips.
# The runner checks the Dart report. It also checks the process exit code.
#Requires -Version 7.0
param(
    [switch]$Software,
    [switch]$CpuDecoding,
    [switch]$RotationOnly,
    [string]$Library = '',
    [string]$MediaDirectory = (Join-Path $PSScriptRoot '../../build/test-media'),
    [string]$OutputDirectory = '',
    [int]$Port = 18768,
    [int]$TimeoutSeconds = 480
)

$ErrorActionPreference = 'Stop'
$mode = if ($Software) { 'sw-cpu' } elseif ($CpuDecoding) { 'd3d11-cpu' } else { 'hw' }
$media = (Resolve-Path -LiteralPath $MediaDirectory).Path
$release = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../build/windows/x64/runner/Release')).Path
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot "../../build/coverage-validation/$mode-$(Get-Date -Format yyyyMMdd-HHmmss)"
}
$out = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $out) { throw "Output directory already exists: $out" }
New-Item -ItemType Directory -Path "$out/app" -Force | Out-Null
Copy-Item -Path "$release/*" -Destination "$out/app" -Recurse
if ($Library) {
    Copy-Item -LiteralPath $Library -Destination "$out/app/libmpv-2.dll"
}
(Get-FileHash -LiteralPath "$out/app/libmpv-2.dll" -Algorithm SHA256).Hash |
    Set-Content "$out/libmpv-sha256.txt"
$serverScript = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'serve_media.mjs')).Path
$server = $null
$app = $null
try {
    $server = Start-Process node -ArgumentList @("`"$serverScript`"", "`"$media`"", "$Port") `
        -WindowStyle Hidden -RedirectStandardOutput "$out/server.log" `
        -RedirectStandardError "$out/server-errors.log" -PassThru
    Start-Sleep -Milliseconds 500
    if ($server.HasExited) { throw 'Fixture server failed to start' }
    $sources = @(
        (Join-Path $media 'landscape.mp4'),
        (Join-Path $media 'hevc-padded.mp4'),
        "http://127.0.0.1:$Port/wide.mp4",
        "http://127.0.0.1:$Port/hevc-main10.mp4",
        (Join-Path $media 'portrait.mp4'),
        "http://127.0.0.1:$Port/hevc-padded.mp4"
    )
    $arguments = @($sources | ForEach-Object { "`"$_`"" })
    if ($Software) { $arguments += '--software' }
    if ($CpuDecoding) { $arguments += '--cpu-decoding' }
    if ($RotationOnly) { $arguments += '--rotation-only' }
    $app = Start-Process "$out/app/media_kit_test.exe" -ArgumentList $arguments `
        -WorkingDirectory $out -WindowStyle Hidden -RedirectStandardOutput "$out/stdout.log" `
        -RedirectStandardError "$out/stderr.log" -PassThru
    $app.Id | Set-Content "$out/pid.txt"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    Write-Output "Started $mode coverage, PID=$($app.Id), reports=$out"
    $exited = $app.WaitForExit($TimeoutSeconds * 1000)
    $processResult = [ordered]@{
        exited = $exited
        exitCode = if ($exited) { $app.ExitCode } else { $null }
        seconds = $timer.Elapsed.TotalSeconds
    }
    $processResult | ConvertTo-Json | Set-Content "$out/process-result.json"
    if (!$exited) { throw "Timed out after $TimeoutSeconds seconds" }
    if ($app.ExitCode -ne 0) { throw "Process crashed: exit code $($app.ExitCode)" }
    $report = Get-Content "$out/coverage-$mode.json" -Raw | ConvertFrom-Json
    if ($report.status -ne 'PASS' -or $report.errors.Count -ne 0) {
        throw "Coverage failed: $($report.errors -join '; ')"
    }
    $log = Get-Content "$out/stdout.log" -Raw
    $renderers = [regex]::Matches($log, 'VideoOutput: Using (D3D11|S/W) rendering')
    $expected = if ($Software) { 'S/W' } else { 'D3D11' }
    if ($renderers.Count -lt 6 -or ($renderers | Where-Object { $_.Groups[1].Value -ne $expected })) {
        throw "Expected every output to use $expected. See $out/stdout.log"
    }
    Write-Output "PASS: $mode; exit code 0; report $out/coverage-$mode.json"
} finally {
    # Stop the child processes that this run started.
    if ($app -and !$app.HasExited) { $app.Kill(); $app.WaitForExit() }
    if ($server -and !$server.HasExited) { $server.Kill(); $server.WaitForExit() }
}
