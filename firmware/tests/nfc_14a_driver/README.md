# NFC 14A driver behavior tests

This harness compiles the complete implementation body from
`application/src/rfid/nfctag/hf/nfc_14a.c` against simulated NFCT registers,
the nrfx NFCT driver boundary, and the legacy clock driver boundary.

It covers the RF receive path that sits below the NFC FIDO bridge:

- delayed HFXO startup before NFCT activation;
- field loss while HFXO startup is pending;
- balanced HFXO requests/releases across field cycles and disable;
- RX recovery after `FRAMEDELAYTIMEOUT`;
- rejection and RX re-arming for frames with a nonzero `rx_status`;
- ATQA replies for REQA/WUPA retries in READY and ACTIVE states;
- disable/re-enable followed by a fresh field detection.

Run from the firmware root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\nfc_14a_driver\run_tests.ps1
```

The harness validates application behavior at the nrfx boundary. The modified
nrfx driver's private field-filter state and the physical HFXO/NFCT timing still
require an nRF52840 hardware test.
