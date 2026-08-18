$ErrorActionPreference = 'Stop'

$helperPath = Join-Path $PSScriptRoot '..\..\cmake-build-hidmaestro\tools\sunshine-hidmaestro-host.exe'
if (-not (Test-Path -LiteralPath $helperPath)) {
  throw "HIDMaestro helper not found: $helperPath"
}

& $helperPath --driver-hold-test 900
$exitCode = $LASTEXITCODE
Write-Host "Hold test exited with code $exitCode."
Read-Host 'Press Enter to close this window'
exit $exitCode
