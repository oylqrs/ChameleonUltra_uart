param(
    [string]$Compiler = 'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe',
    [string]$Python = 'C:\Espressif\tools\python\v5.4.4\venv\Scripts\python.exe',
    [switch]$CompileOnly
)

$ErrorActionPreference = 'Stop'
$firmwareDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$readerDirectory = Join-Path $firmwareDirectory 'application\src\rfid\reader\hf'
$buildDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$testElf = Join-Path $buildDirectory 'nfc_iso_dep_reader_test.elf'
$compilerArguments = @(
    '-march=rv32ima', '-mabi=ilp32', '-std=c11', '-O0',
    '-ffreestanding', '-fno-builtin', '-msmall-data-limit=0', '-nostdlib',
    '-Wall', '-Wextra', '-Werror', '-DRC522_H',
    '-include', (Join-Path $PSScriptRoot 'stubs\rc522.h'),
    '-I', (Join-Path $PSScriptRoot 'stubs'), '-I', $readerDirectory,
    '-Wl,-Ttext=0x10000', '-Wl,-e,test_iso_dep_reader', '-Wl,--no-relax',
    (Join-Path $readerDirectory 'iso14443_4_reader.c'),
    (Join-Path $PSScriptRoot 'test_reader.c'),
    (Join-Path $PSScriptRoot 'freestanding.c'),
    '-o', $testElf
)
& $Compiler @compilerArguments
if ($LASTEXITCODE -ne 0) { throw 'ISO-DEP reader harness compile failed' }
if ($CompileOnly) {
    Write-Output 'ISO-DEP reader harness compiled; behavior tests not executed'
    return
}
& $Python (Join-Path $PSScriptRoot '..\nfc_fido_bridge\run_tests.py') $testElf
if ($LASTEXITCODE -ne 0) { throw 'ISO-DEP reader behavior tests failed' }
