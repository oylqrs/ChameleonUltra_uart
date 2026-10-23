Run `powershell -File tests/nfc_iso_dep/run_tests.ps1` with the ESP-IDF RISC-V
compiler and Python environment. The Python runner is shared with
`tests/nfc_fido_bridge` and requires its Unicorn and pyelftools dependencies.

The harness compiles production ISO-DEP, CRC-A, and parity code. Hardware and
timer stubs cover 297-byte command assembly, 4088-byte response fragmentation,
CRC/block numbers, CID, retransmission, delayed WTX, a deferred bridge callback,
response publication, DESELECT, field loss, and repeated temporary profiles.
Timer callbacks are triggered explicitly; actual NFCT timing needs hardware.
