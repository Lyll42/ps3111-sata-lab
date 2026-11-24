param([Parameter(Mandatory=$true)][string]$Package,
      [Parameter(Mandatory=$true)][string]$Compiler)
$ErrorActionPreference='Stop'
$captureRoot=(Resolve-Path -LiteralPath $Package).Path
if(-not(Test-Path -LiteralPath (Join-Path $captureRoot 'SIMULATION_ONLY.txt'))){throw 'Missing offline marker'}
Push-Location -LiteralPath $captureRoot
try {
    & $Compiler -shared -o TargetSataGuard.dll capture_guard.c -lkernel32 -luser32
    if($LASTEXITCODE -ne 0){throw 'Offline guard compilation failed'}
    & $Compiler -o capture_runner.exe capture_runner.c -lkernel32 -luser32
    if($LASTEXITCODE -ne 0){throw 'Offline runner compilation failed'}
    Write-Output 'Offline tools compiled. No executable was run.'
} finally {Pop-Location}
