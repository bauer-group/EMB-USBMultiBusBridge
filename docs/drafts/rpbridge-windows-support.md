# RPBridge – Windows-Support ohne eigenen Treiber

**Dokumentversion:** 1.0
**Datum:** 2026-04-22
**Scope:** Ergänzungsdokument zu `rpbridge-architektur.md`. Dieses Dokument beschreibt, wie die RPBridge unter Windows ohne eigenen Kernel-Treiber mit den mitgelieferten Klassentreibern (CDC-ACM, WinUSB, HID, MSC) zusammenarbeitet, wie das USB-Enumerations-Deskriptor-Set aufgebaut werden muss, damit Windows automatisch und installationsfrei bindet, und mit welchen Client-Bibliotheken die Endanwender auf Windows die Standardfunktionen nutzen.

Die Linux-Seite (nativer Treiberstack mit SocketCAN-, I²C-dev-, spidev-, GPIO-Chip- und IIO-Integration) ist Thema von `rpbridge-architektur.md`. Dieses Dokument ist **bewusst so angelegt, dass unter Windows kein Treiber signiert und kein Installer gebaut werden muss** — alle genutzten Komponenten sind inbox-Windows-Komponenten oder werden vom Open-Source-Ökosystem bereits unterschrieben bereitgestellt.

---

## 1. Strategie: Standard-USB-Klassen statt Vendor-Driver

Ein Windows-Treiber zu schreiben (KMDF + WHQL-Signatur) ist teuer, dauert Wochen bis Monate und bringt dem Projekt keinen Mehrwert — alle RPBridge-Funktionen lassen sich auf existierende, von Microsoft signierte Klassen-Treiber abbilden. Die Firmware ist der gesamte Unterschied. Windows-Nutzer bekommen damit:

* **Plug & Play** ohne Admin-Klick.
* **Keine Installer**, keine Signatur-Warnungen.
* **Upgrade-sicher** — Windows-Updates können keinen unsignierten Vendor-Treiber aussperren.

Die Strategie für jede der rpbridge-Funktionen:

| Funktion                    | Windows-Pfad                                 | Woher kommt die Software?         |
|-----------------------------|----------------------------------------------|-----------------------------------|
| **UART (RS232 / RS485 / TTL)** | CDC-ACM → virtuelle COM-Ports (`usbser.sys`) | inbox seit Win 10 1709            |
| **I²C, SPI, GPIO, PWM, ADC** | WinUSB + Userspace-Lib (`rpbridge-py` / `.NET`) | inbox via MS OS 2.0 Descriptors |
| **CAN 2.0B**                | Emulation der **gs_usb** / **candleLight**-Protokoll-Variante → bestehender WinUSB-basierter Treiber + Python-can/SocketCAN-Shim | Open-Source (candle, python-can) |
| **1-Wire**                  | WinUSB + Lib                                 | rpbridge-py                       |
| **OTA / Firmware-Update**   | USB MSC (BOOTSEL-UF2) → Drag & Drop auf Laufwerk | Windows Explorer (inbox)       |
| **Logging / Debug-Konsole** | CDC-ACM secondary interface → COM-Port        | inbox                              |

---

## 2. USB-Device-Deskriptor-Struktur

Das zentrale Design ist ein **Composite USB Device mit Interface Association Descriptors (IAD)**, damit Windows die Interface-Gruppen korrekt trennt und je Gruppe den richtigen Klassentreiber lädt.

### 2.1 Interface-Layout

```
Device (VID/PID = 0x1FC9 / 0xTBD, registriert über pid.codes)
│
├── Configuration 1 (bMaxPower = 250 → 500 mA)
│   │
│   ├── IAD #1 (bFirstInterface=0, bInterfaceCount=2, class=CDC)
│   ├── Interface 0: CDC-ACM Communication Class  (→ "rpbridge UART 1")
│   │   └── Endpoint 1 IN (Interrupt, Notification)
│   ├── Interface 1: CDC-ACM Data Class
│   │   ├── Endpoint 2 IN  (Bulk, RX from device)
│   │   └── Endpoint 2 OUT (Bulk, TX to device)
│   │
│   ├── IAD #2 (bFirstInterface=2, bInterfaceCount=2, class=CDC)
│   ├── Interface 2: CDC-ACM Communication Class  (→ "rpbridge UART 2 / PIO-UART")
│   ├── Interface 3: CDC-ACM Data Class
│   │
│   ├── IAD #3 (bFirstInterface=4, bInterfaceCount=2, class=CDC)
│   ├── Interface 4: CDC-ACM Communication Class  (→ "rpbridge Debug Log")
│   ├── Interface 5: CDC-ACM Data Class
│   │
│   ├── IAD #4 (bFirstInterface=6, bInterfaceCount=1, class=VendorSpecific)
│   ├── Interface 6: Vendor Class (WinUSB)           (→ "rpbridge Control")
│   │   ├── Endpoint 3 IN  (Bulk, Host-bound frames: I²C/SPI/GPIO replies, IRQ events)
│   │   └── Endpoint 3 OUT (Bulk, Device-bound commands)
│   │
│   └── IAD #5 (bFirstInterface=7, bInterfaceCount=1, class=VendorSpecific)
│       Interface 7: Vendor Class (gs_usb, WinUSB)   (→ "rpbridge CAN")
│           ├── Endpoint 4 IN  (Bulk, CAN RX frames)
│           └── Endpoint 4 OUT (Bulk, CAN TX frames, control)
```

**Begründung:**

* **Drei CDC-ACM-Ports** entsprechen den drei physischen UART-Kanälen (UART0 über Phy-Bank, PIO-UART #2, Debug-UART). Jeder erscheint unter Windows als eigener COM-Port (z. B. `COM12`, `COM13`, `COM14`) und ist mit PuTTY, TeraTerm, Arduino IDE, Visual Studio Serial Monitor und jeder seriellen Konsole sofort nutzbar.
* **Ein WinUSB-Vendor-Interface** („rpbridge Control") trägt die strukturierte Kommando-Pipeline aus `rpbridge-architektur.md` §4 (Capability-TLV, I²C-/SPI-/GPIO-/ADC-/PWM-/1-Wire-Frames). Unter Windows 8+ wird dieses Interface via **Microsoft OS 2.0 Descriptors** automatisch an `winusb.sys` gebunden, **ohne** dass der Nutzer eine INF-Datei installiert.
* **Ein separates gs_usb-kompatibles WinUSB-Interface** für CAN, damit die RPBridge als drop-in-kompatible candleLight-Alternative erscheint und existierende Tools (python-can, BUSMASTER, SavvyCAN, Cangaroo) ohne jede Modifikation funktionieren.
* **IAD (Interface Association Descriptors)** sind Pflicht, damit Windows die Paare (CDC-Control + CDC-Data) als Einheit an `usbser.sys` bindet und die Vendor-Interfaces separat an WinUSB. Ohne IAD würde Windows das Composite-Device falsch dekomponieren.

### 2.2 MS OS 2.0 Descriptor — automatische WinUSB-Bindung

Damit Windows ohne INF-Datei die beiden Vendor-Interfaces an WinUSB bindet, liefert die Firmware bei Enumeration **Microsoft OS 2.0 Descriptors** (BOS + Platform Capability Descriptor). Ablauf:

1. Windows liest den **Binary Device Object Store (BOS)** (USB 2.1 Feature).
2. Darin findet es eine **Platform Capability** mit der MS-OS-2.0-UUID (`{D8DD60DF-4589-4CC7-9CD2-659D9E648A9F}`).
3. Der Descriptor enthält einen Zeiger auf einen **vendor-specific String Descriptor (Index 0xEE in MS OS 1.0-Ära, heute per Control-Request)** mit:
   * `CompatibleID = "WINUSB"` für Interface 6 und 7,
   * `DeviceInterfaceGUID` für jedes Vendor-Interface (eigene, projektspezifische GUIDs — werden vom Userspace-Tool nach diesen GUIDs gesucht).

Die Kernstruktur (`MS_OS_20_DESCRIPTOR_SET_HEADER` + `MS_OS_20_FEATURE_REGISTRY_PROPERTY`) ist vollständig von TinyUSB implementiert und wird über einen einzigen Array-Literal konfiguriert. Minimalbeispiel:

```c
// firmware/src/usb/ms_os_20.c
#include "tusb.h"

#define MS_OS_20_DESC_LEN  0xB2

uint8_t const desc_ms_os_20[] = {
    /* Set header: length, type, windows version, total length */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    /* Configuration subset header — for config 0 */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    /* --- Function subset for Vendor Interface (Control, IF=6) --- */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    6, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    /* Compatible ID: WINUSB */
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W','I','N','U','S','B',0,0, 0,0,0,0,0,0,0,0,

    /* Registry property: DeviceInterfaceGUIDs (MULTI_SZ) */
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), // REG_MULTI_SZ
    U16_TO_U8S_LE(0x002A),
    /* "DeviceInterfaceGUIDs" in UTF-16LE */
    'D',0, 'e',0, 'v',0, 'i',0, 'c',0, 'e',0,
    'I',0, 'n',0, 't',0, 'e',0, 'r',0, 'f',0, 'a',0, 'c',0, 'e',0,
    'G',0, 'U',0, 'I',0, 'D',0, 's',0, 0,0,
    U16_TO_U8S_LE(0x0050),
    /* "{B83C0F56-... your GUID ...}" in UTF-16LE, terminated, MULTI_SZ -> two nulls */
    '{',0, 'B',0, '8',0, '3',0, 'C',0, '0',0, 'F',0, '5',0, '6',0,
    /* ... full GUID ... */
    '}',0, 0,0, 0,0,

    /* --- Function subset for gs_usb CAN Interface (IF=7) --- */
    /* (identical structure with CAN-specific GUID) */
};
```

Der dazugehörige BOS-Descriptor wird in `tud_descriptor_bos_cb()` gemeldet und der `MS_OS_20`-Blob in `tud_vendor_control_xfer_cb()` auf die MS-spezifische `bRequest = 0x01 /* MS_OS_20_VENDOR_CODE */` ausgeliefert.

### 2.3 Windows-Verhalten beim ersten Einstecken

Schritt für Schritt, direkt aus einem frischen Windows-11-Out-of-Box-Profil:

1. Benutzer steckt USB-C ein.
2. Windows enumeriert, sieht `VID:PID`, fordert alle Standard-Deskriptoren an.
3. Erkennt BOS mit Microsoft-OS-2.0-Platform-Capability.
4. Fordert MS-OS-2.0-Blob an, parst `WINUSB`-CompatibleID für IF 6 und IF 7.
5. Lädt **`winusb.sys`** für die beiden Vendor-Interfaces — **keine INF, keine Installer-UI, kein Neustart**.
6. Erkennt parallel die drei CDC-Interface-Paare, lädt `usbser.sys`, weist drei freie COM-Ports zu.
7. `Geräte-Manager` zeigt: drei COMs unter „Anschlüsse (COM & LPT)", zwei Einträge unter „Universelle Serielle Bus-Geräte".

Fertig. Anwender kann sofort loslegen.

---

## 3. UART-Funktionen via CDC-ACM

### 3.1 Was der Nutzer sieht

Drei COM-Ports. Jeder Port hat in seiner Beschreibung den zugehörigen Kanal, damit er auffindbar ist. Das wird über den **iInterface-String** gesetzt (TinyUSB: `TUD_CDC_DESCRIPTOR(... stridx, ...)`). Beispiel:

* `COM12 — rpbridge UART 1 (RS232/RS485/TTL)`
* `COM13 — rpbridge UART 2 (PIO)`
* `COM14 — rpbridge Debug Log`

### 3.2 Standardfeatures, die out-of-the-box funktionieren

CDC-ACM mappt auf Windows 1:1 auf die „Virtual COM Port"-Semantik:

* Baudrate 50…10 000 000 Bd → in der Firmware per `tud_cdc_set_line_coding_cb()` an den HW-UART weitergereicht.
* Parität, Stopbits, Datenbits.
* DTR/RTS-Leitungssignale (von Firmware frei interpretierbar — z. B. klassischer Reset-Trick: DTR=low → MCU-Reset; DTR+RTS=low → BOOTSEL — wie bei Arduino-Nano / Pico).
* Break-Signal (`tud_cdc_send_break_cb`).

Damit funktionieren **PuTTY, TeraTerm, RealTerm, MobaXterm, Tabby, Windows Terminal + `plink`, Arduino IDE, PlatformIO, CoolTerm, Bray's Terminal, minicom-Clone, .NET `System.IO.Ports.SerialPort`, PowerShell `Get-SerialPort`**. Keine Anpassung nötig.

### 3.3 Phy-Bank-Steuerung (RS232 vs. RS485 vs. TTL)

Der physische Layer von UART 1 wird über einen zusätzlichen **Control-Request** oder über einen **In-Band-Escape** auf dem Vendor-Interface umgeschaltet (siehe `rpbridge-architektur.md` §4.5, Kommando `SET_UART_PHY`). Für Anwender, die nur COM nutzen wollen, liefern wir:

* **`rpbridge-ctl.exe set-phy uart1 rs485 --term on`** — Windows-EXE, setzt über das WinUSB-Interface die Phy-Bank und RS485-Biasing, dann ist der COM-Port bereit.
* **Registry-basierter Default** pro VID/PID-Serial: Firmware merkt sich die letzte Phy-Wahl in Flash.

### 3.4 RS485-Direction-Control unter Windows

Automatische DE/RE-Umsteuerung erfolgt **in der Firmware** (PIO-timed, siehe `rpbridge-architektur.md` §6.2) — für den Windows-Anwender transparent. Kein DTR/RTS-Hacking in der Applikation nötig, wie er bei billigen FTDI-Adaptern üblich ist.

---

## 4. Steuerkanal via WinUSB

### 4.1 Userspace-APIs

Das Vendor-Interface ist unter Windows per **WinUSB API** (`winusb.h` in der Windows SDK) oder besser per **libusb** (Cross-platform, gleiche API wie unter Linux/macOS) erreichbar.

Wir empfehlen **libusb 1.0**, weil:

* eine einzige Userland-Codebasis für Windows/Linux/macOS genügt,
* Windows-libusb bindet intern an WinUSB (und fällt für andere Geräte auch auf libusbK/libusb0 zurück — für uns uninteressant, da wir WinUSB erzwingen),
* das Python-Ökosystem (`pyusb`, `libusb1`) identisch funktioniert.

### 4.2 Windows-Client-Bibliotheken

Das Repository liefert mehrere Sprach-Bindings, alle auf demselben binary Protokoll aufsetzend:

**Python** (`pip install rpbridge`):

```python
import rpbridge

dev = rpbridge.open()                    # erste gefundene Bridge
print(dev.capabilities())                # TLV-dekodierte Capability-Tabelle

# I²C
i2c = dev.i2c(bus=0, freq=400_000)
data = i2c.read(addr=0x76, length=6)     # BMP280 ID lesen

# SPI
spi = dev.spi(bus=0, mode=0, freq=1_000_000)
spi.cs(0, low=True)
rx = spi.transfer(bytes([0x9F, 0,0,0]))  # JEDEC ID eines Flash-ICs
spi.cs(0, low=False)

# GPIO
gpio = dev.gpio(30)
gpio.output()
gpio.write(True)

# ADC
volts = dev.adc(0).voltage()

# PWM
dev.pwm(31, freq=1000, duty=0.5)
```

**.NET / C#** (`dotnet add package RPBridge`):

```csharp
using RPBridge;

using var dev = RpBridgeDevice.Open();
var i2c = dev.OpenI2C(bus: 0, frequency: 400_000);
byte[] buf = i2c.Read(address: 0x76, length: 6);

var spi = dev.OpenSpi(bus: 0, mode: 0, frequency: 1_000_000);
spi.Cs[0].Low();
var rx = spi.Transfer(new byte[] { 0x9F, 0, 0, 0 });
spi.Cs[0].High();
```

**Rust** (`cargo add rpbridge`):

```rust
use rpbridge::{Device, I2cMode};

fn main() -> anyhow::Result<()> {
    let dev = Device::open_first()?;
    let i2c = dev.i2c(0, I2cMode::Fast)?;
    let data = i2c.read(0x76, 6)?;
    println!("BMP280 id = {:02x?}", data);
    Ok(())
}
```

**PowerShell-Cmdlets** (für Scripting + CI-Automatisierung, via .NET-Bridge):

```powershell
Import-Module RPBridge
Get-RPBridgeDevice | Format-Table
$dev = Get-RPBridgeDevice | Select-Object -First 1
Invoke-RPBridgeI2cRead -Device $dev -Bus 0 -Address 0x76 -Length 6
Set-RPBridgeGpio -Device $dev -Pin 30 -Value $true
```

**Kommandozeile (`rpbridge-ctl.exe`)** — ein einziges statisch gelinktes Binary, in jedem Release-Artefakt enthalten:

```
C:\> rpbridge-ctl list
Index  Serial           Firmware   Capabilities
  0    RPB-0001234A     v1.2.0     UART×3 I²C×2 SPI×2 GPIO×14 ADC×4 CAN 1-Wire

C:\> rpbridge-ctl i2c --bus 0 scan
I²C-Bus 0 @ 400 kHz
Found: 0x48, 0x76

C:\> rpbridge-ctl gpio 30 set high

C:\> rpbridge-ctl can --bitrate 500k dump
2026-04-22T12:34:56.123  RX  ID=0x123 DLC=8  de ad be ef ca fe 42 00
...

C:\> rpbridge-ctl flash --fw rpbridge.uf2
Triggering BOOTSEL... OK
Device re-enumerated as RP2350 MSC
Writing UF2 to E:\... done (512 KB / 1.2 s)
Waiting for re-enum as rpbridge... OK (v1.2.1)
```

### 4.3 Co-Existenz mit anderen WinUSB-Geräten

Die projekteigenen DeviceInterfaceGUIDs (eine pro Vendor-Interface, z. B. `{B83C0F56-...}` für Control und `{A11E6D8C-...}` für CAN) garantieren, dass die rpbridge-Libraries nur die RPBridge-Geräte sehen. Andere WinUSB-Geräte (DAPLinks, U2F-Keys, Digilent WaveForms) stören nicht.

---

## 5. CAN unter Windows — gs_usb-Kompatibilität

### 5.1 Warum gs_usb?

Windows hat **keine native CAN-Klasse**. Jeder CAN-Adapter-Hersteller (PEAK, Kvaser, Vector, Ixxat) liefert einen proprietären Treiber + eigene API (PCAN-Basic, CANlib, VCI, …). Das wollen wir **nicht** replizieren.

Stattdessen wählen wir als Windows-USB-Protokoll die Pakettruktur des **gs_usb / candleLight**-Projekts (Geschwister Schneider USB-CAN). Warum:

* Es ist ein **offener, simpler USB-Vendor-Klassenstandard**, ursprünglich für die candleLight-Hardware.
* Unter **Linux** ist `gs_usb` ein **in-tree-Mainline-Kernel-Treiber** (drivers/net/can/usb/gs_usb.c) — dadurch bekommen Linux-Nutzer **kostenlos SocketCAN** (wir nutzen für die RPBridge bereits ausschließlich SocketCAN, siehe `rpbridge-architektur.md` §7).
* Unter **Windows** gibt es ein **open-source, signiert verfügbares** Treiber- und Tool-Ökosystem:
  * `candle_api` (libusb-basiert, cross-plat Library — wird für Windows als WinUSB-Client genutzt),
  * `python-can` mit `interface='gs_usb'` Backend,
  * **SavvyCAN, Cangaroo, BUSMASTER** binden über dasselbe Protokoll ein.

Mit anderen Worten: Wenn die RPBridge auf dem CAN-Vendor-Interface gs_usb-kompatible Frames spricht, bekommen wir Windows-Support **durch fremde Open-Source-Tools, ohne einen eigenen Windows-Treiber zu bauen oder auszuliefern**.

### 5.2 Protokoll-Mapping

gs_usb definiert vier Endpoints (Control + Bulk IN + Bulk OUT) und einen schlanken Satz Control-Requests (SET_MODE, SET_BITTIMING, SET_IDENTIFY, GET_DEVICE_CONFIG, …). Frame-Format:

```c
struct gs_host_frame {
    uint32_t echo_id;
    uint32_t can_id;     // mit RTR/EFF/ERR-Flags
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[8];
    uint32_t timestamp_us;
};
```

Die RPBridge-Firmware (`firmware/src/can/gs_usb_glue.c`) setzt auf can2040 (siehe `rpbridge-architektur.md` §6.1) auf und übersetzt:

* gs_usb SET_BITTIMING → `can2040_set_bitrate()`,
* gs_usb SET_MODE (Start/Stop/Normal/ListenOnly/LoopBack) → can2040-Modus,
* gs_usb Host→Device-Frames → `can2040_transmit()`,
* can2040-RX-Callback → gs_usb-Host-Frame mit Zeitstempel.

Damit ist die RPBridge unter Windows **bit-exakt** als candleLight-Gerät sichtbar und jedes Tool, das candleLight unterstützt, funktioniert sofort — bis hin zur SocketCAN-Shim-Lib (`python-can gs_usb`).

### 5.3 Windows-Anwender-Workflow

1. RPBridge einstecken, Windows lädt WinUSB (siehe §2.3).
2. `pip install python-can` + `pip install gs_usb` (oder, je nach Version, `python-can[gs-usb]`).
3. ```python
   import can
   bus = can.Bus(interface='gs_usb', channel=0, bitrate=500_000)
   for msg in bus:
       print(msg)
   ```
4. Oder nativ in **SavvyCAN**: `Connection → Add New Device → CANDleLight USB / gs_usb`.

### 5.4 Alternative Protokoll-Profile (optional)

Als sekundäres Profil kann die Firmware zusätzlich das **Lawicel / CANUSB ASCII-Protokoll** über einen der CDC-ACM-Ports liefern. Vorteil: Anwender, die ausschließlich Text-basiert arbeiten wollen (einfache Logger, uralte CANUSB-Tools), sind bedient. Auswahl per `rpbridge-ctl.exe can-profile [gs_usb|slcan]`.

---

## 6. Firmware-Update (OTA) unter Windows

### 6.1 BOOTSEL / UF2

RP2350 hat im ROM einen USB-Mass-Storage-Bootloader. Workflow unter Windows:

1. `rpbridge-ctl.exe reboot --bootsel` (triggert `reset_usb_boot()` auf dem Gerät).
2. Gerät reenumeriert nach ~300 ms als USB-Laufwerk `RP2350` (z. B. `E:`).
3. UF2-Datei per Drag-&-Drop oder über `rpbridge-ctl.exe flash --fw rpbridge.uf2` kopieren.
4. Gerät bootet automatisch neu in die neue Firmware.

Alles funktioniert mit inbox-Windows-Komponenten (USB-Mass-Storage = `usbstor.sys`/`disk.sys`). Kein Installer, keine Signatur-Warnung.

### 6.2 Integritätsprüfung

`rpbridge-ctl.exe` prüft vor dem Flashen:

* SHA-256 der UF2 gegen die mitgelieferte Checksums-Datei aus dem GitHub-Release (`CHECKSUMS.txt`, signiert mit minisign — siehe `rpbridge-cicd.md` §8).
* Version-Kompatibilität gegen die Hardware-Revision (Feldset im UF2-Header, von der Firmware-Build-Pipeline eingebettet).

---

## 7. Beschriftung, Icons, User-Experience

Damit die RPBridge unter Windows **nicht als anonymes** „USB Composite Device" erscheint, setzt die Firmware gezielt diese String-Deskriptoren:

| String              | Inhalt                               | Sichtbarkeit                               |
|---------------------|--------------------------------------|--------------------------------------------|
| `iManufacturer`     | `rpbridge.org`                       | Geräte-Manager „Hersteller"                |
| `iProduct`          | `RPBridge Multi-Bus Developer Bridge` | Geräte-Manager „Name", USB-Icon-Bubble    |
| `iSerialNumber`     | z. B. `RPB-0001234A` (aus Flash-UID) | Eindeutige Kennung, Basis für rpbridge-ctl |
| `iInterface` je IF  | „UART 1 (RS232/RS485/TTL)", „UART 2 (PIO)", „Debug Log", „Control", „CAN" | Unterscheidung im Geräte-Manager |

Optional kann ein **MSI-Installer** mitgeliefert werden, der nur Icon-/Registry-Einträge setzt (nicht den Treiber) — reine UX-Politur, nicht funktional nötig.

---

## 8. Signierung und Policy

### 8.1 Was wir NICHT brauchen

* **Keine WHQL-Signatur** — wir liefern keinen Kernel-Treiber.
* **Keine Attestation Signatur** — WinUSB benötigt keine, bindet allein auf Basis MS OS 2.0 Descriptors.
* **Keine EV-Zertifikats-Kosten** — wir liefern kein `.sys`, `.inf` oder `.cat`.

### 8.2 Was wir signieren sollten

* **`rpbridge-ctl.exe`** und mitgelieferte DLLs: **Authenticode-Code-Signing** mit einem OV- oder EV-Zertifikat (ca. 100–300 €/Jahr). Ohne Signatur warnt Windows SmartScreen zunächst — nicht blockierend, aber störend.
* **Installer (`.msi`)**: ebenfalls Authenticode.
* Build-Integrität: `rpbridge.uf2` per **minisign** signieren (Linux-Standard), Public-Key im Repo + in `rpbridge-ctl.exe` einkompiliert.

Wer gänzlich ohne kommerzielles Zertifikat auskommen will: die Tools werden auch als **portable ZIP** ausgeliefert, dort greift SmartScreen erst ab 100+ Downloads „vertrauenswürdig" — in der Praxis für Open-Source-Hardware unkritisch.

---

## 9. Tests und Compliance

### 9.1 USB-Tests unter Windows

Vor jedem Release durchlaufen wir auf einer Windows-11-Referenzinstallation:

1. **`usbview.exe`** (aus Windows SDK): Descriptor-Tree visualisieren, IADs, MS-OS-2.0-BOS, Strings, Endpoints.
2. **USB 3 Command Verifier (USB3CV, USB-IF Test-Suite)**: Descriptor-Konformität, Chapter-9-Tests, CDC-ACM-Konformität, WinUSB-Enumerations-Konformität.
3. **Device Manager → Properties → Events**: keine Gelb-/Roten-Icons, keine „Driver not found"-Events.
4. **`PnPutil /enum-devices /connected`**: RPBridge taucht mit allen Interfaces, mit Klassentreiber-Zuordnung (usbser, WinUSB) auf.
5. **`WPP / ETW`-Traces** aus `usbser`-Provider: keine Bus-Resets, keine Control-Transfer-Fehler.

### 9.2 Anwendungstests

| Szenario                                   | Test-Tool                     | Soll-Ergebnis                          |
|--------------------------------------------|-------------------------------|----------------------------------------|
| COM-Port öffnen, 921 600 Bd, 10 MB senden  | Tera Term + Loopback          | 0 Bit-Fehler                           |
| DTR/RTS-Toggle → Arduino-Reset/BOOTSEL-Trick | `rpbridge-ctl` + Reset-Test  | Board rebootet in BOOTSEL              |
| I²C-Scan + SHT40 kontinuierlich            | Python-Client, 1 h            | 0 Timeouts                             |
| SPI 24 MHz Flash-Dump 16 MB                | `rpbridge-ctl spi-dump`       | Korrektes Image                        |
| CAN @ 1 Mbit/s, 50 % Bus-Last, 24 h        | SavvyCAN + python-can         | Keine Framer-Drops, korrekte Timestamps |
| Hot-Unplug während I²C-Transfer             | manuell                       | Saubere Fehlermeldung, kein BSOD       |
| Gerät beim Suspend/Resume                   | S3-Cycle 100× per `pwrtest`   | Kein Enumerations-Fehler                |

### 9.3 Kompatibilitäts-Support-Matrix

| Windows-Version       | Status      | Bemerkung                                |
|-----------------------|-------------|------------------------------------------|
| Windows 11 (alle SKUs)| **supported** | Primary-Target                          |
| Windows 10 21H2+      | **supported** | WinUSB via MS OS 2.0 inbox              |
| Windows 10 1709–21H1  | supported   | getestet nur sporadisch                  |
| Windows 10 < 1709     | limited     | usbser.sys muss manuell via INF installiert werden (Inbox fehlt). INF im Repo. |
| Windows 8.1          | nicht getestet | sollte prinzipiell gehen                |
| Windows 7            | nicht unterstützt | WinUSB-Inbox fehlt, End-of-Life     |

---

## 10. Umsetzungs-Checkliste

In der Reihenfolge, wie sie bei der Firmware-Implementierung abgehakt wird:

1. [ ] TinyUSB composite device mit drei CDC-ACM-Paaren + zwei Vendor-Interfaces aufsetzen, inkl. IADs.
2. [ ] Eindeutige `VID:PID` beantragen (pid.codes-Block, kostenlos für Open-Source) — parallel unique `iSerialNumber` aus dem RP2350-Flash-Unique-ID generieren.
3. [ ] BOS-Deskriptor mit MS OS 2.0 Platform-Capability implementieren (TinyUSB-Hook `tud_descriptor_bos_cb`).
4. [ ] MS OS 2.0 Descriptor-Set mit CompatibleID „WINUSB" + DeviceInterfaceGUID für IF 6 und IF 7 statisch definieren.
5. [ ] gs_usb-Protokoll-Glue zwischen can2040 und TinyUSB-Vendor-Endpoint implementieren, Control-Requests (SET_MODE, SET_BITTIMING, SET_IDENTIFY, GET_DEVICE_CONFIG) bedienen.
6. [ ] Windows-11-Referenzinstallation per USB3CV + usbview durchtesten, Iterationen bis „green".
7. [ ] `rpbridge-ctl.exe` für Windows (Rust) auf libusb-Basis bauen, DeviceInterfaceGUID-Scan → Wahl-Menü → alle Subkommandos.
8. [ ] Python-Package `rpbridge` mit pyusb-Backend, publizieren auf PyPI (`pip install rpbridge`).
9. [ ] .NET-Paket `RPBridge` mit LibUsbDotNet, publizieren auf nuget.org.
10. [ ] Windows-Anwendungstests nach §9.2, 24-h-Stresstest CAN, I²C, SPI, UART.
11. [ ] Authenticode-Signatur für `rpbridge-ctl.exe` und DLLs im GitHub-Release-Workflow (`rpbridge-cicd.md` §8 erweitern um Windows-Signing-Step mit `signtool.exe` + hardware-token oder Azure-Trusted-Signing).
12. [ ] Dokumentation: „Quickstart Windows" (A4, 1 Seite) — drei Screenshots (Gerätemanager, PuTTY, SavvyCAN), Links zu `pip`/`nuget`/`rpbridge-ctl`.

---

## 11. Referenzen

* **Microsoft Docs — WinUSB Device**: <https://learn.microsoft.com/windows-hardware/drivers/usbcon/winusb-installation>
* **Microsoft Docs — Automatic WinUSB Binding via MS OS 2.0 Descriptors**: <https://learn.microsoft.com/windows-hardware/drivers/usbcon/automatic-installation-of-winusb>
* **USB Device Class Specification — CDC-ACM 1.2**: <https://www.usb.org/document-library/class-definitions-communication-devices-12>
* **TinyUSB — MS OS 2.0 Descriptor Example**: <https://github.com/hathach/tinyusb/tree/master/examples/device/webusb_serial>
* **TinyUSB — Composite CDC+Vendor**: <https://github.com/hathach/tinyusb/tree/master/examples/device/cdc_msc>
* **candleLight Firmware (gs_usb reference)**: <https://github.com/candle-usb/candleLight_fw>
* **gs_usb Linux-Kernel-Treiber**: `drivers/net/can/usb/gs_usb.c` im Mainline-Tree
* **python-can gs_usb backend**: <https://python-can.readthedocs.io/en/stable/interfaces/gs_usb.html>
* **SavvyCAN**: <https://github.com/collin80/SavvyCAN>
* **Microsoft USB3CV**: <https://www.usb.org/document-library/usb3cv-v1148>
* **USB-IF CDC Usage Model Guidelines** (CDC-ACM Mode 0 / Abstract Control Model)
* **pid.codes USB Open-Source VID/PID allocation**: <https://pid.codes>

---

*Ende des Windows-Support-Ergänzungsdokuments.*
