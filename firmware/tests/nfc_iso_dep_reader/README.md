# ISO-DEP reader regression harness

This harness compiles the production `iso14443_4_reader.c` against a scripted
RC522 transport and executes it in the same freestanding RV32 emulator used by
the other NFC tests.

It checks:

- the ISO-DEP chaining flag (`0x10`) and response reassembly;
- R(ACK) with the next expected block number;
- independent reader and card block-number sequences;
- S(WTX) echo and WTXM-scaled timeout before and between response fragments;
- exact response-buffer capacity and overflow reporting;
- CRC, invalid-PCB, and wrong-block-number failures;
- clearing stale RC522 IRQ state and restoring the previous timeout.

Run on Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/nfc_iso_dep_reader/run_tests.ps1
```
