param(
    [string]$Compiler = 'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe',
    [string]$Python = 'C:\Espressif\tools\python\v5.4.4\venv\Scripts\python.exe',
    [switch]$CompileOnly
)

$ErrorActionPreference = 'Stop'
$firmwareDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$bridgeDirectory = Join-Path $firmwareDirectory 'application\src\urat_esp'
$rfDirectory = Join-Path $firmwareDirectory 'application\src\rfid'
$hfDirectory = Join-Path $rfDirectory 'nfctag\hf'
$buildDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$testElf = Join-Path $buildDirectory 'nfc_fido_bridge_rf_test.elf'
$compilerArguments = @(
    '-march=rv32ima', '-mabi=ilp32', '-std=c11', '-O0',
    '-ffreestanding', '-fno-builtin', '-msmall-data-limit=0', '-nostdlib',
    '-Wall', '-Wextra', '-Werror', '-fshort-enums',
    '-I', (Join-Path $PSScriptRoot 'stubs'), '-I', $hfDirectory,
    '-I', $bridgeDirectory, '-I', (Join-Path $rfDirectory 'nfctag'),
    '-I', $rfDirectory, '-I', (Join-Path $PSScriptRoot '..\nfc_iso_dep\stubs'),
    '-I', (Join-Path $PSScriptRoot '..\nfc_fido_bridge\stubs'),
    '-Wl,-Ttext=0x10000', '-Wl,-e,test_nfc_fido_bridge_rf', '-Wl,--no-relax',
    (Join-Path $bridgeDirectory 'nfc_fido_bridge.c'),
    (Join-Path $hfDirectory 'nfc_14a_4.c'),
    (Join-Path $rfDirectory 'crc_utils.c'),
    (Join-Path $PSScriptRoot 'test_bridge_rf.c'),
    (Join-Path $PSScriptRoot '..\nfc_fido_bridge\freestanding.c'),
    '-o', $testElf
)
& $Compiler @compilerArguments
if ($LASTEXITCODE -ne 0) { throw 'NFC FIDO bridge/RF harness compile failed' }
if ($CompileOnly) {
    Write-Output 'NFC FIDO bridge/RF harness compiled; behavior tests not executed'
    return
}
& $Python (Join-Path $PSScriptRoot '..\nfc_fido_bridge\run_tests.py') $testElf
if ($LASTEXITCODE -ne 0) { throw 'NFC FIDO bridge/RF behavior tests failed' }
