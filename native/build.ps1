param([string]$Compiler='tcc.exe')
$ErrorActionPreference='Stop'
$nativeRoot=$PSScriptRoot
$outputRoot=Join-Path (Split-Path $PSScriptRoot) 'build'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
if(-not(Test-Path -LiteralPath (Join-Path $nativeRoot 'target_config.h'))){Copy-Item -LiteralPath (Join-Path $nativeRoot 'target_config.example.h') -Destination (Join-Path $nativeRoot 'target_config.h')}
foreach($entry in @(@('sata_guard.c','SataGuard.dll','shared'),@('target_guard.c','TargetSataGuard.dll','shared'),@('probe_target.c','identify_target.exe','exe'),@('smoke_guard.c','smoke_guard.exe','exe'))){
    $compileArgs=@()
    if($entry[2] -eq 'shared'){$compileArgs+='-shared'}
    $compileArgs+=@('-o',(Join-Path $outputRoot $entry[1]),(Join-Path $nativeRoot $entry[0]),'-lkernel32','-luser32')
    & $Compiler @compileArgs
    if($LASTEXITCODE -ne 0){throw ('Compilation failed: '+$entry[0])}
}
& (Join-Path $outputRoot 'smoke_guard.exe') (Join-Path $outputRoot 'SataGuard.dll')
if($LASTEXITCODE -ne 0){throw 'Offline native smoke test failed'}
Write-Output 'Compiled. Only the memory-only smoke test was run. No hardware probe was executed.'
