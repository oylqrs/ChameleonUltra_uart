param(
    [string]$Compiler = 'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe',
    [string]$Python = 'C:\Espressif\tools\python\v5.4.4\venv\Scripts\python.exe',
    [switch]$CompileOnly
)

$ErrorActionPreference = 'Stop'
$firmwareDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$hfDirectory = Join-Path $firmwareDirectory 'application\src\rfid\nfctag\hf'
$driverPath = Join-Path $hfDirectory 'nfc_14a.c'
$buildDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$testElf = Join-Path $buildDirectory 'nfc_14a_driver_test.elf'

# Compile the complete production driver body against a simulated NFCT/clock environment.
$driver = Get-Content -Raw $driverPath
$bodyStart = $driver.IndexOf('#if NFC_TAG_14A_RX_PARITY_AUTO_DEL_ENABLE')
if ($bodyStart -lt 0) { throw 'NFC 14A driver body not found' }
$driverSource = '#include "test_env.h"' + "`n" + $driver.Substring($bodyStart)

$compilerArguments = @(
    '-march=rv32ima', '-mabi=ilp32', '-std=c11', '-O0',
    '-ffreestanding', '-fno-builtin', '-msmall-data-limit=0', '-nostdlib',
    '-Wall', '-Wextra', '-Werror', '-fshort-enums',
    '-I', (Join-Path $PSScriptRoot 'stubs'), '-I', $hfDirectory,
    '-Wl,-Ttext=0x10000', '-Wl,-e,test_nfc_14a_driver', '-Wl,--no-relax',
    (Join-Path $PSScriptRoot 'test_nfc_14a_driver.c'),
    (Join-Path $PSScriptRoot '..\nfc_fido_bridge\freestanding.c'),
    '-o', $testElf, '-x', 'c', '-'
)

$driverSource | & $Compiler @compilerArguments
if ($LASTEXITCODE -ne 0) { throw 'NFC 14A driver harness compile failed' }
if ($CompileOnly) {
    Write-Output 'NFC 14A driver harness compiled; behavior tests not executed'
    return
}

& $Python (Join-Path $PSScriptRoot 'run_tests.py') $testElf --instruction-budget 20000000
if ($LASTEXITCODE -ne 0) { throw 'NFC 14A driver behavior tests failed' }
