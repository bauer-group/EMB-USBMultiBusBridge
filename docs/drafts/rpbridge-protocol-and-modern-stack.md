# RPBridge – Modernes Protokoll- & Treiberstack-Konzept

**Dokumentversion:** 1.0
**Datum:** 2026-04-22
**Scope:** Autoritatives Dokument für (a) das USB-Kommunikationsprotokoll der RPBridge und (b) die Auswahl moderner, sicherer, performanter und stabiler Treiberkonzepte für Linux und Windows. Ergänzt und präzisiert die Entscheidungen aus `rpbridge-architektur.md`, `rpbridge-windows-support.md` und `rpbridge-cicd.md`. Wo dieses Dokument von den vorherigen abweicht, **gilt dieses Dokument**.

Leitlinie: **keine Legacy-Pfade, keine „funktioniert irgendwie auch"-Workarounds**. Jede Entscheidung ist auf dem aktuellen Stand der Kernel-, Windows- und USB-Praxis (Stand Q2 2026) belegt und gegen ausgereifte Alternativen abgewogen.

---

## 1. Designprinzipien

1. **Asynchron und pipelined.** Kein Request/Response-Lockstep. Viele Transaktionen und Streams laufen gleichzeitig über einen gemeinsamen Transportkanal. Sub-Millisekunden-Latenz für kurze Ops (I²C-Byte, GPIO-Toggle), gleichzeitig volle Bus-Bandbreite für CAN-Dumps und Logic-Streams.
2. **Effizient am Medium.** USB 2.0 Full Speed liefert netto ~1,1 MB/s bulk. Das Protokoll darf keine unnötigen Bytes pro Frame verschwenden. Overhead-Ziel: ≤ 8 B Header für Stream-Frames, ≤ 12 B für Request/Response.
3. **Selbstbeschreibend & versioniert.** Das Gerät meldet seine Capabilities vollständig beim Bind. Clients müssen nie Firmware-Version-Tabellen pflegen. Feature-Flags statt Version-Vergleiche.
4. **Integritätsgeschützt auf Applikationsebene.** USB hat zwar CRC-16 pro Paket, aber wir fügen **pro Nachricht eine CRC-32C** hinzu. Das fängt Firmware-Bugs, ESD-Transienten an User-Headern (die als kausale Kette am MCU-Kern ankommen) und rare Bit-Flips im SRAM ab. Kostet 4 B pro Nachricht — irrelevant.
5. **Ereignisse first-class.** Interrupts von GPIO, CAN-Bus-Status, UART-Break, Timeouts sind **unsolicited messages**, nicht Polling. Hostseitig eventbasiert (epoll/IOCP/select), nicht polling.
6. **Sicher.** Signierte Firmware, optional Per-Device-Key für Capability-Attestation, USB-Transport zwar unverschlüsselt (USB kennt kein TLS), aber mit authentifizierter, integritätsgeschützter Firmware-Herkunft.
7. **Keine Legacy-Treiber.** Auf beiden Plattformen ausschließlich aktuelle APIs, keine INF-Hand-Installation, keine platform_device-Verdrahtung für MFD, keine integer-basierten GPIO-Numbers.

---

## 2. Audit der bisherigen Entscheidungen

Ergebnis der Durchsicht der bisherigen Dokumente mit Blick auf „modern vs. überholt":

| Bereich                         | bisher gewählt                          | Bewertung | Aktion                                                           |
|---------------------------------|-----------------------------------------|-----------|------------------------------------------------------------------|
| Linux: MFD-Pattern              | klassisches `mfd_add_devices` + platform_device | **überholt seit 5.11** | Migration auf **`auxiliary_bus`** (siehe §5) |
| Linux: GPIO-Backend             | gpiolib mit integer-numbers             | veraltet (ABI v1)       | **GPIO cdev ABI v2 + libgpiod 2.x** only (§5.3) |
| Linux: CAN-Integration          | gs_usb-Stil SocketCAN netdev            | **modern**              | beibehalten, **CAN-FD-ready** (auch wenn PHY heute classic) |
| Linux: Userspace I²C/SPI        | i2c-dev / spidev                        | **modern**              | beibehalten                                                        |
| Linux: DT-Overlay-Loader        | `of_overlay_fdt_apply()`                | ok, aber ergänzungsbedürftig | Fallback & Policy für systemd-neuere non-DT-Systeme (§5.6) |
| Linux: Rust-Komponenten         | Userspace-Tools in Rust                 | **modern**              | beibehalten, Prüfung Rust-for-Linux-Migration-Pfad im Horizon    |
| Windows: CDC-Installation       | via Windows-10-1709 inbox usbser         | **modern**              | beibehalten                                                        |
| Windows: Vendor-Bindung         | MS OS 2.0 Descriptors → WinUSB          | **modern**              | beibehalten, Descriptor-Set in §4.6 vollständig spezifiziert      |
| Windows: INF + cat-File         | gar nicht ausgeliefert                  | **modern**              | klar so lassen                                                     |
| Windows: Code-Signing           | Authenticode EV-Cert                     | veraltet (2024+)         | **Azure Trusted Signing** (§6)                                    |
| USB-Protokoll generell          | vage TLV-basierte Kommandos             | nicht genug definiert   | **hier komplett neu spezifiziert (§3)**                            |
| CAN-Profil                      | gs_usb (Linux inbox + Windows Tools)    | **modern & etabliert** | beibehalten (§3.9)                                                |
| Firmware-Signierung             | minisign-Release-Artefakte               | gut                     | ergänzt um **in-device Signaturprüfung** bei Boot (§6.4)          |
| USB-Authentication              | nicht adressiert                         | Lücke                   | USB-if-Authentication v1.0 erwogen, §6.5 entscheidet ausdrücklich dagegen |
| Linux-Tests                     | KUnit + umockdev                         | **modern**              | beibehalten                                                        |
| Windows-Tests                   | USB3CV manuell                           | ok                      | erweitert um automatisierbaren Test-Harness (§8.2)                |

Zu-Korrigierende-Liste aus diesem Audit ist die Arbeitsgrundlage für den Rest des Dokuments.

---

## 3. USB-Kommunikationsprotokoll — RPBP v1

**Name**: **RPBP** (RPBridge Protocol), Versionsnummer im Header.

### 3.1 Transport-Layer

Zwei unabhängige Pfade auf dem USB-Composite-Device (siehe `rpbridge-windows-support.md` §2.1):

* **Control-Interface (WinUSB-gebunden)**: zwei Bulk-Endpoints à 64 B (FS-Grenze) für RPBP-Traffic.
* **CAN-Interface (gs_usb-kompatibel)**: bleibt **wortwörtlich gs_usb**, unverändert. Standard, der millionenfach getestet ist und Linux in-tree sowie Windows via candle_api gratis Unterstützung liefert. RPBP deckt CAN **nicht** ab.

Der Rest dieses Kapitels bezieht sich auf den Control-Kanal.

**Bulk-Endpoint-Größe**: 64 B (USB 2.0 FS-Max pro Bulk-Transfer). Der RP2350-USB-Controller unterstützt bis zu 16 aktive Endpoints; wir bleiben bei zwei Bulks für den Control-Kanal, damit sowohl Linux- als auch Windows-Stacks ohne Tricks maximalen Durchsatz liefern.

**Keine HID-Reports.** HID zieht auf Windows und Linux eine dicke Wrapping-Schicht mit fest dimensionierten Reports ein, die unser dynamisches Framing kastrieren würde. Raw-Bulk ist schneller und klarer.

### 3.2 Message-Framing

Eine RPBP-Nachricht kann über mehrere 64-B-USB-Pakete gehen. Der Empfänger sammelt bis zu einem **USB-Short-Packet** (Paket < 64 B, inklusive Zero-Length-Packet), dessen Ende das Framegrenze-Signal ist. Das ist die Standard-USB-Stream-Konvention und wird sowohl von libusb als auch vom Linux-Kernel-USB-Core ohne Gymnastik unterstützt.

Innerhalb einer Nachricht:

```
+--------+--------+--------+--------+--------+--------+--------+--------+
|                        rpbp_header (16 B)                             |
+--------+--------+--------+--------+--------+--------+--------+--------+
|                            payload (0..N B)                           |
+--------+--------+--------+--------+--------+--------+--------+--------+
|                    CRC-32C (Castagnoli, 4 B)                          |
+--------+--------+--------+--------+--------+--------+--------+--------+
```

Maximale Nachrichtengröße: **4096 B** (konfigurierbar in der Capability-Antwort). Größer ist selten nötig; CAN-Streams und UART-Daten fließen über gs_usb bzw. CDC-ACM, nicht RPBP.

### 3.3 Header-Format

16 Bytes, little-endian:

```c
struct rpbp_header {
    uint8_t  magic;          // 0x52 ('R')  — Start-Marker
    uint8_t  version;        // Protokoll-Version, aktuell 0x01
    uint8_t  msg_type;       // siehe Tabelle
    uint8_t  flags;          // siehe Tabelle
    uint16_t channel;        // 0..65535, 0 = control, 1..N = dynamisch
    uint16_t seq;            // monoton steigend pro channel
    uint32_t payload_len;    // Netto-Payload, ohne Header, ohne CRC
    uint32_t timestamp_us;   // µs seit Device-Boot, wrapping
};
```

**msg_type** (8 Bit):

| Wert | Name            | Bedeutung                                                  |
|------|-----------------|-------------------------------------------------------------|
| 0x01 | `CMD_REQUEST`   | Host → Device: synchroner Befehl, Device antwortet mit RESPONSE |
| 0x02 | `CMD_RESPONSE`  | Device → Host: Antwort auf CMD_REQUEST, gleiche `seq` + `channel` |
| 0x03 | `STREAM_OPEN`   | Host → Device: Kanal öffnen (IRQ, ADC-Stream, Logic-Trace) |
| 0x04 | `STREAM_CLOSE`  | Host → Device: Kanal schließen                              |
| 0x05 | `STREAM_DATA`   | Device → Host: Stream-Frame, 0..N pro Kanal                 |
| 0x06 | `STREAM_CREDIT` | Host → Device: Flow-Control-Credit-Grant                    |
| 0x07 | `EVENT`         | Device → Host: unsolicited (Hotplug, Fault, Watchdog)       |
| 0x0F | `PING`          | Round-Trip-Messung, Health-Check                            |
| 0x10 | `ERROR`         | Device → Host: Fehler zu einer vorhergehenden Msg           |

**flags** (8 Bit):

```
bit 0  CBOR         payload ist CBOR-kodiert (default: binär-strukturiert)
bit 1  COMPRESSED   payload ist mit lz4-block komprimiert (nur Streams > 256 B)
bit 2  URGENT       Empfänger soll zuerst bearbeiten (nur EVENT)
bit 3  FRAGMENT     Nachricht ist Fragment einer größeren (für >4 kB)
bit 4  LAST         letztes Fragment
bit 5..7  reserved, MBZ
```

**channel**-Semantik:

* `0`: Control-Channel für CMD_REQUEST/RESPONSE (alle synchronen Ops).
* `1..N`: dynamisch via STREAM_OPEN zugewiesen; maximale Zahl gleichzeitiger Streams ist in der Capability deklariert (Default 16).
* `0xFFFF`: reserviert für Broadcast-EVENTs (z. B. Watchdog, Reboot-Warnung).

**seq**: pro `channel` monoton steigend. Client matched Response an Request über (`channel`, `seq`). Overflow bei 65536 ist kein Problem, weil innerhalb der Ring-Größe bereits aufgeräumt ist.

**timestamp_us**: Device-lokale Zeit, µs-genau, 32-Bit wrapping (ca. 71 min). Der Host gleicht mit PING-Abweichung auf Host-Uhr ab und interpoliert.

### 3.4 CRC-32C

Castagnoli-Polynom `0x1EDC6F41`. Pro Nachricht über **Header + Payload** berechnet. Auf dem RP2350 via `__builtin_cpu_crc32c()` oder per DMA+SNIFFER-Block (RP2350 hat einen dedizierten CRC-Engine im DMA-Controller — quasi kostenfrei).

Warum CRC-32C und nicht CRC-16-CCITT? CRC-32C hat bessere Fehlererkennung für große Payloads, hardwareunterstützt auf x86_64 (`crc32` SSE4.2-Instruktion) und ARMv8 (`CRC32Cx`). Linux-Kernel liefert `libcrc32c.ko`. Null-Aufwand auf Host-Seite.

### 3.5 Payload-Format — binär-strukturiert (default) vs. CBOR (optional)

**Default: binär-strukturiert** — C-struct-layouts mit festen Feldern, little-endian. Grund: ultra-niedrige Parsing-Kosten im MCU (memcpy), kleinster Overhead, deterministische Größe. Beispiel eines `I²C_READ_REQUEST`:

```c
struct rpbp_i2c_read_req {
    uint8_t  bus;          // 0 oder 1
    uint8_t  addr;         // 7-bit slave addr
    uint8_t  flags;        // 10-bit addr, no-stop, etc.
    uint8_t  reserved;
    uint16_t length;       // max 512
    uint16_t timeout_ms;
};
```

Antwort:

```c
struct rpbp_i2c_read_resp {
    uint8_t  status;       // 0 = OK, siehe §3.7
    uint8_t  reserved[3];
    uint16_t length;       // tatsächlich gelesene Bytes
    uint16_t reserved2;
    uint8_t  data[];       // length Bytes
};
```

**Optional: CBOR** (RFC 8949) — wenn `flag.CBOR` gesetzt ist. Genutzt für:

* komplexe Capability-Antworten, die neue Felder in späteren Versionen hinzubekommen,
* Event-Payloads mit optionalen Feldern,
* Userspace-Tools, die lieber strukturiert parsen wollen.

CBOR ist gegenüber JSON **deutlich kompakter und schneller zu parsen**; Libraries (`tinycbor` auf dem MCU, `cbor2` in Python, `CBOR.NET` im .NET-Ökosystem, `serde_cbor`/`ciborium` in Rust) sind ausgereift. CBOR hat **kein Schema-Compiler**-Requirement wie Protobuf/Flatbuffers — das ist uns wichtig, weil wir keine Codegen-Pipeline durch alle Sprachen ziehen wollen.

**Nicht gewählt**: Protobuf (schema-pflege-intensiv, große Runtimes auf dem MCU), JSON (zu groß und zu langsam zu parsen), MsgPack (kompatibel zu CBOR aber weniger standardisiert, keine RFC). **Explizit nicht gewählt**: textuelle AT-Befehle (Latenz, Parsing-Albtraum, extrem legacy — Grund unter anderem für den schlechten Ruf von Billig-USB-UART-Tools).

### 3.6 Capability-Discovery

Erster Request nach Enumeration: `CMD_REQUEST, msg = GET_CAPABILITIES` (mit `flag.CBOR`). Device antwortet mit einer strukturierten Capability-Map:

```
{
  "protocol_version": 1,
  "firmware_version": "1.2.0",
  "firmware_git": "a1b2c3d",
  "firmware_built": "2026-04-22T10:12:00Z",
  "board_id": "rpbridge_rp2354b",
  "board_rev": "2",
  "hw_uid": "e660a45957..." ,          // aus RP2350 flash unique id
  "max_msg_len": 4096,
  "max_streams": 16,
  "features": {
    "compression": ["lz4"],
    "cbor": true,
    "attestation": false
  },
  "buses": {
    "i2c": [
      { "idx": 0, "max_freq": 1000000, "pullup": 2200, "headers": ["qwiic0", "qwiic1", "i2c0_header"] },
      { "idx": 1, "max_freq":  400000, "pullup": 2200, "headers": ["qwiic2", "i2c1_header"] }
    ],
    "spi": [ ... ],
    "gpio": { "count": 14, "pwm_capable": [30,31,37,...], "adc_channels": [26,27,28,29] },
    "uart": [ ... ],
    "one_wire": [{ "idx": 0, "gpio": 32 }]
  },
  "ota": {
    "mechanism": "uf2_bootsel",
    "signature": "minisign-ed25519",
    "pubkey_fingerprint": "RWQ+g7..."
  }
}
```

Jede neue Firmware darf Felder **ergänzen**. Clients ignorieren unbekannte Felder (CBOR-Standard-Semantik). **Feature-Flags statt Versions-Gates** sind Pflicht — das vermeidet den in alten USB-Tools üblichen „Firmware 1.3 hat, 1.4 hat nicht mehr, 1.5 hat wieder"-Fluch.

### 3.7 Statuscodes

Einheitliche 8-Bit-Status-Enum, aus POSIX-errno inspiriert, aber mit bus-spezifischen Codes erweitert:

```
0x00  OK
0x01  EINVAL           ungültige Parameter
0x02  EBUSY            Bus belegt / Semaphore blockiert
0x03  ETIMEDOUT        Operation Timeout
0x04  EIO              Low-Level-IO-Fehler
0x05  ENODEV           keine Antwort vom Slave (I²C NACK, SPI keine CS, 1-Wire kein Presence)
0x06  EOVERFLOW        Empfangspuffer zu klein / Payload zu groß
0x07  ENOTSUP          Feature nicht unterstützt (prüfen via capabilities!)
0x08  EPERM            Operation verboten (z. B. Phy-Wechsel während aktivem Transfer)
0x09  EAGAIN           transienter Fehler, erneut versuchen
0x0A  EPROTO           Protokoll-Verstoß (CRC falsch, Msg-Type unerwartet)
0x10..0x1F  i²c-spezifisch (arbitration loss, clock stretching timeout, ...)
0x20..0x2F  spi-spezifisch
0x30..0x3F  can-spezifisch (wird nur in EVENTs verwendet, da CAN eigenes Interface)
0x40..0x4F  uart-spezifisch (break, framing, parity, overrun)
0xF0..0xFF  vendor-reserved
```

### 3.8 Flow Control (Credit-Based)

Für Stream-Channels: vor STREAM_OPEN spezifiziert der Host ein **Credit-Budget** in Frames. Jeder `STREAM_DATA`-Frame verbraucht 1 Credit. Der Host sendet regelmäßig `STREAM_CREDIT`-Nachrichten mit neuen Credits. Das verhindert, dass das Device den Host mit unbegrenzten Daten überfährt (Problem: klassische Logic-Analyzer-Firmware, die bei langsamen Hosts in den Endpoint-Queue-Stall läuft). Credit-Budgets per Stream sind unabhängig, damit ein IRQ-Stream nicht durch einen saturierten ADC-Stream blockiert wird.

**Architekturmuster**: direkt aus USB NCM / UAS übernommen, **nicht** aus TCP (dort wäre die Ack-Latenz störend für sub-ms-Targets).

### 3.9 CAN — Separates gs_usb-Interface

CAN läuft **nicht** über RPBP, sondern bleibt auf einem dedizierten USB-Vendor-Interface mit **gs_usb**-Protokoll. Gründe:

* Linux-Mainline-Kernel-Treiber `gs_usb` existiert in-tree (`drivers/net/can/usb/gs_usb.c`) — kostenloser SocketCAN-Support.
* Windows: candle_api, python-can gs_usb-Backend, SavvyCAN, Cangaroo, BUSMASTER — sofort kompatibel.
* Frame-Format ist für CAN optimal (inkl. Hardware-Timestamps).
* 8 Jahre Ausreifzeit, Bugfixes, Stack-Traces in vielen Labors bereits gesehen → stabil.

Das ist ein Fall, wo ein **etablierter Standard einem selbstgebauten Protokoll vorzuziehen ist**.

### 3.10 UART — Separate CDC-ACM-Interfaces

UART-Traffic läuft ebenfalls **nicht** über RPBP, sondern über **3× CDC-ACM** (siehe `rpbridge-windows-support.md` §3). Dort ist die Integration mit existierenden Tools (PuTTY, seriellen Terminals, SerialPort-APIs) entscheidend. Phy-Wahl (RS232/RS485/TTL), DE-Timing, Break-Generation etc. läuft über RPBP-Steuer-Befehle, aber der eigentliche Byte-Stream bleibt auf CDC-ACM.

### 3.11 Was RPBP abdeckt

Zusammengefasst: Control, Config, synchrone Transaktionen für I²C, SPI, GPIO, ADC, PWM, 1-Wire, UART-Phy/Config, Event-Streams (GPIO-IRQ, Bus-Fault, Watchdog), Capability-Discovery, OTA-Trigger.

### 3.12 Benchmarks und Latenzziele

| Operation                                | Zielwert  | Gemessen (geplant) |
|------------------------------------------|-----------|-------------------|
| Einfaches I²C-Read (16 B) Round-Trip     | < 800 µs  | —                 |
| GPIO-Set Round-Trip                      | < 400 µs  | —                 |
| GPIO-IRQ Event-to-Host                   | < 1 ms    | —                 |
| SPI 24 MHz kontinuierlich, 1 MB          | > 900 kB/s effektiv | —       |
| CAN RX@1 Mbit/s sustained                | 8 000 Frames/s | — (Limit ist USB-FS, nicht Firmware) |
| CMD_REQUEST queue depth ohne Stall       | 16 gleichzeitig | —            |

Gemessen wird mit `rpbridge-bench` (Userspace-Tool im Repo), CI-Benchmarks in GitHub Actions auf einem Raspberry Pi 5 + Referenzhardware, tracked pro Firmware-Release als Grafana-Board.

---

## 4. Linux-Treiberstack — moderne Umsetzung

### 4.1 Architektur

```
   +--------------------+     +----------------+     +--------------+
   |  rpbridge-core     |     |  rpbridge-i2c  |     | rpbridge-spi |
   |  (USB interface    |     |  (auxiliary    |     | (auxiliary   |
   |   driver)          | <-- |   device)      |     |  device)     |
   +--------------------+     +----------------+     +--------------+
           ^                         ^                      ^
           |                         |                      |
           |   auxiliary_bus         |                      |
           +-------------------------+----------------------+----------- ...
                                     (one aux_device pro Subsystem)
```

**Bus-Wahl**: `auxiliary_bus` (seit Linux 5.11, mainline), **nicht** mehr `platform_bus`. Grund: auxiliary_bus ist explizit für „ein Parent-Driver spawnt Children unterschiedlicher Subsysteme" gedacht; die DT/ACPI-bezogene Komplexität des platform_bus entfällt, und es ist das Pattern, das moderne Mainline-Treiber (mlx5, ice/irdma, nvmem, i3c-client-composites) durchgängig verwenden.

### 4.2 USB-Interface-Driver (`rpbridge-core.ko`)

Kernaufgaben:

* Registriert sich als `struct usb_driver` für den RPBridge-VID/PID, bindet das Control-Vendor-Interface.
* Allokiert einen Pool von **bulk-IN-URBs (async)**, füttert ihn kontinuierlich an den Endpoint-Ring. Kein `usb_bulk_msg()` im Hot-Path (das ist synchron-blockierend und killt die Latenz).
* Parst RPBP-Nachrichten, dispatcht Responses an wartende Requester (per `completion` + per-seq-Wait-Queue), dispatcht EVENTs/STREAM_DATA an registrierte Callbacks.
* Hält die CRC-32C-Verifikation (`crc32c()` aus `<linux/crc32c.h>`, hardware-unterstützt auf x86_64 und ARMv8).
* Exponiert eine schlanke interne API (`rpbridge_send_sync`, `rpbridge_send_async`, `rpbridge_stream_open`) für die aux_children.
* Registriert pro Subsystem ein **`auxiliary_device`** (I²C, SPI, GPIO, IIO/ADC, netdev/CAN-Shim, hwmon, …), aber nur für die in der Capability-Map gemeldeten Subsysteme — Zero-Cost für nicht-bestückte Features.

Modernisierungen gegenüber einem naiven USB-Treiber:

* **Keine eigene Threading-Infrastruktur.** Async URBs haben Completion-Callbacks im softirq-Kontext; dort wird bevorzugt dispatched. Langläufer wandern in `workqueue`.
* **Locking** mit `mutex` für State und `spinlock_t` nur für URB-Completion-Pfad. Kein `BKL`, kein global locking.
* **Robust gegenüber Unplug**: `usb_anchor` für alle ausstehenden URBs, `usb_kill_anchored_urbs()` im `disconnect()` ruft alle Completions auf, die auf `rpbridge_send_sync` warten.
* **Suspend/Resume** sauber: `.suspend` / `.resume` kippen alle aux_devices in einen quieszenten Zustand, persistieren die aktive Phy-Wahl, erholen sich aus Remote-Wakeup ohne State-Neuinit.

### 4.3 GPIO — cdev ABI v2 only

Userspace-Zugriff ausschließlich über die **GPIO character device ABI v2** (seit Kernel 5.10, stabil seit 5.17 für alle Randfälle). In der Praxis heißt das: **libgpiod ≥ 2.0**. Tools wie `gpioset` / `gpioget` / `gpiomon` in ihren v2-Varianten, die `GPIO_V2_LINE_*` ioctls sprechen.

Im Treiber:

* `struct gpio_chip` mit `.get`, `.set`, `.direction_input`, `.direction_output`, `.to_irq` implementiert.
* `.set_config` für Pull-up/down, Drive-Strength, Open-Drain — alle RP2350-PAD-Features sind so über das offizielle GPIO-Generic-Config-Schema konfigurierbar.
* **Interrupts**: RPBP-EVENT → `generic_handle_irq_safe()` im Child-IRQ-Domain, der `gpio_chip` wurde als IRQ-Chip registriert (`girq`-Struktur). Userspace sieht IRQs als `GPIO_V2_LINE_EVENT_RISING_EDGE` / `FALLING_EDGE` — moderne Semantik.
* **Keine sysfs-GPIO-Schnittstelle**. Die ist **seit 4.8 deprecated** und vom Mainline-Release-Horizont. Wir exportieren sie nicht, auch nicht als Compat-Shim.

### 4.4 I²C / SPI

I²C-Adapter als `struct i2c_adapter` mit `I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL` — moderne User-Space-Tools (`i2c-tools 4.x`, `i2cdetect`, `python-smbus2`) funktionieren unverändert.

SPI als `struct spi_controller` (seit 5.1 der gültige Name, `spi_master` ist nur noch Alias). Via `spidev` ergibt das `/dev/spidev0.0` etc. — userspace-kompatibel mit allem (Flash-Programmern, Displays, SD-Card-Tools).

### 4.5 CAN — netdev + SocketCAN

Wir nutzen **nicht** unseren eigenen aux_device-Ansatz für CAN, sondern lassen das **bereits in-tree vorhandene `gs_usb.ko`** vom Kernel direkt binden (das USB-Interface erscheint dem Kernel als gs_usb-Gerät, weil wir das Protokoll 1:1 nachbilden). Ergebnis: `can0` erscheint in `ip link`, `candump`/`cansend`/`python-can`/`libsocketcan`/`kayak` funktionieren sofort.

**CAN-FD-Readiness**: Die Firmware-Seite kann heute noch kein CAN FD (can2040 ist CAN 2.0B), aber das Protokoll-Gerüst auf der gs_usb-Seite meldet den Feature-Flag korrekt als **„fd: false"**. Eine zukünftige Hardware-Revision mit MCP2518FD o. ä. könnte ohne Treiber-Änderung FD-Support melden; der Kernel-Treiber zieht dann automatisch die CAN-FD-Pfade.

### 4.6 ADC / IIO

ADC-Kanäle werden über das **Industrial I/O (IIO)**-Subsystem exportiert: `/sys/bus/iio/devices/iio:deviceX/in_voltage0_raw` etc. Userspace-Tools wie `iio_generic_buffer`, `libiio`, Grafana-IIO-Plugin funktionieren unmodifiziert. Streaming-ADC → IIO-Trigger + `iio_buffer` direkt auf RPBP-Stream-Channel aufgesetzt.

### 4.7 Device Tree / Overlay-Provisioning

Für Rechner mit Device Tree (ARM64 SBCs, RISC-V): optionaler Auto-Load von Overlay-Fragmenten, die die an den Qwiic-Port gesteckten Sensoren als I²C-Clients beschreiben. API: `of_overlay_fdt_apply()`, seit 4.16 stabil, benutzt auch in Mainline für runtime configuration von FPGA Managern.

Für Nicht-DT-Systeme (x86_64-Desktop-Linux ohne DT): Fallback auf **i2c_new_client_device()**-direkt-Instanziierung via udev-Regeln + rpbridge-ctl-Userspace-Tool. Beide Pfade koexistieren sauber.

### 4.8 Sysfs, Netlink, nichts Exotisches

Konfiguration/Statistik: sysfs-Attribute pro Subsystem (read-only `bus_errors`, `rx_frames`, `tx_frames`, …). Ereignis-Push an Userspace: **generic netlink** (für komplexere Events), nicht `/proc/`, nicht `char device polling`. Systemd kann über udev-netlink direkt hooken.

### 4.9 Testing

* **KUnit** in-tree-Tests für Framing, CRC, Ring-Buffer, State-Machine (Kernel 5.5+). CI-Integration siehe `rpbridge-cicd.md` §4.4.
* **umockdev** für USB-Emulation, abdeckt den Happy-Path und 30+ Fehlerszenarien (disconnect mid-transfer, short-packet-stalling, CRC-Bit-Flip, …).
* **Syzkaller**-Setup im Nightly: Fuzzing des USB-Interfaces gegen die driver-side-Verarbeitung — damit finden wir Parser-Bugs, bevor sie in Prod landen.

### 4.10 Rust-for-Linux — Perspektive

Rust-for-Linux ist seit 6.1 upstream und in 6.12 für mehrere Subsysteme praxistauglich (NVMe, PHY, misc-devices). Als Zukunftsoption beobachten wir die Migration der **auxiliary_bus**-Bindings nach Rust; sobald ein stabiles Rust-API für auxiliary_device existiert (Roadmap 6.14–6.16), planen wir einen Rust-Port von `rpbridge-core` als zweites Build-Target (C bleibt primary solange, bis Rust-Variante in allen unterstützten Kernel-Versionen lauffähig ist). Heute: C, idiomatisch, strikt `-Wall -Wextra -Werror` plus Sparse/Smatch/Coccinelle.

---

## 5. Windows-Stack — moderne Umsetzung

### 5.1 Ausschließliche WinUSB-Bindung für Vendor-Interfaces

Kein KMDF-Treiber. Kein UMDF. Keine INF-Datei-Auslieferung. Alle Vendor-Interfaces binden über **MS OS 2.0 Descriptors** automatisch an `winusb.sys`.

Wenn in Zukunft ein Vendor-spezifischer Treiber nötig würde (z. B. für sehr niedrige IRQ-Latenz mit Kernel-Puffern): **UMDF 2.0** als Userspace-Driver-Framework, **nicht** KMDF. UMDF ist seit Windows 10 stable, isolation-sicher, keine WHQL-Attestation nötig für Distribution via Windows Update.

### 5.2 CDC-ACM — moderne Descriptor-Konformität

Wir liefern **USB CDC 1.2**-konforme Deskriptoren, inkl. IADs. Das garantiert:

* Inbox-usbser auf allen Windows 10 ≥ 1709 (Oct 2017) — das sind 99,7 % des installed-base lt. Microsoft-Telemetrie 2025.
* Saubere Interaktion mit modernen Windows-Terminal-Produkten (WT, VS-SerialMonitor, PowerShell 7.4+).
* Keine INF-Datei nötig.

Für Windows 10 < 1709 und Windows 8.1 liefert das Repo einen **generischen INF** im Ordner `windows/inf/` (nur dokumentiert, nicht im Installer-Default) — Legacy-Pfad, nicht empfohlen.

### 5.3 Client-Libraries — moderne Sprach-Bindings

Alle Bindings bauen auf **libusb 1.0 ≥ 1.0.27** (Windows-Backend WinUSB, automatische Enumeration über DeviceInterfaceGUID aus MS-OS-2.0-Descriptor). Begründung:

* Cross-Plattform-kompatibel (Linux/macOS/Windows).
* Ausgereifter als `WinUSB.h`-direkt-Calls.
* Async-IO über `libusb_submit_transfer` mit Completion-Callbacks passt zu unserem asynchronen Protokoll (§3.1).

Ergänzend:

* **Python**: `pyusb` + eigenem high-level `rpbridge`-Wrapper. Installation: `pip install rpbridge` aus PyPI. Keine `pywin32`-Abhängigkeit.
* **.NET 8+**: `LibUsbDotNet` als Transport, high-level `RPBridge`-Package auf nuget.org. Ziel-Framework `net8.0` (nicht .NET Framework 4.x — das ist Legacy).
* **Rust**: `rusb`-Crate, high-level `rpbridge`-Crate auf crates.io.
* **PowerShell 7.4+ Module**: Cmdlets über .NET-Bindings.

**Nicht empfohlen** (Legacy-Pfade, werden im Repo nicht unterstützt):

* VB6-/VBScript-Wrapper.
* WMI-Integration (`Win32_SerialPort` etc. — unzuverlässig über Composite-Devices).
* `System.IO.Ports.SerialPort` in .NET Framework 2.0–3.5.

### 5.4 UX — WinRT und Windows.Devices

Wer aus einer modernen Windows-Desktop-/MAUI-App serielle Ports öffnen will, soll **`Windows.Devices.SerialCommunication`** (WinRT) nutzen — moderner und konkurrenz-sicherer als das alte `System.IO.Ports`. Unser .NET-Wrapper unterstützt beide, stellt den WinRT-Pfad aber als Default.

### 5.5 Testing

Erweitert gegenüber dem USB3CV-Manual-Run aus `rpbridge-windows-support.md` §9:

* **HLK-ähnlicher Smoke-Test** automatisiert in GitHub Actions mit `windows-latest`-Runner: `pnputil`-Enumeration, `usbview`-Descriptor-Dump mit Expected-Regex-Match, serielle-Port-Echo-Loopback-Test via Python-Script.
* **S3/S4-Cycle-Test** in einer VM (Hyper-V-Nested oder physische Testbench).
* **Plugfest**: 2×/Jahr gegen 10 Referenz-Hosts (Dell Latitude, Lenovo ThinkPad, Surface, diverse USB-Hubs) — manueller, aber dokumentierter Prozess.

---

## 6. Sicherheit

### 6.1 Threat-Model (knapp)

* **Malicious USB host**: kann prinzipiell alles auf dem RPBridge tun, was das Protokoll erlaubt. Das ist akzeptiert — das Gerät ist als Entwickler-Werkzeug konzipiert, der Host ist trusted.
* **Malicious device (Supply-Chain-Angriff)**: gefälschte RPBridge-Firmware auf echter Hardware könnte Host-Daten abgreifen. → Firmware-Signaturprüfung beim Boot, Attestation-Optional.
* **Malicious peer on external bus**: CAN-Busangriffe, I²C-Slave-Fehlverhalten. → Isolation ist nicht Teil der Standard-Hardware (siehe `rpbridge-hardware-design.md` §1, Abgrenzung). Wer isoliert arbeiten muss, nutzt optionale Isolator-Daughterboards.
* **Firmware-Update-Man-in-the-Middle**: GitHub-Release-Mirror-Angriff. → minisign-Signatur aller Release-Artefakte, Public-Key im Tool einkompiliert.

### 6.2 Secure Boot / Firmware-Signatur

RP2350 hat **Secure Boot** als Hardware-Feature (OTP-Fuses, SHA-256-Image-Hash, optionaler Ed25519-Bootloader-Signaturtest). Wir nutzen:

* **ROM-Bootloader verifiziert** die Firmware-Signatur gegen den in OTP gebrannten Public-Key-Hash.
* Produktions-Geräte werden mit einem **Board-Public-Key** (Hash) ge-fusted.
* Firmware wird mit dem **Development-Private-Key** (für CI-Builds) oder dem **Production-Private-Key** (für Release-Tags) signiert.
* Der Private-Key liegt in einem **HSM** oder **Azure Key Vault**, wird nie im Repo commited.

Für Community-Self-Builds kann die OTP-Fuse unberührt bleiben — Secure Boot ist optional. Die Release-Builds von rpbridge.org sind immer signiert.

### 6.3 Capability Attestation

Optional: das Device kann eine `GET_ATTESTATION`-Anfrage beantworten mit:

* hardware_uid (RP2350-Flash-Unique-ID),
* firmware_sha256,
* firmware_signature (Ed25519, vom Boot-Zeitpunkt auf dem Device gespeichert),
* nonce vom Host (Replay-Schutz).

Ein Host-Tool kann damit verifizieren, dass tatsächlich eine signierte, unmodifizierte Firmware läuft. Capability-Flag `attestation: true` im Capability-Response.

**Explizit nicht benutzt**: USB-IF Authentication Spec v1.0 (2019). Die ist in der Industrie kaum adoptiert, Microsoft-Unterstützung ist rudimentär, Linux-Treiber-Support existiert praktisch nicht. Der Aufwand steht in keinem Verhältnis zum Nutzen; wir bauen die gleiche Funktion applikationsebene ohne Vendor-Lock-in.

### 6.4 Firmware-Update-Security

* UF2-Datei mit `minisign` (Ed25519) signiert; Signatur-Datei `rpbridge.uf2.minisig` im Release.
* `rpbridge-ctl flash` prüft Signatur lokal vor dem Reboot-to-BOOTSEL.
* Optional kann das Device nach dem Flash eine **Post-Flash-Attestation** liefern (siehe §6.3) — das Tool verifiziert die neu geflashte Firmware authentisch.

### 6.5 Keine Verschlüsselung auf USB-Ebene

Wir verschlüsseln den USB-Transport **nicht**. Gründe:

* USB ist ein Host-lokaler Bus, physikalischer Zugriff erforderlich.
* TLS/DTLS über Bulk-Endpoints ist zwar möglich (siehe WebUSB-Experimente bei Google), aber: kein Standard, keine inbox-Unterstützung, massiver Zusatzaufwand, und die Angriffe, die es verhindern würde, sind mit physischem Zugriff gleichwertig.
* Integrität haben wir über CRC-32C plus USB-Paket-CRC-16 (redundanzähnlich). Authentizität über Firmware-Signatur.

### 6.6 Fuzzing

* Host-seitig: `afl++` gegen den libusb-Client-Parser.
* Device-seitig: Firmware-Parser wird in einem **Cortex-M33-Emulator** (QEMU `mps3-an547`, modifiziert für RP2350-Peripherie) mit Syzkaller-ähnlichem Input gefüttert. Fuzzing-Korpus wächst in Richtung 100% Branch-Coverage des Parser-Codes.

---

## 7. Performance und Stabilität — konkrete Ziele

### 7.1 Performance-Targets

| Metrik                                      | Zielwert            | Messmethode                  |
|---------------------------------------------|---------------------|------------------------------|
| RPBP Round-Trip (min-payload Ping)          | ≤ 500 µs (p50)      | `rpbridge-bench ping`        |
| I²C 16-B-Read @ 400 kHz Round-Trip          | ≤ 800 µs            | integration test             |
| CAN RX-Throughput @ 1 Mbit/s (80 % Bus-Last) | > 6 500 Frames/s    | gs_usb-Benchmark             |
| USB-FS effektive Bulk-Bandbreite            | > 1,1 MB/s          | `usbmon` + counter           |
| Worst-Case Event-to-Host-Latenz (GPIO IRQ)  | < 1 ms (p99)        | GPIO-Loopback + oscilloscope |
| Treiber-Speicherbedarf (Linux, alle Subsys) | < 120 KB resident   | `cat /sys/module/*/coresize` |
| Firmware-RAM-Footprint                      | < 180 KB bei 520 KB SRAM | `arm-none-eabi-size`    |

### 7.2 Stabilitäts-Ziele

* **MTBF** (Mean Time Between Failures): ≥ 30 Tage Dauerbetrieb ohne Reboot, 100 % Bus-Last auf I²C + SPI + CAN gleichzeitig. Validiert durch Nightly-Stress-Test in CI.
* **0 Kernel-Oopses** in `syzkaller`-Fuzzing über 72 h Run.
* **0 Memory-Leaks** unter `kasan`-Build (Linux) und `heap_caps_check`-artig auf dem MCU.
* **Saubere Hotplug-Semantik**: 10 000× USB-Reconnect-Zyklus ohne Leak, ohne Zombie-URB, ohne dangling `struct auxiliary_device`.

### 7.3 Observability

Pro Bus: Counter in sysfs (Linux) und in `GET_STATS`-Response (Windows):

* `tx_bytes`, `rx_bytes`, `errors`, `timeouts`, `bus_faults`.
* Histogram der Round-Trip-Latenzen (ring-buffer der letzten 1024 Werte, abrufbar per `GET_LATENCY_HISTOGRAM`).
* Logs über CDC-ACM-Debug-Port, strukturiertes Format (timestamp, level, subsystem, message) — passt zu `systemd-journald`-Import auf Linux und `ETW`-Analog unter Windows (über ein kleines Helper-Tool).

### 7.4 Diagnose-Tools

* **`rpbridge-ctl diag`**: Self-Test aller Busse, liest Capability, misst Ping-Latenz, Bus-Counter, meldet Fehler im menschenlesbaren Report.
* **`rpbridge-ctl log --follow`**: streamt den Debug-UART-Log in Echtzeit (via CDC-ACM-Port 3).
* **`rpbridge-ctl capture --channel gpio-irq`**: Events mitschneiden, als pcap-next-gen-style-Datei (für spätere Visualisierung in Wireshark mit custom-dissector).

---

## 8. Verifikationsstrategie

### 8.1 Protokoll-Konformität

* **Golden Vectors**: eine Sammlung handgeschriebener RPBP-Nachrichten mit erwarteter CRC, Header-Bytes, Roundtrip-Semantik. Teil des Repos unter `tests/golden/`. Wird sowohl gegen MCU-Firmware (Host-Simulation) als auch gegen Client-Libs ausgeführt.
* **Fuzzing-Korpus-Teilung**: die Golden Vectors dienen als Seed-Korpus für AFL++/Syzkaller.

### 8.2 Interop-Tests

* Linux-Workflow `driver-integration.yml` (neuer CI-Job, wird zu `rpbridge-cicd.md` ergänzt): startet eine echte RPBridge-Firmware in **QEMU mit USB-Passthrough auf einen virtuellen Device-Endpoint**, lädt den Treiber, führt Test-Szenarien aus. Schneller als echte Hardware, deterministisch, läuft in CI.
* Windows-Workflow: `windows-latest`-Runner mit **USB-over-IP** zu einer physischen RPBridge in einem Labor-Gateway — für jede Release-Tag-Build einmal. Nicht in jeder PR (Latenz).

### 8.3 Performance-Regression

Bei jedem Tag: `rpbridge-bench` läuft gegen Referenzhardware, Ergebnis wird als `benchmark.json` dem Release angehängt und in ein Grafana-Dashboard gepusht. Alerts bei > 10 % Regression gegenüber dem 7-Tage-Moving-Average.

---

## 9. Änderungszusammenfassung gegenüber den Vorgänger-Dokumenten

| Dokument                          | Punkt                                 | Änderung                                                                              |
|-----------------------------------|----------------------------------------|---------------------------------------------------------------------------------------|
| `rpbridge-architektur.md` §7       | MFD-Pattern                           | **Migration platform_device → auxiliary_bus** (§4.1 hier)                             |
| `rpbridge-architektur.md` §8       | GPIO-Interface                         | **libgpiod 2.x / cdev v2 only**, sysfs-GPIO ausdrücklich nicht bedient                |
| `rpbridge-architektur.md` §4       | Protokoll-Framing                     | **komplett neu in RPBP v1 formalisiert** (§3)                                         |
| `rpbridge-architektur.md` §6       | CAN-Integration                        | bleibt gs_usb-Protokoll, jetzt explizit als Grund dokumentiert (§3.9)                 |
| `rpbridge-windows-support.md` §4   | Client-Libraries                      | **.NET 8+ only, libusb-1.0 only**, Legacy-Pfade gestrichen (§5.3)                     |
| `rpbridge-windows-support.md` §8   | Code-Signing                           | **Azure Trusted Signing** statt klassischer EV-Zertifikat-Token-Workflow (§5 + §6)    |
| `rpbridge-cicd.md` §4.4             | Kernel-Tests                          | ergänzt um **syzkaller-nightly** + umockdev-Matrix-Erweiterung                         |
| allgemein                          | USB-Protokoll-Security                 | **CRC-32C + minisign-signierte Firmware + optional Device-Attestation** (§6)          |

---

## 10. Umsetzungs-Reihenfolge

Phase 1 (Bring-up, Firmware-Grundgerüst):

1. RPBP-Header, CRC-32C-Routine, Framing-Parser, Ring-Buffer implementieren.
2. `GET_CAPABILITIES` + `PING` als Minimal-Set.
3. CDC-ACM-Interfaces (3×) laufen.
4. gs_usb-Interface läuft (CAN-Echo-Test).

Phase 2 (Linux-Treiber):

5. `rpbridge-core.ko` mit async-URB-Pool + RPBP-Dispatcher.
6. auxiliary_bus + Child-Drivers für I²C, GPIO, SPI, IIO/ADC.
7. KUnit + umockdev-Tests.

Phase 3 (Windows-Libraries):

8. Python- und .NET-Libs mit libusb/LibUsbDotNet.
9. `rpbridge-ctl.exe` in Rust.

Phase 4 (Security + Observability):

10. Firmware-Signing, Secure-Boot-Fuses-Procedure, minisign-Integration im Release-Workflow.
11. Capability-Attestation.
12. Performance-Dashboards.

Phase 5 (Langfristig):

13. Rust-for-Linux-Port von `rpbridge-core` (wenn auxiliary_bus-Rust-API stabil).
14. CAN-FD-Hardware-Variante (neue Revision mit MCP2518FD).
15. USB Hi-Speed via externen PHY (z. B. USB3300) — falls Bandbreite wirklich knapp wird. Aktuell: nein.

---

## 11. Referenzen

* **Linux auxiliary_bus Documentation**: `Documentation/driver-api/auxiliary_bus.rst` (Kernel-Tree), Einführungs-Commit `7de3697e9cbd` (5.11).
* **Linux GPIO cdev v2 ABI**: `Documentation/userspace-api/gpio/chardev_v2.rst`.
* **libgpiod v2 Release Notes**: <https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git>
* **Linux CAN (SocketCAN) Documentation**: `Documentation/networking/can.rst`
* **gs_usb Linux driver**: `drivers/net/can/usb/gs_usb.c`
* **USB CDC 1.2 Specification**: USB-IF (<https://www.usb.org/document-library/class-definitions-communication-devices-12>)
* **Microsoft MS OS 2.0 Descriptors**: <https://learn.microsoft.com/windows-hardware/drivers/usbcon/microsoft-os-2-0-descriptors-specification>
* **Microsoft Azure Trusted Signing**: <https://learn.microsoft.com/azure/trusted-signing/>
* **RFC 8949 CBOR**: <https://datatracker.ietf.org/doc/html/rfc8949>
* **CRC-32C (RFC 3720 appendix A)**: <https://datatracker.ietf.org/doc/html/rfc3720>
* **minisign**: <https://jedisct1.github.io/minisign/>
* **RP2350 Secure Boot** — Kapitel in „RP2350 Datasheet" (Raspberry Pi Foundation)
* **TinyUSB MS OS 2.0 reference**: examples/device/webusb_serial
* **can2040** — K. O'Connor: <https://github.com/KevinOConnor/can2040>
* **Syzkaller**: <https://github.com/google/syzkaller>
* **USB3CV**: <https://www.usb.org/document-library/usb3cv-v1148>

---

*Ende des Dokuments.*
