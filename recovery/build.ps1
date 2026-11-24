param([Parameter(Mandatory=$true)][string]$Package,
      [Parameter(Mandatory=$true)][string]$Compiler,
      [ValidateSet('Factory','Nand','Identify')][string]$Mode='Factory')
$ErrorActionPreference='Stop'
$packageRoot=(Resolve-Path -LiteralPath $Package).Path
foreach($required in @('recovery_config.h','factory_data.h','target_guard.c','sata_guard.c','factory_policy.h','factory_runner.c')){
    if(-not(Test-Path -LiteralPath (Join-Path $packageRoot $required))){throw "Missing prepared input: $required"}
}
Push-Location -LiteralPath $packageRoot
try {
    $modeFlags=@()
    if($Mode -eq 'Factory'){$modeFlags+='-DFACTORY_RUNTIME'}
    if($Mode -eq 'Nand'){$modeFlags+='-DNAND_DIAGNOSTICS'}
    & $Compiler @modeFlags -shared -o TargetSataGuard.dll target_guard.c -lkernel32 -luser32
    if($LASTEXITCODE -ne 0){throw 'Factory guard compilation failed'}
    & $Compiler -o factory_runner.exe factory_runner.c -lkernel32 -luser32
    if($LASTEXITCODE -ne 0){throw 'Factory runner compilation failed'}
    & $Compiler -shared -o SataGuard.dll sata_guard.c -lkernel32 -luser32
    if($LASTEXITCODE -ne 0){throw 'Offline bridge compilation failed'}
    & $Compiler -o smoke_guard.exe smoke_guard.c -lkernel32 -luser32
    if($LASTEXITCODE -ne 0){throw 'Smoke test compilation failed'}
    & .\smoke_guard.exe (Join-Path $packageRoot 'SataGuard.dll')
    if($LASTEXITCODE -ne 0){throw 'Memory-only smoke test failed'}
    $hashes=@{}
    foreach($name in @('TargetSataGuard.dll','factory_runner.exe','recovery_config.h','factory_data.h','target_guard.c','sata_guard.c','factory_policy.h','factory_runner.c')){
        $hashes[$name]=(Get-FileHash -LiteralPath $name -Algorithm SHA256).Hash
    }
    @{files=$hashes;hardware_executed=$false} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath build-manifest.json -Encoding utf8
    Write-Output 'Compiled and tested in memory. No recovery executable was run.'
} finally {Pop-Location}
