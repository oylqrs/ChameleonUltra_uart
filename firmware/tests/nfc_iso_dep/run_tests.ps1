param(
    [string]$Compiler = 'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe',
    [string]$Python = 'C:\Espressif\tools\python\v5.4.4\venv\Scripts\python.exe',
    [switch]$CompileOnly
)

$ErrorActionPreference = 'Stop'
$firmwareDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$rfDirectory = Join-Path $firmwareDirectory 'application\src\rfid'
$hfDirectory = Join-Path $rfDirectory 'nfctag\hf'
$buildDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$testElf = Join-Path $buildDirectory 'nfc_iso_dep_test.elf'

# Compile the production parity helpers without the NFCT register driver.
$driver = Get-Content -Raw (Join-Path $hfDirectory 'nfc_14a.c')
$parityStart = $driver.IndexOf('uint16_t nfc_tag_14a_wrap_frame(')
$parityEnd = $driver.IndexOf('#define NFC_14A_TX_BYTE_CORE', $parityStart)
if ($parityStart -lt 0 -or $parityEnd -lt $parityStart) { throw 'Parity helpers not found' }
$paritySource = '#include "nfc_14a.h"' + "`n" + '#include <string.h>' + "`n" +
                $driver.Substring($parityStart, $parityEnd - $parityStart)
$compilerArguments = @(
    '-march=rv32ima', '-mabi=ilp32', '-std=c11', '-O0',
    '-ffreestanding', '-fno-builtin', '-msmall-data-limit=0', '-nostdlib',
    '-Wall', '-Wextra', '-Werror', '-fshort-enums',
    '-I', (Join-Path $PSScriptRoot 'stubs'), '-I', $hfDirectory,
    '-I', (Join-Path $rfDirectory 'nfctag'), '-I', $rfDirectory,
    '-Wl,-Ttext=0x10000', '-Wl,-e,test_iso_dep', '-Wl,--no-relax',
    (Join-Path $hfDirectory 'nfc_14a_4.c'),
    (Join-Path $rfDirectory 'crc_utils.c'),
    (Join-Path $PSScriptRoot 'test_iso_dep.c'),
    '-o', $testElf, '-x', 'c', '-'
)
$paritySource | & $Compiler @compilerArguments
if ($LASTEXITCODE -ne 0) { throw 'ISO-DEP harness compile failed' }
if ($CompileOnly) {
    Write-Output 'ISO-DEP harness compiled; behavior tests not executed'
    return
}
& $Python (Join-Path $PSScriptRoot '..\nfc_fido_bridge\run_tests.py') $testElf --instruction-budget 20000000
if ($LASTEXITCODE -ne 0) { throw 'ISO-DEP behavior tests failed' }
