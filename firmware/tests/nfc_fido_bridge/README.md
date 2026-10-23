# NFC FIDO bridge regression tests

These tests link the real `application/src/urat_esp/nfc_fido_bridge.c` against
minimal RF, device-mode, clock and Nordic SDK stubs. They call public APIs and
inspect UART payloads and RF calls. The critical-section macros have the same
lexical scope as the SDK's SoftDevice macros.

Coverage includes strict CONFIGURE validation and echo; temporary RF profile;
first snapshot preservation across reconfiguration; RESET/disable restoration;
FIELD_ON ordering and returned-event visibility; RESPONSE and POLL acknowledgement gates; duplicate RESPONSE
without a second RF submission, including a retry after the next CAPDU;
FIELD_OFF priority and late-response rejection; coalescing sessions that end
before the host observes them; unchanged `61xx`, `9100` and cancel bytes; APDU
size limits; lock-free large RAPDU submission/cache checks; field loss during
response submission; RF submission failure; and idle/pending-APDU watchdogs
including RTC wrap.

Run with a native C compiler on Linux or macOS:

```sh
sh tests/nfc_fido_bridge/run_tests.sh
```

On this Windows workspace, use the already installed ESP RISC-V compiler and
Python, with `unicorn` and `pyelftools` available in Python or `build/deps`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/nfc_fido_bridge/run_tests.ps1
```

The PowerShell runner accepts `-Compiler`, `-Python` and `-CompileOnly`.
The Python runner also accepts an ELF path for another freestanding RV32
harness whose entry point returns zero on success or the failing C line number.
No runner installs packages. Real RF timing, WTX and hardware UART remain
separate hardware acceptance checks.
