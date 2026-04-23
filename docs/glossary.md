# Glossary

| Term                   | Definition                                                                |
|------------------------|---------------------------------------------------------------------------|
| **auxiliary_bus**      | Linux kernel bus type (since 5.11) designed for a single parent device to spawn children on multiple subsystems. Replaces classic MFD platform_device wiring. |
| **BOOTSEL**            | The RP2350's ROM-resident USB mass-storage bootloader, activated by pulling a dedicated pin low at power-on. Used by RPBridge for OTA. |
| **can2040**            | Kevin O'Connor's open-source software CAN 2.0B controller that runs on RP2040/RP2350 PIO. The RPBridge firmware vendors it as a git submodule. |
| **capability map**     | CBOR-encoded description of what an RPBridge firmware build can do. Queried by the host at bind time. See [`protocol/capabilities.md`](protocol/capabilities.md). |
| **cdev v2**            | GPIO character-device ABI v2, stable since Linux 5.10. The only GPIO userspace ABI RPBridge supports (sysfs-GPIO is deprecated). |
| **CDC-ACM**            | USB Communications Device Class, Abstract Control Model. Used for the three virtual serial ports RPBridge exposes. |
| **CRC-32C**            | Cyclic redundancy check using the Castagnoli polynomial `0x1EDC6F41`. Used on every RPBP frame. |
| **DKMS**               | Dynamic Kernel Module Support — a Debian-originated framework that rebuilds out-of-tree modules on every kernel upgrade. |
| **gs_usb**             | Geschwister-Schneider USB / candleLight USB-to-CAN protocol. Standard on Linux (`drivers/net/can/usb/gs_usb.c`) and supported by every major CAN tool. |
| **IAD**                | USB Interface Association Descriptor — groups multi-interface functions (e.g. CDC-ACM's control + data interfaces) so host OSes load the right driver. |
| **IIO**                | Linux Industrial I/O subsystem — canonical home for ADCs, DACs, light/temp sensors. RPBridge's ADC lives here. |
| **MFD**                | Multi-Function Device — Linux kernel pattern for single hardware exposing multiple logical subsystems. RPBridge uses the modern auxiliary_bus variant, not the legacy MFD+platform_device pair. |
| **minisign**           | Ed25519-based file signing tool, used for UF2 artefact signing in releases. |
| **nusb**               | Pure-Rust USB library, default transport for the RPBridge userspace client. |
| **PIO**                | RP2350's Programmable I/O state machines — tiny, fast, user-programmable co-processors. 12 total, grouped into three PIO blocks of four SMs each. |
| **RPBP**               | **R**PBridge **P**rotocol — our wire format over the USB vendor bulk pipe. Current version: RPBP v1. |
| **SPSC**               | Single-producer, single-consumer — describes the lock-free ring buffers used between the two Cortex-M33 cores. |
| **TinyUSB**            | Embedded USB stack used by the firmware. Bundled with pico-sdk. |
| **UF2**                | USB Flashing Format — Microsoft-originated archive format accepted by the RP2350 ROM mass-storage bootloader. |
