# Hardware — bring-up log

This page is populated as rev-B prototypes come off the SMT line. Each
entry records what was measured, what surprised us, and what was
changed. The format is intentionally loose — this is an engineering
log, not API documentation.

## Template

```text
## YYYY-MM-DD — serial RPB-0000xxxx

- Power-on current: ... mA
- +3V3: ... V
- LDO thermal: ambient → ... °C at idle, ... °C at full bus load
- USB enumeration: ...
- I²C scan (bus 0): ...
- I²C scan (bus 1): ...
- SPI loopback 24 MHz: ...
- CAN loopback against CANable: ...
- RS485 against a known-good master: ...
- RGB LED colour calibration (PWM % for neutral white): R=... G=... B=...
- Issues observed:
- Changes planned:
```

No entries yet — the first rev-B run is pending. Updates land via a
PR labelled `hardware/bringup`.
