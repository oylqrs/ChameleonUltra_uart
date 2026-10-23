# Bridge and ISO-DEP integration regression

Links the production `nfc_fido_bridge.c`, `nfc_14a_4.c`, and CRC-A helpers.
Only the NFC register driver, SDK timers, critical sections, and mode/sleep
integration are replaced. The real ISO-DEP header takes precedence over the
bridge unit-test stubs.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/nfc_fido_bridge_rf/run_tests.ps1
```

Uses the same RV32 compiler and Unicorn/pyelftools runner as
`tests/nfc_fido_bridge`. `-CompileOnly` does not execute behavior tests.

Checks two interactions that isolated bridge and RF tests cannot establish:

- A reader sends a new CAPDU after receiving its RAPDU but before the old
  UART POLL ACK. The old event stays byte-identical while the new CAPDU queues
  behind it. A POLL that ACKs the old event returns the new CAPDU immediately,
  without waiting for the WTX timer. Repeated old RESPONSE only returns its
  UART ACK and never starts an RF response for the new CAPDU.
- FIELD_OFF cancels an armed WTX timer and rejects late RESPONSE. After a
  new field arrives behind the retained OFF event, a stale timer callback
  cannot send or alter the new timer. OFF, ON, and the new APDU stay ordered.

The harness also checks temporary-handler/mode restoration and no slot
flash writes. It does not verify NFC hardware timing, interrupts, DMA, or
ARM linking. Those still require the target build and reader testing.
