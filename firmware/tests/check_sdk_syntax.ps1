param(
    [string]$Compiler = 'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe'
)
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    $includePaths = [System.Collections.Generic.List[string]]::new()
    $readingIncludes = $false
    foreach ($line in Get-Content application/Makefile) {
        if ($line.Trim() -match '^INC_FOLDERS\s*\+=') {
            $readingIncludes = $true
            continue
        }
        if (!$readingIncludes) { continue }
        $entry = $line.Trim().TrimEnd('\').Trim()
        if ($entry) {
            $entry = $entry.Replace('${PROJ_DIR}', 'application/src').Replace('${SRC_COMMON}', 'common').Replace('$(SDK_ROOT)', 'nrf52_sdk')
            $includePaths.Add('-I' + $entry)
        }
        if (!$line.TrimEnd().EndsWith('\')) { $readingIncludes = $false }
    }

    # This checks C declarations against the real SDK, without assembling ARM code.
    $compilerArgs = @(
        '-fsyntax-only', '-march=rv32ima', '-mabi=ilp32', '-std=gnu11',
        '-Wall', '-Werror', '-fshort-enums', '-DPROJECT_CHAMELEON_ULTRA',
        '-DAPP_TIMER_V2', '-DAPP_TIMER_V2_RTC1_ENABLED', '-DFLOAT_ABI_HARD',
        '-DNRF52840_XXAA', '-DNRF_SD_BLE_API_VERSION=7', '-DS140',
        '-DSOFTDEVICE_PRESENT', '-D__ARM_ARCH_7EM__',
        '-DAPP_FW_VER_MAJOR=2', '-DAPP_FW_VER_MINOR=0',
        '-DGIT_VERSION=\"syntax-check\"'
    ) + $includePaths.ToArray()
    $sources = @(
        'application/src/urat_esp/nfc_fido_bridge.c',
        'application/src/urat_esp/uart_esp.c',
        'application/src/utils/dataframe.c',
        'application/src/rfid/nfctag/hf/nfc_14a_4.c',
        'application/src/rfid/reader/hf/iso14443_4_reader.c',
        'application/src/rfid/nfctag/hf/nfc_14a.c',
        'application/src/rfid/nfctag/lf/lf_tag_em.c',
        'nrf52_sdk/modules/nrfx/drivers/src/nrfx_nfct.c',
        'application/src/rfid_main.c',
        'application/src/utils/syssleep.c',
        'application/src/app_cmd.c',
        'application/src/app_main.c',
        'application/src/usb_main.c',
        'application/src/ble_main.c'
    )
    & $Compiler @compilerArgs @sources
    if ($LASTEXITCODE -ne 0) { throw 'SDK syntax check failed' }
    Write-Output 'Real SDK C syntax check passed; ARM assembly and firmware linking were not tested.'
} finally {
    Pop-Location
}
