# rpbridge — Treiber-Stack & Versionierbares USB-Protokoll (v1.0)

> **Kurskorrektur gegenüber `rpbridge-windows-support.md`:** dieses Dokument definiert den
> **eigenen** Windows- und Linux-Treiber als *Primärpfad*. Inbox-Klassen (CDC-ACM, gs_usb) bleiben
> optional als **Kompatibilitätsmodus** für Nutzer, die keinen Treiber installieren wollen — sie
> sind aber **nicht** die Referenz-Integration.
>
> **Leitlinie:** state of the art, **sicher**, **performant**, **stabil**, **semantisch versioniert**,
> **vorwärts- und rückwärtskompatibel erweiterbar**.

---

## 1  Design-Prinzipien

| Prinzip | Konsequenz |
|---|---|
| **Ein Gerät, eine Treiber-Identität** | Composite-Device mit **einer** Vendor-Function (RPBP-Transport). Logische Kanäle (UART, CAN, I²C, SPI, GPIO, ADC …) werden **im Protokoll** multiplexed, nicht über zusätzliche USB-Interfaces. Das reduziert Deskriptor-Kombinatorik und macht Updates Feature-flag-getrieben statt USB-reenumeration-getrieben. |
| **Kernel-Minimalismus** | Der Kernel-Treiber ist **reiner Transport + Demux**. Business-Logik (Framing, CBOR, Capability-Parsing, Flow-Control-Policy) lebt in User-Space-Libraries. Kernel-Code ist klein, auditierbar, wenig fehleranfällig. |
| **Semantische Protokoll-Versionierung** | `MAJOR.MINOR.PATCH`. MAJOR-Break verboten innerhalb Produktlebenszyklus; MINOR = additive Features; PATCH = Clarifications. Host verhandelt mit Firmware beim Connect. |
| **Capability-Negotiation, keine statischen Matrizen** | Kein *„Firmware 1.4 → Treiber 1.3 → App 1.2"*-Jonglage. Jede Session beginnt mit `HELLO` → `CAPABILITIES` CBOR-Payload; Host nutzt nur, was Firmware anbietet. |
| **Signierte Artefakte Ende-zu-Ende** | RP2350 Secure Boot, signierte Firmware (minisign + SBOM), WHQL-signierter Windows-Treiber, signierte Linux-Module (optional), signierte User-Libraries (Sigstore). |
| **Keine Legacy-APIs** | Nix WDM, nix platform_device-MFD, nix sysfs-gpio, nix .NET Framework 4.x. |

---

## 2  Windows-Treiber — Referenz-Stack

### 2.1 Framework-Wahl

| Schicht | Wahl | Begründung |
|---|---|---|
| Driver Framework | **KMDF 1.31+** (Windows Driver Framework, Kernel-Mode) | Einziger von Microsoft für neue USB-Treiber empfohlener Weg. UMDF 2 wäre User-Mode und schöner (Isolation), aber für Geräte mit **Bulk-Streaming + IOCTL-Fan-out + kernel-nahe Performance** (CAN-Bursts, hohe UART-Raten) ist KMDF die pragmatische Wahl. Alternative Variante: **UMDF 2** falls Performance-Messungen zeigen dass User-Mode reicht (Update jederzeit möglich, KMDF→UMDF-Port trivial da Fx-APIs identisch). |
| INF-Modell | **DCH** (Declarative Componentized Hardware-support apps) | **Pflicht** für neue Treiber auf Windows 10 ≥ 1809 und Windows 11. Deklarativ, keine Co-Installer, keine RegisterDll, komponentenweise verteilbar. |
| Signatur | **Azure Trusted Signing** + **WHQL-Attested Signing** | EV-Codesign-Zertifikate sind ausgelaufen. Azure Trusted Signing ist die moderne Microsoft-Identity-bound Signatur; WHQL-Attested Signing ist für Kernel-Mode zusätzlich Pflicht. |
| Distribution | **Windows Update** (Driver Update) + **MSI-Fallback** | Treiber-Paket bei Microsoft Hardware Developer Center einreichen → „Publish to Windows Update". Fallback-MSI für Offline/Enterprise. |
| Hardware-Support-App (HSA) | **MSIX** via Microsoft Store | UI-Anteil (Status-Panel, Log-Viewer, Firmware-Updater) als UWP/WinUI-3 App; über DCH automatisch gekoppelt. Optional — CLI reicht für Release 1.0. |
| Userland-Lib | **Windows Runtime C++/WinRT** oder reines **Win32 API** | Von Python/.NET/Rust über schlanken C-Header ansprechbar. |

### 2.2 Device-Stack-Topologie

```
 User-App (Python, .NET, Rust, C++)
        │
        ▼
 rpbridge.dll  (C17, Win32)  ── IOCTL / ReadFile / WriteFile
        │
        ▼  \Device\rpbridge_N   (PDO-Name; N = Geräte-Instanz)
 ┌───────────────────────────────────────────┐
 │  rpbridge.sys  (KMDF 1.31, DCH, WHQL)     │
 │                                           │
 │  • Transport: Bulk-IN / Bulk-OUT queue    │
 │  • Protokoll-Demux (Channel-ID → FDO)     │
 │  • Per-Channel FDO:                       │
 │      uart0, uart1, uart2                  │
 │      can0                                 │
 │      i2c0, spi0                           │
 │      gpio0, adc0                          │
 │  • Power-Management (WDF_POWER_POLICY)    │
 │  • Selective Suspend                      │
 │  • Trace: WPP / ETW                       │
 └───────────────────────────────────────────┘
        │
        ▼
 WDF USB Target  →  USBD.SYS / UCX  →  xHCI  →  rpbridge-Hardware
```

### 2.3 Kernel-Treiber-Verantwortungen (was im `.sys` landet)

- Enumeration via **MS OS 2.0 Descriptor** (CompatibleID=RPBRIDGE + DeviceInterfaceGUIDs)
- **Bulk-Queue** mit *Continuous-Reader*-Pattern (WDF) für Bulk-IN
- **RPBP-Header-Parsing** minimal — nur Channel-Demux + CRC-Validierung + Length-Check; **keine** CBOR-Interpretation im Kernel
- **Pro-Channel Request-Queues** mit Fairness-Scheduling (Weighted Round-Robin zwischen Control/UART/CAN/…)
- **Flow-Control** (Credit-basiert, siehe §4.7) im Kernel, damit ein fluteder UART-Kanal keinen CAN-RX blockiert
- **IOCTL-Oberfläche** (`IOCTL_RPBRIDGE_*`) — gestaffelt:
  - **Synchrone** Kontroll-IOCTLs (Capability-Query, Channel-Open, Reset)
  - **Asynchrone** Read/Write via `DeviceIoControl(…, OVERLAPPED)` oder `ReadFile/WriteFile` auf Per-Channel-Handles
- **WPP/ETW Tracing** für `wtrace.exe`-Live-Debug; keine `DbgPrint`-Spam-Logs
- **Selective Suspend** korrekt (Idle-Detection, Wake-on-Event-Support)
- **Power-Managed Queues** damit Pending-I/O Suspend nicht blockiert
- **Cancel-Safe Queues** für User-Abort (Ctrl+C, Handle-Close)
- **PnP-Robustheit** — Surprise-Removal, Re-Plug, RundownProtection

### 2.4 IOCTL-Contract (gekürzt)

```c
/* rpbridge-ioctl.h — stable ABI, ausgeliefert mit SDK */

#define FILE_DEVICE_RPBRIDGE    0x8371

#define RPBRIDGE_IOCTL(fn, access) \
    CTL_CODE(FILE_DEVICE_RPBRIDGE, (fn), METHOD_BUFFERED, (access))

/* Control-plane */
#define IOCTL_RPBRIDGE_GET_PROTOCOL_VERSION   RPBRIDGE_IOCTL(0x801, FILE_READ_ACCESS)
#define IOCTL_RPBRIDGE_GET_CAPABILITIES       RPBRIDGE_IOCTL(0x802, FILE_READ_ACCESS) // CBOR out
#define IOCTL_RPBRIDGE_OPEN_CHANNEL           RPBRIDGE_IOCTL(0x803, FILE_WRITE_ACCESS)
#define IOCTL_RPBRIDGE_CLOSE_CHANNEL          RPBRIDGE_IOCTL(0x804, FILE_WRITE_ACCESS)
#define IOCTL_RPBRIDGE_CONFIGURE_CHANNEL      RPBRIDGE_IOCTL(0x805, FILE_WRITE_ACCESS) // CBOR in
#define IOCTL_RPBRIDGE_RESET                  RPBRIDGE_IOCTL(0x806, FILE_WRITE_ACCESS)

/* Data-plane (pro Kanal; Handle wird per OPEN_CHANNEL geliefert) */
#define IOCTL_RPBRIDGE_STREAM_READ            RPBRIDGE_IOCTL(0x810, FILE_READ_ACCESS)
#define IOCTL_RPBRIDGE_STREAM_WRITE           RPBRIDGE_IOCTL(0x811, FILE_WRITE_ACCESS)
#define IOCTL_RPBRIDGE_STREAM_IOCTL           RPBRIDGE_IOCTL(0x812, FILE_WRITE_ACCESS) // CBOR in/out

/* Event-plane (Inverted Call) */
#define IOCTL_RPBRIDGE_WAIT_EVENT             RPBRIDGE_IOCTL(0x820, FILE_READ_ACCESS)

typedef struct _RPBRIDGE_VERSION {
    UINT16  ProtocolMajor;   // RPBP major
    UINT16  ProtocolMinor;
    UINT16  ProtocolPatch;
    UINT16  DriverBuild;     // this .sys build number
    UINT32  FeatureFlags;    // Bitmap, siehe §4.6
} RPBRIDGE_VERSION, *PRPBRIDGE_VERSION;
```

**Versionierungs-Regel:**
- Neue IOCTLs bekommen **neue Function-Codes** — bestehende sind *immutable*.
- Bestehende Strukturen werden **nur am Ende erweitert**; Version-Feld im Header signalisiert die Länge.
- Caller füllt `InputBuffer` mindestens bis `ProtocolMinor` aus → Treiber liest nur was der Caller versteht.

### 2.5 WHQL-Pfad & HLK-Tests

Für WHQL-Zertifizierung laufen pflicht folgende Tests aus dem **Windows Hardware Lab Kit (HLK)**:

- `Device.BusController.UsbDevice.UsbBusCompliance` — Standard-USB-Compliance
- `Device.BusController.UsbDevice.SelectiveSuspend` — Power-Management
- `Device.DevFund.DriverFramework.UniversalKmdf` — KMDF-Konformität
- `Static Driver Verifier` (SDV) mit Rule-Sets `WDF` + `USB` — statische Analyse
- `Device Fundamentals` (DF) — PnP/Power-Stress
- `SleepStressWithIO`, `PlugAndPlayStress`, `ReInstallDevice`

Plus `Code-Analysis` mit MSVC `/W4 /WX` + `/analyze`. SDV + CodeQL in CI laufen lassen, **bevor** das Image zum Hardware Dev Center hochgeladen wird.

### 2.6 Distribution & Update

1. **Build**: `cl.exe /Zi /O2 /W4 /WX` + `inf2cat` + `signtool sign` via Azure Trusted Signing
2. **WHQL-Submission**: Hardware Dev Center → Driver-Package hochladen → HLK-Report mit anhängen
3. **Publish**: *Automatic* (Windows Update) oder *Manual* (MSI). Für neue Hardware-IDs: *Shared* nach 30 Tagen Telemetrie.
4. **Update-Kadenz**: Semver. Treiber-Build-Nummer ist orthogonal zur Protokoll-Version.
5. **Rollback**: WU kennt Driver-Rollback per Device-Manager. MSI-Fallback muss idempotent sein.

### 2.7 Beobachtbarkeit

- **ETW-Provider** `{rpbridge-GUID}` mit Events: `Enumerate`, `Open`, `Close`, `RpbpRxFrame`, `RpbpTxFrame`, `CrcError`, `OverrunDropped`, `SuspendEntry`, `ResumeEntry`
- `wpr /start rpbridge.wprp` → `wpr /stop rpbridge.etl` → Perfetto-kompatible Trace via `etl2ndjson`
- Dump-Extension `rpbridge.dll` für WinDbg: `!rpbridge.queues`, `!rpbridge.channels`

---

## 3  Linux-Treiber — Referenz-Stack

### 3.1 Architektur

```
 User-App (Python, Rust, C)
        │
        ▼  libusb? NO — /dev/rpbridgeN-*, /dev/ttyRPB*, can0, /dev/i2c-X, /dev/spidevX.Y, /dev/gpiochipN, /dev/iio:deviceN
        ▼
 ┌────────────────────────────────────────────────────────────────┐
 │  rpbridge_usb.ko  (USB class driver, registriert VID/PID)      │
 │  • URB-Queue (Bulk-IN/OUT), RPBP Demux, CRC-Check              │
 │  • Expose als auxiliary_device für jede Sub-Function:          │
 │     - rpbridge-uart.N    → serdev / tty core                   │
 │     - rpbridge-can.N     → can netdev (mainline-kompatibel)    │
 │     - rpbridge-i2c.N     → i2c_adapter                         │
 │     - rpbridge-spi.N     → spi_master                          │
 │     - rpbridge-gpio.N    → gpio_chip (cdev v2)                 │
 │     - rpbridge-iio.N     → iio_dev (für ADC)                   │
 └────────────────────────────────────────────────────────────────┘
        │                │               │            │
        ▼                ▼               ▼            ▼
 rpbridge_uart.ko  rpbridge_can.ko  rpbridge_i2c.ko ...  (jeweils ein .ko pro Subsystem)
```

**Warum diese Aufteilung:** `auxiliary_bus` (mainline seit 5.11) ist der moderne Nachfolger der alten **MFD platform_device**-Muster. Jedes Sub-Driver-Modul registriert sich gegen den Typ `rpbridge-<class>` und bekommt einen stabilen Kontext — kein `struct mfd_cell`-Geflicht, kein sysfs-ID-Chaos.

Jedes Sub-Modul lebt dabei in **seinem jeweiligen Subsystem** (`drivers/net/can/usb/`, `drivers/tty/serial/`, …) — das ist der Weg, der im Upstream akzeptiert wird.

### 3.2 Userland-API (das Beste am Linux-Weg)

- **UART** → `/dev/ttyRPB0..2` via **serdev / tty core** → jede Software die termios versteht (minicom, socat, pyserial) funktioniert ohne Modifikation
- **CAN** → `can0` via **SocketCAN** → `candump`, `cansend`, `cansequence`, `kvaser_usb`-Ökosystem
- **I²C** → `/dev/i2c-N` via i2c-dev → `i2cdetect`, `i2ctransfer`, libi2c
- **SPI** → `/dev/spidevN.M` via spidev — **Hinweis**: spidev ist als Userspace-Interface leicht verpönt im Upstream; wir ziehen `/dev/rpbridge-spiN` als *char-device mit eigener IOCTL-Oberfläche* vor (siehe §3.5) und bieten spidev-Kompat optional
- **GPIO** → `/dev/gpiochipN` via **GPIO cdev ABI v2** (seit 5.10) → **libgpiod 2.x** (`gpioget`, `gpioset`, `gpiomon`). `sys/class/gpio` ist **deprecated**, wird **nicht** bedient.
- **ADC** → `/dev/iio:deviceN` via **IIO** → `iio_info`, `iio_readdev`

### 3.3 Kernel-Version-Baseline

Minimum: **Linux 6.6 LTS** (2026-relevant, bis 12/2026 / 12/2028 mit CIP). Wir nutzen:
- `auxiliary_bus` (≥ 5.11) ✓
- `gpio_chip` cdev v2 (≥ 5.10) ✓
- `usb_fill_*_urb`-moderne Signaturen ✓
- `device_driver.dev_groups` statt `DRIVER_ATTR` ✓
- `devm_*` für alles was geht ✓
- `guard()`/`__free()` *cleanup class* (≥ 6.5) ✓ — wir nutzen sie
- **Rust-for-Linux** (≥ 6.7 mit USB) — Migration ab Phase 3 (§8)

### 3.4 Out-of-Tree vs. Upstream

**Ziel:** Upstream mainline. **Weg dorthin** in zwei Phasen:

| Phase | Distribution | Build | Zielgruppe |
|---|---|---|---|
| 0–12 Mon | **DKMS-Paket** (`rpbridge-dkms_1.0.0_all.deb` + Arch AUR + RPM) | `dkms install` baut gegen User-Kernel | Early Adopters |
| 12–24 Mon | **linux-usb** & Sub-Maintainer-Patches, iterativ Upstream | Mainline | Alle |
| ab 24 Mon | In-tree. DKMS-Paket wird zu *backport* für ältere LTS | Distro-Kernel | Default |

Für den Upstream-Weg: *immer* im Mainline-Stil entwickeln, nicht im „DKMS-first"-Stil. `git format-patch`-kompatibel, `scripts/checkpatch.pl` sauber, `lib/dim`, `devres`, korrekte `MODULE_*` Makros, Coccinelle-Durchläufe bestanden.

### 3.5 Char-Device vs. Subsystem-Integration

Für *jede* Sub-Funktion prüfen:

| Funktion | Primärer Weg | Warum |
|---|---|---|
| UART | **tty core** | Drop-in für Userland |
| CAN | **can netdev** | SocketCAN-Ökosystem |
| I²C | **i2c-dev** | Drop-in |
| GPIO | **gpiolib + cdev v2** | libgpiod |
| ADC | **IIO** | Drop-in für `iio_readdev` |
| SPI | **spi-master + optional char-dev** | spi-master ideal; nur falls User-Mode-SPI nötig: char-dev |
| **RPBP control** | **eigener char-dev** `/dev/rpbridge0` | Firmware-Update, Capability-Query, Tracing — ohne Subsystem-Zwang |

### 3.6 Signierung & Integrität

- **Modul-Signatur**: Distros mit `CONFIG_MODULE_SIG=y` erwarten signierte Module. Unser DKMS-Build bietet `--sign` mit user-provided Key; für Secure-Boot-Systeme: MOK-Enrollment-Anleitung.
- **Firmware-Blobs** (für Sub-Firmware-Updates live): unter `/lib/firmware/rpbridge/<ver>/*.elf`, signiert mit **minisign**, Treiber verifiziert Signatur **vor** Upload zum Gerät.
- **udev**: `/usr/lib/udev/rules.d/70-rpbridge.rules` setzt `MODE=0660 GROUP=plugdev` + stabile Symlinks `ttyRPB-<serial>`.

### 3.7 Rust-for-Linux-Migrationspfad

Für Linux 6.7+ mit USB-Rust-Abstraktionen:

```rust
// rpbridge_usb.rs (gekürzt, zukünftig)
use kernel::{prelude::*, usb};

struct RpbridgeDriver;

#[vtable]
impl usb::Driver for RpbridgeDriver {
    type Data = Arc<RpbridgeDevice>;

    fn probe(intf: &usb::Interface, _id: &usb::DeviceId) -> Result<Self::Data> {
        let dev = RpbridgeDevice::new(intf)?;
        dev.negotiate_capabilities()?;
        dev.register_aux_devices()?;
        Ok(dev)
    }
}

module_usb_driver! {
    type: RpbridgeDriver,
    name: "rpbridge_usb",
    authors: ["Karl Bauer"],
    license: "GPL v2",
}
```

Plan: den Transport-Layer (Bulk-Queue + RPBP Demux + CRC) als erstes in Rust umziehen — isoliert, ohne Subsystem-Abhängigkeiten. Sub-Module bleiben vorerst C.

### 3.8 Beobachtbarkeit

- **Tracepoints**: `rpbridge:rpbp_rx`, `rpbridge:rpbp_tx`, `rpbridge:crc_error`, `rpbridge:credit_stall` → über `trace-cmd`, `perf`, `bpftrace` greifbar
- **Debugfs**: `/sys/kernel/debug/rpbridge/N/{stats,channels,credits,caps_cbor}` (nur Debug-Builds)
- **ethtool-ähnliche** Stats über `devlink` für CAN-Sub-Device

---

## 4  USB-Kommunikationsprotokoll — RPBP v1

> RPBP = **R**P**B**ridge **P**rotocol. Bulk-IN/Bulk-OUT basiert, Rahmen-Framing,
> Channel-Multiplex, CRC-32C, CBOR-erweiterbar, Credit-Flow-Control, semantisch versioniert.

### 4.1 Transport

Zwei Bulk-Endpoints auf dem Vendor-Interface:
- **BulkOUT** (Host → Device) — wMaxPacketSize 64 (FS)
- **BulkIN**  (Device → Host) — wMaxPacketSize 64 (FS)

Optional zusätzlich **BulkIN (Events)** getrennt, falls Event-Stream nicht neben Bulk-Traffic stecken soll — in v1.0 nicht verpflichtend.

### 4.2 Rahmen-Format

```
 0        1        2        3        4        5        6        7
 ┌────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
 │ MAGIC  │  VER   │ MTYPE  │ FLAGS  │   CHANNEL (LE)  │   SEQ (LE)      │
 ├────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┤
 │                    PAYLOAD_LEN (LE, uint32)                           │
 ├───────────────────────────────────────────────────────────────────────┤
 │                    TIMESTAMP_US (LE, uint32, device-boot)             │
 ├───────────────────────────────────────────────────────────────────────┤
 │                    PAYLOAD  (0..65535 Bytes, MTU-abhängig)            │
 ├───────────────────────────────────────────────────────────────────────┤
 │                    CRC-32C (LE, über Header+Payload, init=0xFFFFFFFF) │
 └───────────────────────────────────────────────────────────────────────┘

 MAGIC = 0x52 ('R')
 VER   = 0x01  (major=1; minor via CAPABILITIES verhandelt)
```

**Header = fix 16 Bytes.** Trailer = 4 Bytes CRC-32C. Ein Rahmen endet wenn `PAYLOAD_LEN` Bytes konsumiert sind + 4 Bytes CRC gelesen wurden; Bulk-Paket-Grenzen sind **irrelevant** (Streaming-fähig, muss keinen Short-Packet-Terminator benutzen — aber sendet optional ZLP nach Rahmen-Ende für Low-Latency-Flush).

### 4.3 Message Types

| Wert | Name | Richtung | Zweck |
|---:|---|---|---|
| 0x00 | `HELLO` | Host→Dev, Dev→Host | Session-Init, Protokoll-Version, Nonce |
| 0x01 | `CAPABILITIES` | Dev→Host | CBOR-Payload mit Fähigkeiten |
| 0x02 | `CMD_REQUEST` | Host→Dev | Kontroll-Kommando (CBOR) |
| 0x03 | `CMD_RESPONSE` | Dev→Host | Ergebnis (CBOR) |
| 0x04 | `STREAM_DATA` | beide | Roher Kanal-Datenstrom (UART-Bytes, CAN-Frames, …) |
| 0x05 | `STREAM_CREDIT` | Host→Dev | Credit-Grant pro Kanal |
| 0x06 | `EVENT` | Dev→Host | Async Event (GPIO-IRQ, CAN-Bus-Fehler, Over-Current) |
| 0x07 | `PING` | Host→Dev | Keepalive; Dev antwortet mit `PONG` |
| 0x08 | `PONG` | Dev→Host | Keepalive-Antwort |
| 0x09 | `ERROR` | Dev→Host | Nackend für vorausgegangenen Frame mit SEQ-Bezug |
| 0x0A | `RESET_CHANNEL` | Host→Dev | Kanal flushen/reinit |
| 0x0B | `TIME_SYNC` | Host→Dev, Dev→Host | Host-↔-Dev Timestamp-Abgleich |
| 0x10–0x7F | reserviert | | Future minor-Extensions |
| 0x80–0xFF | reserviert | | Vendor-Experiment (nie in Release) |

### 4.4 Flags

| Bit | Name | Bedeutung |
|---|---|---|
| 0 | `CBOR` | Payload ist CBOR-kodiert |
| 1 | `COMPRESSED` | Payload LZ4-komprimiert (Minor-Feature, nur wenn Cap signalisiert) |
| 2 | `URGENT` | Out-of-band-Kanal; überholt normale Queue (nur Kontroll-Kanal) |
| 3 | `FRAGMENT` | Rahmen ist Teil einer Sequenz (nicht das letzte Fragment) |
| 4 | `LAST` | Letztes Fragment einer Sequenz |
| 5 | `CONTINUATION` | Mittleres Fragment (weder erstes noch letztes) — optional |
| 6–7 | reserviert | *muss* 0 sein im v1.0 |

Fragmentierung: Rahmen > MTU werden in N Fragmente geteilt; jedes Fragment trägt eigenen Header, gleiche `CHANNEL`, monoton steigende `SEQ`. `FRAGMENT`-Bit im ersten+mittleren, `LAST`-Bit im letzten.

### 4.5 Channels

| Range | Zweck |
|---|---|
| 0 | **Control** — HELLO, CAPABILITIES, CMD_*, TIME_SYNC, RESET_CHANNEL |
| 1 | **Events** — asynchrone Dev→Host Benachrichtigungen |
| 2–15 | reserviert (statisch zugewiesen in späteren Versionen) |
| 16–239 | **Dynamic Streams** — per `OPEN_CHANNEL` Kommando zugewiesen, an logische Funktionen gebunden (uart0, can0, i2c0, …) |
| 240–254 | **Vendor** — erlaubt für Forks/Prototypen, nie in Upstream-Firmware |
| 255 (0xFF) | **Broadcast** (Dev→Host) — selten, z. B. für globale Bus-Fehler |

Die statische Zuweisung *ab 2.0* kann sich ändern; aber: *„Control=0, Events=1"* ist **für alle Zeit** fix. Das ist der einzige ABI-Teil, der niemals brechen darf.

### 4.6 Versionierung & Feature-Flags

**Protokoll-Version** = `MAJOR.MINOR.PATCH`. In `HELLO`:

```cbor
{
  "proto": [1, 0, 0],        // MAJOR MINOR PATCH des *Protokolls*
  "fw":    "1.0.0+abc123",   // Firmware-Version (semver + git-hash)
  "build": 42,                // monotoner build counter
  "board": "rpbridge-rev-B",
  "serial": "h'1122334455667788'",
  "nonce":  "h'...'",         // 16 Byte, für Attestation
}
```

**Feature-Flags** kommen **nicht** in den Header (zu viel Platz), sondern in der `CAPABILITIES`-CBOR-Map. Der Kernel-Treiber hält ein Spiegel-Bitmap für Hot-Path-Entscheidungen (IOCTL `GET_CAPABILITIES` → User-Lib; auf IO-Pfad nur „kanal X erlaubt Op Y?" Bit-Checks).

**Kompatibilitätsregeln:**
- **MAJOR↑** = ABI-Bruch. Host weigert sich, auf altem MAJOR zu laufen (Fehler statt Fehlverhalten).
- **MINOR↑** = additiv. Host mit MINOR_host < MINOR_dev nutzt nur was er kennt; neue Events/Kanäle ignoriert er.
- **PATCH↑** = Klarstellung/Bugfix. Immer kompatibel.

### 4.7 Credit-basierte Flow-Control

**Warum nicht NAK?** USB-FS kennt kein aussagekräftiges Flow-Control-Signal; eine überlastete Host-App darf CAN-RX nicht stehen-bleiben lassen, wenn sie UART nicht bedient.

**Mechanik:**
1. Host sendet `STREAM_CREDIT(channel=C, credits=N)` = „ich habe Platz für N Bytes von Kanal C".
2. Device sendet max. N Bytes in `STREAM_DATA(C, …)`-Rahmen; zieht dabei Credits herunter.
3. Credits=0 → Device stoppt Sendung auf Kanal C, sendet ggf. `EVENT(cause=FLOW_BLOCKED)`.
4. Host verteilt Credits je nach Kanal-Priorität (Policy im User-Space, Default: Round-Robin mit Min-Guarantees).

Credits werden **pro Kanal** gehalten. Initial-Credits kommen aus `CAPABILITIES.max_rx_inflight[channel]`.

### 4.8 Fehler-Codes (ERROR Payload)

POSIX-errno-inspiriert als stabile Integers:

| Code | Name | Bedeutung |
|---:|---|---|
| 0  | `OK` | — |
| 1  | `EAGAIN` | Bitte später erneut versuchen |
| 2  | `EINVAL` | Ungültige Parameter |
| 3  | `EPERM` | Op nicht erlaubt (Kanal nicht offen) |
| 4  | `ENOENT` | Unbekannter Kanal/Feature |
| 5  | `EIO` | HW-Fehler (I²C-NACK, CAN-Bus-Off, …) |
| 6  | `ETIMEDOUT` | Kommando-Timeout im Device |
| 7  | `EMSGSIZE` | Payload zu groß |
| 8  | `ECRC` | CRC der letzten RX-Frame fehlerhaft (Info, nicht unsere CRC) |
| 9  | `EPROTO` | Protokoll-Verletzung (soll nie passieren) |
| 10 | `ENOTSUP` | Feature nicht implementiert |
| 11 | `EBUSY` | Kanal belegt |
| 12 | `ECANCELED` | Host hat Vorgang abgebrochen |
| 13 | `ENOSPC` | FIFO voll |
| 14 | `EOVERFLOW` | Credit-Unterlauf (sollte nie passieren → Protokoll-Bug-Indikator) |

### 4.9 Capability-Discovery (CBOR)

```cbor
{
  "proto": [1, 0, 0],
  "mtu":   { "out": 512, "in": 512 },
  "fw":    "1.0.0+abc123",
  "board": "rpbridge-rev-B",
  "features": [
    "uart.rs232", "uart.rs485", "uart.ttl",
    "can.classic", "can.fd",
    "i2c.100k", "i2c.400k", "i2c.1m",
    "spi.mode0..3", "spi.slave",
    "gpio.input", "gpio.output", "gpio.pwm", "gpio.irq",
    "adc.12bit",
    "crc32c.hw", "cbor", "credit-fc",
    "time-sync", "events",
    "secure-boot", "firmware-update"
  ],
  "channels": {
    "16": { "class": "uart", "index": 0, "caps": { "baudmin": 300, "baudmax": 3000000 } },
    "17": { "class": "uart", "index": 1, "caps": { "baudmin": 300, "baudmax": 921600 } },
    "18": { "class": "uart", "index": 2, "caps": { "baudmin": 300, "baudmax": 3000000 } },
    "32": { "class": "can",  "index": 0, "caps": { "fd": true, "brs": true } },
    "48": { "class": "i2c",  "index": 0, "caps": { "speeds": [100000, 400000, 1000000] } },
    "64": { "class": "spi",  "index": 0, "caps": { "modes": [0,1,2,3], "maxfreq": 31250000 } },
    "80": { "class": "gpio", "pins": 16, "caps": { "irq": true, "pwm": true } },
    "96": { "class": "adc",  "channels": 4, "caps": { "bits": 12, "maxrate": 500000 } }
  },
  "max_rx_inflight": { "16": 8192, "17": 8192, "18": 8192, "32": 65535, "48": 4096, "64": 16384, "80": 256, "96": 8192 },
  "identity": {
    "serial": "h'1122334455667788'",
    "mfg":    "BAUER GROUP",
    "pcb":    "rpbridge-rev-B",
    "otp":    { "secure_boot": true, "locked": true }
  }
}
```

Der **Wildcard-Ansatz** (unbekannte Feature-Strings werden ignoriert) macht die Fähigkeits-Erweiterung rückwärtskompatibel — ein alter Host sieht einfach neue Strings nicht.

### 4.10 Zeit-Synchronisation

`TIME_SYNC` implementiert **PTP-light**:
1. Host sendet `TIME_SYNC(type=REQ, t1=host_send_us)`.
2. Device antwortet `TIME_SYNC(type=RESP, t1, t2=dev_rx_us, t3=dev_send_us)`.
3. Host liest `t4=host_rx_us`, berechnet Offset + Delay wie bei NTP/PTP.
4. Alle Device-Timestamps (Header) sind relativ zu Device-Boot; Host mappt via Offset → Host-Uhr.

Synchronisationsgüte-Ziel: ±10 µs im Idle, ±100 µs unter Bulk-Last.

### 4.11 Sicherheit im Protokoll

| Bedrohung | Gegenmittel |
|---|---|
| Bit-Fehler auf USB-Strecke | CRC-32C *zusätzlich* zu USB-CRC (tiefere Layer-Isolation) |
| Replay | SEQ pro Kanal; Gerät hält SEQ-Fenster, verwirft Duplikate |
| Malicious Firmware ersetzt | RP2350 Secure Boot + minisign-Firmware-Signatur, `ATTEST` in CAPABILITIES liefert SHA256 des laufenden Firmware-Images |
| Device-Impersonation | Seriennummer aus OTP, Public-Key in OTP-Area, `ATTEST`-Command signiert Host-Nonce mit Device-Key |
| Downgrade-Attack (neue Host-Lib lässt sich auf alte Firmware locken) | Host verifiziert Firmware-Version gegen Policy; „minimum required firmware version" in Lib-Config |
| I²C-/SPI-Flash dump via Gerät | Kein direktes Flash-Lesen im Protokoll; nur aktive Operationen |
| Over-Current / Kurzschluss (HW) | PTC-Fuse + Software-Watchdog + `EVENT(OVER_CURRENT)` |

---

## 5  Userland-Library-Matrix

Eine **Kern-DLL/.so** (`rpbridge`, C17-ABI), Sprach-Bindings generieren gegen einen C-Header + CBOR-Schema.

| Sprache | Binding | Notes |
|---|---|---|
| **C/C++** | direkter Header | Referenz |
| **Python** | `rpbridge` via `cffi` | wheels für Windows/macOS/Linux x64+arm64 |
| **Rust** | `rpbridge-rs` | idiomatisch, `Result`-basierend, optional async (tokio) |
| **.NET 8+** | `Bauer.Rpbridge` | NuGet, nur modernes .NET 8+, keine .NET Framework 4.x-Unterstützung |
| **Go** | `go-rpbridge` | cgo-Binding |
| **Node.js** | `@bauer-group/rpbridge` | N-API, nicht nan.h |

Alle Bindings sind gegen **denselben** CBOR-Capability-Reader programmiert → Feature-Parität per Konstruktion.

---

## 6  Versionierungs-Strategie (Gesamtsystem)

```
          ┌─────────────────────┐
          │  Release "rpbridge" │
          │        v2.3.1       │
          └──────────┬──────────┘
                     │
    ┌────────────────┼────────────────┬────────────────┬────────────────┐
    ▼                ▼                ▼                ▼                ▼
 Protocol v1.2  FW 1.5.3        Windows drv 2.3  Linux drv 2.3    User libs 2.3
 (semver)       (semver+hash)   (semver+build)   (semver+build)   (semver)
```

**Regel:** Release-Tag ist ein *Meta-Tag* der die konkreten Versionen aller Komponenten bündelt. Jede Komponente versioniert **unabhängig** semantisch. Kompatibilität wird **zur Laufzeit** geprüft (Capability-Negotiation), nicht per statischer Matrix.

**Mindest-Kompatibilitätszusage:**
- Firmware N.x bleibt *immer* kompatibel mit Treiber (N−1).x und (N+1).x, solange MAJOR identisch.
- Treiber-Release liefert *mindestens* 3 Jahre Bugfixes (Windows/Linux gleichgetaktet).

---

## 7  Erweiterbarkeit — wie neue Features hinzugefügt werden

Beispielhaft: „I²S-Audio als neuer Sub-Bus":

1. **Firmware**: neuer Channel-Class `"i2s"`, neue Channel-ID (z. B. 112), Initial via neuem `OPEN_CHANNEL{class:"i2s"}`.
2. **CAPABILITIES**: neue Feature-Strings `"i2s.16k"`, `"i2s.44k1"`, `"i2s.48k"` + Channel-Definition.
3. **Protokoll-Minor-Bump**: 1.0 → 1.1 (additiv, Host versteht oder ignoriert).
4. **Windows-Treiber**: neues Sub-FDO `rpbridge-i2s0.sys`, neuer `IOCTL_RPBRIDGE_STREAM_IOCTL` Tag `I2S_CONFIGURE`. Existierende IOCTLs ändern sich **nicht**.
5. **Linux-Treiber**: neues `rpbridge_i2s.ko`-Modul bindet auf `auxiliary_device` Typ `"rpbridge-i2s"`, integriert in ALSA.
6. **Libs**: `rpbridge_i2s_open()`, `rpbridge_i2s_write()`. Alte Libs kennen die Symbole nicht und rufen sie nicht auf → kein Bruch.
7. **Release-Tag**: `rpbridge-v2.1.0` mit `proto=1.1, fw=1.6.0, win-drv=2.1.0, linux-drv=2.1.0, libs=2.1.0`.

**Was nie geht:** Bedeutung existierender Felder, Message-Types, Channel-IDs ändern. Struktur-Layouts in IOCTLs umbauen. `OK=0` zu `OK=1` ändern. Solche Brüche brauchen MAJOR-Bump + Parallel-Support-Zeitraum.

---

## 8  Roadmap

| Phase | Wochen | Deliverables |
|---:|---|---|
| P0 | 1–4 | Hardware V2 (Rev-A-Prototyp), FW-Skeleton (TinyUSB, RPBP v1 Framing, HELLO+CAPABILITIES), **beide** Treiber nur als Echo-Test |
| P1 | 5–12 | Linux-Treiber: alle 6 Subsysteme funktional, DKMS-Paket, udev, libgpiod-Tests grün |
| P2 | 13–20 | Windows-Treiber: KMDF-Skeleton, Enumeration, Bulk-Echo, DCH-INF, Azure Trusted Signing, erster HLK-Lauf |
| P3 | 21–28 | Windows-Treiber: alle Kanäle, Selective Suspend, WPP, WHQL-Submission, Publish to WU |
| P4 | 29–36 | User-Libs C/Python/Rust/.NET 8+/Go/Node, Beispiele, `rpbridge-cli`, Doku, Conformance-Test-Suite |
| P5 | 37–48 | Upstream-Patches Linux (linux-usb + Sub-Maintainers), Rust-Port des Transport-Layers, 1. Minor-Release (1.1) mit neuem Feature als Lackmustest der Erweiterbarkeit |

---

## 9  Conformance & Tests

- **Protokoll-Golden-Vectors**: Referenzkorpus (~200 Frames mit bekannten CRC + CBOR) — Hostlibs und FW müssen 100 % verarbeiten. Veröffentlicht als Teil des Repos.
- **Fuzzing**: `cargo-fuzz` auf `rpbridge-rs`-Parser, `honggfuzz` auf C-Kernel-Pfad-Simulator, `syzkaller`-Harness für Linux-Treiber.
- **Interop-Matrix**: jede Release-PR baut Host-Lib v(N−1) gegen FW v(N) *und* Host-Lib v(N) gegen FW v(N−1) → Rückwärts-/Vorwärts-Kompat-Verifikation automatisiert.
- **Power-Test**: 72-h Soak bei 80 % Bulk-Saturation auf UART+CAN parallel ohne RX-Verluste.
- **WHQL HLK**: siehe §2.5.
- **kselftest / kunit** für Linux-Module.

---

## 10  Bezüge & ersetzte Entscheidungen

| Dokument | Abschnitt | Was dort stand | Was jetzt gilt |
|---|---|---|---|
| `rpbridge-windows-support.md` | §2 „Composite Device mit CDC-ACM + WinUSB" | *Kein eigener Treiber nötig* | **Eigener DCH/KMDF-Treiber = Primärpfad.** CDC-ACM-Variante bleibt als *Fallback-Compat-Mode*, per Capability `"compat.cdc-acm"` zuschaltbar. |
| `rpbridge-windows-support.md` | §5 „gs_usb" | *Nutze mainline gs_usb* | **Eigener Transport.** gs_usb-Kompat nur als alternativer Descriptor-Set, wenn User `compat-mode` aktiviert. |
| `rpbridge-protocol-and-modern-stack.md` | §3 | *RPBP v1 Draft* | dieses Dokument ist die **Referenzspezifikation** — bei Konflikt gewinnt dieses. |
| `rpbridge-protocol-and-modern-stack.md` | §4 | *Linux auxiliary_bus* | **Bleibt gültig**, präzisiert in §3.1 dieses Dokuments. |

---

## 11  Referenzen

- Microsoft: *Getting Started with Universal Windows drivers* / *Write a Universal Windows driver using KMDF*
- Microsoft: *MS OS 2.0 Descriptors Specification v1.0*
- Microsoft: *Azure Trusted Signing for kernel-mode drivers*
- Microsoft: *Windows Hardware Lab Kit (HLK)*
- Linux kernel: `Documentation/driver-api/auxiliary_bus.rst`, `Documentation/driver-api/gpio/*.rst`
- Linux kernel: `Documentation/rust/`
- Greg KH: *The Linux Kernel Driver Interface — stable API nonsense* (Kontext für Out-of-Tree → Upstream-Migration)
- IETF RFC 8949: *Concise Binary Object Representation (CBOR)*
- IETF RFC 8610: *CDDL — Concise Data Definition Language* (für Capability-Schema)
- Castagnoli et al. 1993: *Optimization of Cyclic Redundancy-Check Codes* (CRC-32C-Polynom 0x1EDC6F41)
- USB-IF: *USB 2.0 Specification*, *USB Base Class / Vendor Specific Class*

---

*Änderungen an diesem Dokument erfordern einen ADR (Architecture Decision Record) und MAJOR-Bump, falls sie die Protokoll-Kompatibilität betreffen.*
