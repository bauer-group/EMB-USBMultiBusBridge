# RP2350 USB-Multi-Bus-Bridge — Architektur- und Implementierungsleitfaden

*Arbeitsgrundlage für Firmware-, Kerneltreiber- und Anwendungsentwicklung.*
*Ziel-Chip: RP2350 (Fallback RP2040). Pico-SDK als Firmware-Basis, Linux-Kerneltreiber als MFD-Familie nach Vorbild `dln2`.*

---

## 1. Ziel, Scope, Nicht-Ziele

Ein USB-Full-Speed-Gerät auf Basis RP2350, das einem Linux-Host die folgenden Busse und Peripherien so zur Verfügung stellt, als wären sie nativ am SoC vorhanden:

- **UART** (mehrere Instanzen, inkl. RS232/RS485-Varianten mit externen Transceivern)
- **I²C** Master (Standard-, Fast- und Fast-Plus-Mode)
- **SPI** Master
- **GPIO** inkl. Interrupt-Eingängen
- **PWM** (Hardware-PWM-Slices)
- **ADC** (über IIO-Subsystem)
- **CAN 2.0B** *softwareseitig in PIO implementiert* (als Referenzbeispiel für die SDK/PIO-Kombination; externer CAN-Transceiver erforderlich)

Auf der Host-Seite erscheinen diese als `/dev/i2c-N`, `/dev/spidevX.Y`, `/dev/gpiochipN`, `/sys/class/pwm/…`, `/sys/bus/iio/devices/iio:deviceN`, `/dev/ttyACM*`, `can0` — alle mit Standard-Userspace-Tools (`i2c-tools`, `libgpiod`, `iio_info`, `can-utils`) ohne proprietäre Bibliotheken verwendbar.

OTA-Firmware-Update geschieht bewusst einfach gehalten über den ROM-Bootloader (BOOTSEL / UF2) per `picotool` — kein eigener Bootloader, keine Signierung, keine A/B-Slots.

**Nicht-Ziele** in diesem Dokument: Windows-/macOS-Treiber, USB-High-Speed (der RP2350 hat nur FS-PHY), isochroner Audio-Stream (I²S-UAC2 ist als zukünftige Erweiterung vorgesehen, aber nicht Teil dieser Spec), feldbus-Varianten jenseits CAN 2.0.

---

## 2. Systemübersicht

```
+--------------------- Linux Host -----------------------+
|                                                        |
|  Userspace: i2c-tools, libgpiod, can-utils, spidev,    |
|             iio_info, picotool, fwupd-Plugin (später)  |
|     |        |        |         |         |           |
|  /dev/i2c-N /dev/gpio*/dev/spidev can0    iio:deviceN  |
|     |        |        |         |         |           |
|  +--i2c--+ +-gpio-+ +-spi-+  +-can-+   +-iio-adc-+     |
|  |       | |      | |     |  |     |   |         |     |
|  +---------- rpbridge MFD core ---------+            |
|  |         usb bulk/interrupt pipes                |   |
+--|------------------------------------------------|---+
   |                                                |
   |                  USB 2.0 FS                    |
   |                                                |
+--v------------------------------------------------v---+
|                    RP2350                              |
|                                                        |
|   Core0: TinyUSB | Cmd-Router | Capability | Pin-Broker|
|   Core1: I/O-Scheduler | DMA-IRQ | PIO-Service         |
|                                                        |
|   DMA-Chan ├── USB EP FIFO ⇄ Peripherie-FIFO           |
|                                                        |
|   PIO0: can2040 TX/RX/sync  (3 SMs)                    |
|   PIO1: extra UARTs, I²S, 1-Wire (4 SMs)               |
|   PIO2: frei / WS2812 / parallel-LCD (4 SMs)           |
|                                                        |
|   HW-Periph: 2× UART, 2× I²C, 2× SPI, 24× PWM, 8× ADC  |
|                                                        |
+--|------------------------------------------------|----+
   |                                                |
 Transceiver: MAX3232 (RS232), MAX485 (RS485), SN65HVD230 (CAN)
```

---

## 3. Hardware

### 3.1 Chipwahl RP2350

- Dual Cortex-M33 @ 150 MHz, FPU + DSP, TrustZone (optional), ROM mit Secure/Signed Boot.
- 520 KB SRAM, getrennt in 10 Bänke für parallele DMA-Zugriffe.
- **12 PIO-State-Machines in 3 PIO-Blöcken** (32 Instruktionen je Block) — Schlüssel für CAN-in-PIO und weitere Soft-Peripherien.
- 16 DMA-Kanäle.
- QFN-80-Variante (RP2350B) mit 48 GPIOs empfohlen, damit Pin-Muxing nicht zum Engpass wird.
- USB 2.0 Full-Speed (12 Mbit/s) — die einzige echte Bandbreitengrenze; netto ca. 1 MB/s, die sich alle Busse teilen.

### 3.2 Referenz-Pinout (QFN-80, Vorschlag)

| Funktion                    | Pins (Beispiel)            | Bemerkung                                       |
|----------------------------|----------------------------|-------------------------------------------------|
| USB D+/D-                  | USB_DP, USB_DM             | dedizierte Pins                                 |
| UART0 (Debug)              | GP0/GP1                    | intern, serielle Konsole, RTT als Alternative   |
| UART1 HW (User)            | GP4/GP5                    | freigeführt                                     |
| UART PIO (User ×2)         | GP6/GP7, GP8/GP9           | PIO1                                            |
| I²C0 HW                    | GP2/GP3                    | Standard/Fast/Fast+                             |
| I²C1 HW                    | GP18/GP19                  |                                                  |
| SPI0 HW                    | GP10–GP13                  | SCK/MOSI/MISO/CS0; zusätzliche CS als GPIO      |
| PWM                        | beliebige GPIO             | Hardware-PWM-Slices chipweit verfügbar          |
| ADC                        | GP26–GP29, + Chip-interne  | 8 Kanäle RP2350                                 |
| CAN-TX/RX (PIO)            | GP20/GP21                  | an SN65HVD230; PIO0 komplett                    |
| RS485 DE                   | ein GPIO je UART-Instanz   | PIO toggelt synchron zur TX                     |
| GPIO (User)                | restliche Pins             | Pin-Broker verwaltet Zuweisung                  |
| BOOTSEL                    | Button auf QSPI_SS         | für Firmware-Recovery                           |

### 3.3 Externe Beschaltung

- **CAN**: SN65HVD230 (3,3 V), 120 Ω-Terminierung per Jumper, optional Split-Termination für EMV. TX → PIO_TX, RX → PIO_RX. RP2350-Pin 5 V-tolerant? Nein — bei 5-V-Transceivern Pegelwandler vorsehen. Der HVD230 ist 3,3-V-nativ.
- **RS485**: MAX3485 o. ä. mit DE-Pin. DE wird von einer PIO-SM synchron zur UART-TX gesteuert (Gain: präzises Timing ohne CPU-Beteiligung).
- **RS232**: MAX3232 (3,3 V). Kein spezielles Timing nötig.
- **Schutz**: TVS-Dioden auf allen externen Leitungen, Ferrite auf den Versorgungen, ESD-Schutz auf den User-GPIOs (insb. wenn die Bridge als Laborwerkzeug dient).

---

## 4. Firmware-Architektur (pico-sdk)

### 4.1 Projektstruktur

```
firmware/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── src/
│   ├── main.c              # init, launch core1, main loop on core0
│   ├── usb/
│   │   ├── usb_descriptors.c/h
│   │   ├── tusb_config.h
│   │   ├── vendor_class.c   # vendor interface + cmd router
│   │   └── cdc_glue.c       # CDC-ACM <-> UART bridge
│   ├── core1/
│   │   ├── core1_main.c     # I/O scheduler
│   │   └── queues.c/h       # lockfree ringbuffer, SIO FIFO helper
│   ├── periph/
│   │   ├── pin_broker.c/h   # Zuweisung & Konfliktprüfung
│   │   ├── i2c.c/h
│   │   ├── spi.c/h
│   │   ├── uart_hw.c/h
│   │   ├── pwm.c/h
│   │   ├── adc.c/h
│   │   └── gpio.c/h
│   ├── pio/
│   │   ├── applets.c/h      # Applet-Registry
│   │   ├── can2040_glue.c   # Bindung der can2040-Library
│   │   ├── uart_pio.pio
│   │   ├── i2s_pio.pio
│   │   ├── onewire.pio
│   │   └── ws2812.pio
│   ├── capability/
│   │   ├── cap_table.c/h    # Pin-Fähigkeiten, Applet-Liste, DT-Fragments
│   │   └── cap_descriptor.c # serialisiert für den Host
│   └── cmd/
│       ├── framing.c/h      # Command/Response-Protokoll
│       └── dispatch.c/h     # Subsystem-Dispatcher
├── third_party/
│   └── can2040/             # Git-Submodul: github.com/KevinOConnor/can2040
└── tools/
    ├── build.sh
    └── flash.sh             # picotool load -fxv
```

### 4.2 CMake-Grundgerüst

```cmake
cmake_minimum_required(VERSION 3.20)
include(pico_sdk_import.cmake)
project(rpbridge C CXX ASM)
set(PICO_BOARD pico2)        # RP2350
pico_sdk_init()

add_executable(rpbridge
    src/main.c
    src/usb/usb_descriptors.c
    src/usb/vendor_class.c
    src/usb/cdc_glue.c
    src/core1/core1_main.c
    src/core1/queues.c
    src/periph/pin_broker.c
    src/periph/i2c.c
    src/periph/spi.c
    src/periph/uart_hw.c
    src/periph/pwm.c
    src/periph/adc.c
    src/periph/gpio.c
    src/pio/applets.c
    src/pio/can2040_glue.c
    src/capability/cap_table.c
    src/capability/cap_descriptor.c
    src/cmd/framing.c
    src/cmd/dispatch.c
    third_party/can2040/src/can2040.c
)

pico_generate_pio_header(rpbridge ${CMAKE_CURRENT_LIST_DIR}/src/pio/uart_pio.pio)
pico_generate_pio_header(rpbridge ${CMAKE_CURRENT_LIST_DIR}/src/pio/i2s_pio.pio)
pico_generate_pio_header(rpbridge ${CMAKE_CURRENT_LIST_DIR}/src/pio/onewire.pio)
pico_generate_pio_header(rpbridge ${CMAKE_CURRENT_LIST_DIR}/src/pio/ws2812.pio)

target_include_directories(rpbridge PRIVATE
    src
    third_party/can2040/src
)
target_link_libraries(rpbridge
    pico_stdlib pico_multicore
    hardware_dma hardware_pio hardware_irq
    hardware_i2c hardware_spi hardware_uart
    hardware_pwm hardware_adc hardware_gpio
    tinyusb_device tinyusb_board
)
pico_add_extra_outputs(rpbridge)      # erzeugt rpbridge.uf2
```

### 4.3 Core-Split und Scheduling

**Core 0** betreibt:
- TinyUSB Event-Loop (`tud_task()`) — nicht blockieren, keine langen Sektionen
- Kommando-Framing: Paket aus Bulk-OUT URB holen, Header parsen, an Subsystem-Handler dispatchen
- Capability-Descriptor-Auslieferung (Control-Transfer)
- Konfigurationsoperationen (Pin-Zuweisungen, Bitraten, Modi) — das sind seltene, nicht zeitkritische Requests

**Core 1** betreibt:
- den I/O-Scheduler für die PIO-Applets und nicht-DMA-getriebene Peripheriepfade
- PIO-IRQ-Handler (wenn z. B. can2040 einen Frame fertig hat, wird er in eine Core-1-Queue geschrieben)
- DMA-IRQs für lange Transfers (der Wert ist hier, dass ein blockierender Transfer auf Core0 die USB-Event-Loop nicht stört)
- Periodische Aufgaben (Timestamp-Sync-Pakete, ADC-Sampling-Timer)

**Inter-Core-Kommunikation**:
- zwei Lockfree-SPSC-Ringbuffer im SRAM (eine Richtung je Ring), Kapazität 64 Slots à 64 Byte
- die SIO-FIFOs werden nur für „Wakeups" / leichte Notifications genutzt, nicht als Datenpfad (sie sind nur 8 Einträge tief)
- Memory-Barrieren: `__dmb()` am Ende des Writers, `__dmb()` am Anfang des Readers — der M33 verlangt das.

### 4.4 Interrupt-Prioritäten (NVIC)

Empfohlene Zuordnung (niedriger Wert = höher priorisiert):

| Priorität | IRQ                                | Begründung                                         |
|-----------|------------------------------------|----------------------------------------------------|
| 0         | PIO0_IRQ0 (CAN RX edge)            | CAN-Bit-Timing darf keine Latenz erleiden          |
| 1         | PIO0_IRQ1 (CAN TX bit boundary)    | dito                                                |
| 2         | DMA_IRQ_0 (USB-Out-Bulk-Complete)  | Host-Durchsatz                                     |
| 2         | DMA_IRQ_1 (Peripherie-DMA)         | I²C/SPI-Throughput                                 |
| 3         | USBCTRL_IRQ                        | TinyUSB-Kern                                       |
| 4         | UART/I²C/SPI Fehler-IRQs           | selten, kürzer als eine Bittransaktion             |
| 5         | Timer-IRQ (Sync-Paket 1 Hz)        | unkritisch                                          |

Die can2040-Bibliothek verlangt feste Priorisierung ihrer PIO-IRQs und dokumentiert das explizit — den Wert nicht eigenmächtig anheben.

### 4.5 Memory-Map-Vorgaben

- Flash-Layout: ein einziges Image; der SDK-Default reicht. Bei einer späteren A/B-Erweiterung später zwei Partitionen via custom linker script.
- SRAM:
  - Bank 0: Stacks Core0/Core1
  - Bank 1: TinyUSB-Puffer (muss `__attribute__((aligned(4)))`, getrennte Bank verringert DMA-Stalls)
  - Bank 2: DMA-Deskriptoren und Peripherie-Ringpuffer
  - Bank 3–8: frei, Zuweisung über Linker-Sections bei Bedarf
- Stack-Check: MPU-Region an den Enden der Stacks für Overflow-Fallen (M33-Feature).

---

## 5. USB-Protokoll

### 5.1 Composite-Deskriptor-Layout

| # | Interface             | Klasse      | Endpunkte                          | Zweck                                |
|---|-----------------------|-------------|------------------------------------|--------------------------------------|
| 0 | CDC-ACM #0 (Control)  | 0x02/0x02   | EP1-IN (Interrupt)                 | UART-Instanz 1 (HW-UART1)            |
| 1 | CDC-ACM #0 (Data)     | 0x0A        | EP2-IN, EP2-OUT (Bulk)             |                                      |
| 2 | CDC-ACM #1 (Control)  | 0x02/0x02   | EP3-IN                              | UART-Instanz 2 (PIO-UART)            |
| 3 | CDC-ACM #1 (Data)     | 0x0A        | EP4-IN, EP4-OUT                    |                                      |
| 4 | Vendor                | 0xFF/0x00   | EP5-IN, EP5-OUT (Bulk), EP6-IN (Interrupt) | alles andere (I²C, SPI, GPIO, PWM, ADC, CAN) |

Die Vendor-Schnittstelle trägt ein **Interface-Association-Descriptor (IAD)**, damit Linux sie als eigene logische Funktion erkennt. IDs und Strings werden über Class-Specific-Descriptoren selbstbeschrieben.

VID/PID: eine **pid.codes**-Allokation (z. B. 0x1209:0xXXXX) ist für ein Open-Source-Projekt der kanonische Weg. Microsoft OS 2.0 Descriptors (`WINUSB`) optional mitgeben, damit Windows-Hosts später ohne INF-Frickelei funktionieren.

### 5.2 Capability-Descriptor

Beim Binding ruft der Host einen **Vendor-Class-Specific-GET** (`bmRequestType=0xC0, bRequest=0x10`) auf und erhält eine TLV-Struktur (Type-Length-Value) mit:

```
[Header]
  magic      : u32 = 'RPBR'
  version    : u16  (0x0100)
  fw_rev     : u16
  serial     : u8[16]   // aus flash id
  caps_bits  : u32      // grobe Feature-Flags
  n_pins     : u16
  n_subsys   : u16
  n_applets  : u16
  dt_frag_sz : u16      // Länge des DT-Overlay-Fragments
[PinTable]
  struct pin_entry {
    u8  id;            // interne Pin-Nummer (0..47)
    u8  gpio_nr;       // physische GPIO am RP2350
    u32 modes_bitmap;  // GPIO | I2C_SDA | I2C_SCL | SPI_SCK | ... | CAN_TX | CAN_RX
    u32 flags;         // pull-up vorhanden, 5V-tol, analog-capable, ...
    u8  reserved_for;  // aktuell von Subsys # belegt (0xFF = frei)
  }[n_pins];
[SubsysTable]
  struct subsys_entry {
    u8  id;            // SUBSYS_I2C, SUBSYS_SPI, ...
    u8  instance;      // 0, 1, ...
    u8  backend;       // HW, PIO, BITBANG
    u8  flags;
    u16 max_freq_khz;
    u16 fifo_depth;
    u8  str_idx;       // Index in String-Tabelle (name)
  }[n_subsys];
[AppletTable]
  struct applet_entry {
    u8  id;            // APPLET_UART_PIO, APPLET_CAN_PIO, APPLET_I2S_TX, ...
    u8  pio_block;
    u8  pio_sm_mask;   // welche SMs das Applet belegt
    u8  instr_used;
    u8  str_idx;
  }[n_applets];
[StringTable]
  ... zero-terminated UTF-8 strings referenced by str_idx ...
[DtFragment]
  flattened device tree fragment, siehe §7.6
```

Diese Struktur entkoppelt Firmware und Treiber: jede neue Firmwareversion darf Subsysteme oder Pins hinzufügen, ohne dass der Treiber aktualisiert werden muss, solange die Version kompatibel bleibt.

### 5.3 Command-Framing (Bulk)

Jedes Paket auf dem Vendor-Bulk-Endpoint trägt einen 8-Byte-Header plus Payload:

```
offset  size  field
0       1     magic      (0xA5)
1       1     subsys     (I2C=1, SPI=2, GPIO=3, PWM=4, ADC=5, CAN=6, SYS=0)
2       1     opcode     (siehe pro Subsystem)
3       1     flags      (bit0: expect_response, bit1: streaming)
4       2     xid        (Transaction-ID, wrap-around)
6       2     payload_len
8..     N     payload
```

Antworten spiegeln `xid`, tragen denselben `subsys/opcode` und setzen in `flags` Bit 7 (`RESPONSE`) oder Bit 6 (`ERROR`). Bei Error folgt in der Payload ein `u16 errno` (POSIX-kompatibel gewählt: `-EIO=5`, `-ENODEV=19`, `-EBUSY=16`, `-EPROTO=71`, `-ETIMEDOUT=110`).

Die Transaktions-ID erlaubt dem Linux-Treiber mehrere Requests in-flight zu halten; das ist entscheidend für Durchsatz, da ein Roundtrip über USB-FS bei ≈1 ms liegt.

### 5.4 Event-Kanal

Der Interrupt-IN-Endpoint (EP6) liefert asynchrone Ereignisse, ohne Host-Request:

```
type  = EV_GPIO_IRQ | EV_UART_ERROR | EV_CAN_RX | EV_CAN_ERROR | EV_OTA_PROGRESS | EV_TIMESTAMP_SYNC | EV_HEARTBEAT
ts_us = u64  (MCU-Timer seit Boot)
len   = u16
body  = variabel
```

`EV_TIMESTAMP_SYNC` wird 1 Hz vom MCU gesendet und enthält den aktuellen MCU-Zeitstempel; der Host korreliert ihn mit `CLOCK_MONOTONIC_RAW` und baut ein Kalman-Filter oder lineare Regression über mehrere Samples, um ein stabiles Mapping zu halten. Latenzschwankung auf USB-FS liegt bei ±125 µs; das Mapping ist daher auf ±200 µs präzise, was für Sensor-Timestamps ausreicht.

### 5.5 Subsystem-Opcodes (Kurzform)

**I²C (subsys=1)**: `OP_CONFIG` (Instanz, SDA, SCL, Freq, Pull-Up), `OP_XFER` (7/10-bit addr, flags, write-buf, read-len), `OP_PROBE` (ACK-Sweep), `OP_SMBUS_BLOCK`.

**SPI (subsys=2)**: `OP_CONFIG` (Mode 0..3, MSB/LSB, Bits/Word, Freq, Pins), `OP_XFER` (CS-Gpio, TX+RX-Länge, chip_select-hold), `OP_CS_ASSERT/RELEASE` (manuelles CS für segmentierte Transaktionen).

**GPIO (subsys=3)**: `OP_CONFIG_PIN` (Mode, Pull, Drive), `OP_SET` (bitmap), `OP_GET`, `OP_IRQ_ENABLE` (edge, both, level, + debounce-us), `OP_IRQ_ACK`.

**PWM (subsys=4)**: `OP_CONFIG_CHANNEL` (gpio, freq_hz, period_ns), `OP_SET_DUTY` (duty_ns), `OP_ENABLE/DISABLE`.

**ADC (subsys=5)**: `OP_CONFIG` (kanal, sps, avg), `OP_READ_ONCE`, `OP_START_STREAM` (Ring via Bulk-IN in Events), `OP_STOP_STREAM`.

**CAN (subsys=6)**: `OP_CONFIG` (bitrate, samplepoint, sjw, tx_pin, rx_pin), `OP_OPEN`, `OP_CLOSE`, `OP_SEND_FRAME` (11/29-bit id, dlc, data[0..8], flags), RX läuft rein über Events.

**SYS (subsys=0)**: `OP_GET_CAPS`, `OP_ECHO`, `OP_RESET`, `OP_REBOOT_BOOTSEL` (siehe §10), `OP_GET_UPTIME`, `OP_GET_VBUS`, `OP_SET_LED`.

---

## 6. Nativ-Peripherien (pico-sdk-Nutzung)

### 6.1 I²C

Die SDK-API (`hardware/i2c.h`) reicht; entscheidend ist der **DMA-basierte Transfer** für längere Blöcke, um Core0 nicht zu blockieren:

```c
static int i2c_xfer_dma(uint bus, uint8_t addr, const uint8_t *tx, size_t txlen,
                        uint8_t *rx, size_t rxlen, uint32_t timeout_us) {
    i2c_inst_t *i2c = (bus == 0) ? i2c0 : i2c1;

    // Write-Phase (blockierend reicht bei ≤16 Byte; darüber DMA)
    if (txlen) {
        int n = i2c_write_timeout_us(i2c, addr, tx, txlen, rxlen > 0, timeout_us);
        if (n < 0) return n;
    }
    // Read-Phase
    if (rxlen) {
        int n = i2c_read_timeout_us(i2c, addr, rx, rxlen, false, timeout_us);
        if (n < 0) return n;
    }
    return 0;
}
```

Für längere Reads (z. B. EEPROM-Dump) wird auf die DREQ-gesteuerte DMA umgeschaltet (`DREQ_I2C0_RX`). Die Opcode-Flags (`OP_XFER.flags`) erlauben dem Host explizit „Burst", was intern den DMA-Pfad wählt.

### 6.2 SPI

`hardware/spi.h` in Kombination mit DMA — sowohl TX als auch RX werden mit eigenen Kanälen programmiert. Der Chip-Select darf ruhig per GPIO gesteuert werden (hält 4 weitere SPI-Geräte möglich), was gleichzeitig CS-Hold-Time-Tricks erlaubt:

```c
static int spi_xfer_dma(uint inst, uint8_t cs_gpio, const void *tx, void *rx, size_t n) {
    spi_inst_t *spi = (inst == 0) ? spi0 : spi1;
    int dma_tx = dma_claim_unused_channel(true);
    int dma_rx = dma_claim_unused_channel(true);
    dma_channel_config c_tx = dma_channel_get_default_config(dma_tx);
    channel_config_set_dreq(&c_tx, spi_get_dreq(spi, true));
    dma_channel_configure(dma_tx, &c_tx, &spi_get_hw(spi)->dr, tx, n, false);
    dma_channel_config c_rx = dma_channel_get_default_config(dma_rx);
    channel_config_set_dreq(&c_rx, spi_get_dreq(spi, false));
    channel_config_set_write_increment(&c_rx, true);
    channel_config_set_read_increment(&c_rx, false);
    dma_channel_configure(dma_rx, &c_rx, rx, &spi_get_hw(spi)->dr, n, false);

    gpio_put(cs_gpio, 0);
    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    dma_channel_wait_for_finish_blocking(dma_rx);
    gpio_put(cs_gpio, 1);

    dma_channel_unclaim(dma_tx); dma_channel_unclaim(dma_rx);
    return 0;
}
```

### 6.3 UART (HW)

`hardware/uart.h` + DMA für RX-Dauerbetrieb mit Ring-Puffer. RS485 erfordert, dass der DE-Pin *genau* mit dem ersten TX-Startbit kommt und *nach* dem letzten Stoppbit wieder fällt. Das ist mit der HW-UART alleine nicht präzise genug — bei den zwei RS485-fähigen Instanzen wird der DE-Pin daher **per PIO-Co-SM** getoggelt (siehe §7).

### 6.4 PWM

`hardware/pwm.h`. Jede GPIO kann an einen PWM-Slice gebunden werden. Für Frequenzen ≤ 1 kHz über den normalen Teiler; für präzise hohe Auflösung bei niedrigen Frequenzen über Phase-Correct-Mode. Die Host-Opcodes reichen `freq_hz` und `duty_ns`; die Firmware rechnet die Slice-Config selbst aus.

### 6.5 ADC

RP2350-ADC ist ein SAR-ADC, effektiv ~10–11 bit ENOB, max ~500 kSPS. Streaming-Modus via Round-Robin + DMA in einen Ringpuffer, der periodisch als Events zum Host gepumpt wird. Für 48 kHz Einkanal ist das mühelos; für Mehrkanal-Streaming muss man die USB-FS-Bandbreite im Auge behalten (500 kSPS × 2 Byte = 1 MB/s = bereits das komplette USB-Budget).

### 6.6 GPIO

`hardware/gpio.h`. IRQ-Callback registriert, der in eine Event-Queue schreibt; Core1 holt sie heraus und gibt sie als `EV_GPIO_IRQ` auf den Interrupt-EP. Debouncing geschieht firmwareseitig per Timer (Parameter vom Host).

### 6.7 Pin-Broker

Eine zentrale Instanz, die Pins zuweist. Jeder Subsystem-Aktivierungsbefehl ruft `pin_broker_claim(pin, subsys_id, mode)`. Konflikte werden mit `-EBUSY` beantwortet. Freigabe beim Deaktivieren oder Reset. Der Broker serialisiert die Pin-Tabelle auch für den Capability-Descriptor — Single Source of Truth.

---

## 7. PIO-basierte Soft-Peripherien

### 7.1 PIO-Grundlagen (kompakt)

- 3 PIO-Blöcke (RP2350), je 4 State Machines, je 32 Instruktionen Programmspeicher.
- Jede SM: eigener Clock-Divider, zwei FIFOs à 4 Wörter (joinbar zu 8), OSR und ISR je 32 Bit, Pin-Mapping für In/Out/SideSet.
- 9 Instruktionen — das Set ist klein, aber extrem leistungsfähig dank Side-Set und automatischem Pull/Push.
- IRQ-Flags 0..7 für SM-zu-CPU-Signalisierung (und SM-zu-SM).
- DMA-Anbindung via DREQ (`DREQ_PIOx_TXy`, `DREQ_PIOx_RXy`).

Die **Applet-Abstraktion** in unserer Firmware:

```c
typedef struct {
    uint8_t  id;
    const pio_program_t *program;
    uint8_t  n_sm_required;
    int  (*init)(const struct applet_desc *d, const uint8_t pins[], void **ctx);
    void (*deinit)(void *ctx);
    int  (*ioctl)(void *ctx, uint32_t op, void *arg);
} applet_desc_t;
```

Applets werden beim Host-Befehl `APPLET_INSTANTIATE(id, pins[])` vom Pin-Broker bestätigt, in einem freien PIO-Block geladen (`pio_add_program`), initialisiert und bekommen einen `instance_handle` zurück, über den Datenoperationen laufen.

### 7.2 CAN 2.0B über PIO — Referenzimplementierung mit can2040

**can2040** (K. O'Connor, MIT-Lizenz, https://github.com/KevinOConnor/can2040) ist die produktiv genutzte, von SocketCAN-Stacks (Klipper, einige Foundry-Builds) getestete Umsetzung eines CAN-2.0-Controllers rein in PIO + ARM-Code auf RP2040/RP2350. Wir binden sie ein, statt die Bit-Timing-Maschine neu zu schreiben.

**Was can2040 macht (Architekturzusammenfassung, zum Verständnis für den Entwickler):**

- belegt 1 PIO-Block vollständig mit 3 State Machines:
  - *SM „sync"*: hängt an RX und erzeugt bei jedem Edge (recessive→dominant) einen PIO-IRQ, mit dem die anderen SMs ihre Bit-Timing-Referenz resynchronisieren. Damit bleibt das Sampling auch bei Oszillatorabweichungen der Sender innerhalb der CAN-Toleranz.
  - *SM „rx"*: sampelt RX an einem einstellbaren Samplepunkt (typ. 75–87,5 % einer Bitzeit) und pusht 32-Bit-Wörter in den RX-FIFO.
  - *SM „tx"*: shiftet den vom CPU vorbereiteten Bitstream (inkl. Stuff-Bits) auf den TX-Pin, mit demselben Takt wie SM „rx". Arbitrierung: SM „tx" liest RX zurück und meldet dem CPU per IRQ, wenn ein dominantes Bit empfangen wurde, während recessive gesendet wurde (= arbitrierung verloren).
- CPU-Anteil: Bit-Stuffing/Unstuffing, CRC-15-Berechnung (Tabelle), Frame-Serialisierung/-Deserialisierung, ACK-Slot-Handling, Error-State-Automat (active/passive/bus-off), RX/TX-Queues.
- Timing: der PIO-Clock-Divider wird aus `sys_clk / (bitrate × ticks_per_bit)` gerechnet, wobei can2040 intern mit 32 Ticks pro Bit arbeitet.

**Glue-Code im rpbridge-Projekt** (`src/pio/can2040_glue.c`):

```c
#include "can2040.h"
#include "hardware/irq.h"
#include "core1/queues.h"

static struct can2040 cbus;

// Callback aus can2040 — wird im PIO-IRQ-Kontext aufgerufen
static void can2040_cb(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
    switch (notify) {
    case CAN2040_NOTIFY_RX:
        // Frame in Event-Queue für Core1 schieben
        core1_queue_push_can_rx(msg);
        break;
    case CAN2040_NOTIFY_TX:
        core1_queue_push_can_txdone(msg);
        break;
    case CAN2040_NOTIFY_ERROR:
        core1_queue_push_can_error(cd->errors);
        break;
    }
}

static void __isr can2040_pio0_irq(void) {
    can2040_pio_irq_handler(&cbus);
}

int can_start(uint tx_gpio, uint rx_gpio, uint32_t bitrate) {
    const uint pio_num = 0;      // PIO0 voll für CAN
    const uint sys_clock = clock_get_hz(clk_sys);

    can2040_setup(&cbus, pio_num);
    can2040_callback_config(&cbus, can2040_cb);

    irq_set_exclusive_handler(PIO0_IRQ_0, can2040_pio0_irq);
    irq_set_priority(PIO0_IRQ_0, 0);        // höchste Priorität
    irq_set_enabled(PIO0_IRQ_0, true);

    can2040_start(&cbus, sys_clock, bitrate, rx_gpio, tx_gpio);
    return 0;
}

int can_send(const struct can2040_msg *msg) {
    return can2040_transmit(&cbus, msg);
}

void can_stop(void) {
    can2040_stop(&cbus);
    irq_set_enabled(PIO0_IRQ_0, false);
}
```

**Host-Sichtbarkeit**: der rpbridge-Kerneltreiber registriert für den CAN-Subsys ein **SocketCAN-Netzwerkdevice** (`net_device` mit `can_priv`). TX-Frames vom SocketCAN gelangen als `OP_SEND_FRAME` zur Firmware; RX-Frames kommen als `EV_CAN_RX` zurück und werden in `netif_rx()` eingekippt. Bus-off / error-passive werden über `can_change_state()` im Treiber abgebildet.

**Bitraten-Matrix** (validiert mit can2040):
- 125 kbit/s: sample 87,5 %, sjw 1
- 250 kbit/s: sample 87,5 %, sjw 1
- 500 kbit/s: sample 80 %, sjw 1
- 1 Mbit/s: sample 75 %, sjw 1 — noch stabil, aber System-Taktfestigkeit und saubere Leitung vorausgesetzt

**Grenzen**: CAN-FD wird von can2040 nicht unterstützt. Wer FD braucht, greift zu einem externen MCP2517FD o. ä. über SPI.

### 7.3 Weitere PIO-Applets

**UART-TX/RX (`uart_pio.pio`)**: Standardbeispiel aus dem pico-examples, leicht erweitert um `break`-Erkennung und RX-Error-Signalisierung. Ein Applet belegt 2 SMs (TX+RX), 8+14 Instruktionen. Drei parallele Instanzen sind in einem PIO-Block realisierbar, wenn der Befehlsspeicher reicht — andernfalls auf zwei Blöcke verteilen.

**I²S-TX (`i2s_pio.pio`)**: produziert BCK, LRCK und SD aus einem von DMA gefüllten Audio-Puffer. Mit Side-Set für BCK und LRCK lässt sich das in ~6 Instruktionen realisieren. Häufig genutztes Muster; Beispiele im pico-extras. Linux-seitig wird dieses Applet später an `snd-usb-audio` via UAC2-Interface gekoppelt — für dieses Dokument: out of scope, aber hardware- und pinseitig vorgesehen.

**1-Wire (`onewire.pio`)**: Reset-Puls + Slots bitweise, ~14 Instruktionen. Abstrahiert als I²C-ähnlicher Subsys-Zweig (zur Not als Untermenge des GPIO-Subsys mit Custom-Ioctl; ein eigener Subsys-Code `SUBSYS_W1` ist sauberer und erlaubt Linux-w1-Anbindung später).

**WS2812 / RGB-LED**: Standardbeispiel. Nützlich als „Status-LED-Leiste" am Dongle selbst.

**RS485-DE-Toggler**: 1 SM, die den DE-Pin auf TX-Aktivität des HW-UART synchronisiert. Das PIO-Programm lauscht auf den TX-Pin (oder ein internes Aktivsignal) und hält DE für ein konfigurierbares Guard-Intervall nach dem letzten Bit.

### 7.4 Applet-Orchestrierung

Die Capability-Tabelle listet statisch alle kompilierten Applets. Zur Laufzeit können Applets nur instantiiert werden, wenn ein PIO-Block genug freie SMs und Instruktionsspeicher hat. Der Applet-Manager führt eine einfache Bin-Packing-Heuristik: gleichartige Applets (viele UARTs) werden in denselben Block gelegt, fremdartige auf einen anderen verteilt. can2040 belegt immer exklusiv PIO0 — der Manager behandelt das als Sonderfall (`APPLET_CAN` reserviert den gesamten Block).

---

## 8. Linux-Kerneltreiber

### 8.1 Treiberfamilie nach MFD-Vorbild

Die idiomatische Umsetzung ist ein **MFD-Kerntreiber** (`drivers/mfd/rpbridge.c`), der:

1. das USB-Device beansprucht,
2. den Capability-Descriptor holt und parst,
3. Bulk-TX/RX-URBs initialisiert und einen Transaktions-Demultiplexer aufsetzt,
4. für jedes in der Capability aufgeführte Subsystem ein `platform_device` per `mfd_add_devices()` anlegt.

Kernel-Vorbilder:
- `drivers/mfd/dln2.c` — fast identische Topologie, exzellenter Startpunkt. **Den Hotplug-Pfad (remove → orderly disposal aller Child-Devices) 1:1 übernehmen.**
- `drivers/mfd/cros_ec*.c` — vorbildlich in Sachen DT-Overlay-Integration.

### 8.2 Dateiaufteilung

```
drivers/
  mfd/
    rpbridge-core.c         # usb_driver, cmd framing, endpoint mgmt
    rpbridge-cap.c          # Capability-Parser
    rpbridge-overlay.c      # DT-Fragment-Applikation
    rpbridge.h              # öffentliche API für Child-Treiber
  gpio/
    gpio-rpbridge.c         # gpio_chip + irq_chip
  i2c/busses/
    i2c-rpbridge.c          # i2c_adapter
  spi/
    spi-rpbridge.c          # spi_controller
  pwm/
    pwm-rpbridge.c          # pwm_chip
  iio/adc/
    rpbridge-adc.c          # iio_dev
  net/can/usb/
    rpbridge-can.c          # net_device (SocketCAN)
  tty/serial/
    (CDC-ACM via usbserial — kein eigener Treiber nötig)
```

### 8.3 MFD-Kernskelett

```c
// drivers/mfd/rpbridge-core.c (gekürzt)
struct rpbridge {
    struct usb_device *udev;
    struct usb_interface *intf;
    u8 ep_bulk_in, ep_bulk_out, ep_intr_in;
    struct urb *in_urb, *intr_urb;

    struct rpbridge_cap caps;          // geparster Capability-Descriptor
    struct ida xid_ida;                // Transaktions-IDs
    struct list_head pending;          // in-flight Requests
    spinlock_t lock;

    struct completion caps_ready;
};

static int rpbridge_send_sync(struct rpbridge *r, u8 subsys, u8 op,
                              const void *tx, u16 txlen,
                              void *rx, u16 *rxlen, unsigned long tmo_ms) {
    struct rpbridge_req req;
    int xid = ida_alloc_range(&r->xid_ida, 1, 0xFFFE, GFP_KERNEL);
    if (xid < 0) return xid;

    req.xid = xid; req.rx_buf = rx; req.rx_max = rxlen ? *rxlen : 0;
    init_completion(&req.done);
    spin_lock(&r->lock); list_add(&req.node, &r->pending); spin_unlock(&r->lock);

    rpbridge_xmit_header(r, subsys, op, RPBR_FL_EXPECT_RESP, xid, tx, txlen);

    if (!wait_for_completion_timeout(&req.done, msecs_to_jiffies(tmo_ms))) {
        spin_lock(&r->lock); list_del(&req.node); spin_unlock(&r->lock);
        ida_free(&r->xid_ida, xid);
        return -ETIMEDOUT;
    }
    if (rxlen) *rxlen = req.rx_len;
    ida_free(&r->xid_ida, xid);
    return req.status;
}

static int rpbridge_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    struct rpbridge *r = devm_kzalloc(&intf->dev, sizeof(*r), GFP_KERNEL);
    /* ... EP-Discovery, URB-Alloc, Capability-Fetch ... */
    rpbridge_apply_dt_overlay(r);             // §8.8

    struct mfd_cell cells[RPBR_MAX_CELLS];
    int n = rpbridge_build_mfd_cells(r, cells);
    return mfd_add_devices(&intf->dev, PLATFORM_DEVID_AUTO, cells, n, NULL, 0, NULL);
}

static void rpbridge_disconnect(struct usb_interface *intf) {
    struct rpbridge *r = usb_get_intfdata(intf);
    mfd_remove_devices(&intf->dev);
    usb_kill_urb(r->in_urb); usb_kill_urb(r->intr_urb);
    /* pending requests -ENODEV completen */
}
```

### 8.4 GPIO-Child

```c
static int rpbr_gpio_get_direction(struct gpio_chip *gc, unsigned off) { ... }
static int rpbr_gpio_direction_input(struct gpio_chip *gc, unsigned off) {
    struct rpbr_gpio *g = gpiochip_get_data(gc);
    u8 buf[3] = { off, GPIO_MODE_INPUT, /*pull*/ 0 };
    return rpbridge_send_sync(g->bridge, SUBSYS_GPIO, OP_CONFIG_PIN, buf, 3, NULL, NULL, 100);
}
static void rpbr_gpio_set(struct gpio_chip *gc, unsigned off, int val) { ... }

// IRQ-Chip
static void rpbr_gpio_irq_enable(struct irq_data *d) {
    struct rpbr_gpio *g = irq_data_get_irq_chip_data(d);
    u8 buf[3] = { d->hwirq, g->trigger[d->hwirq], g->debounce_us / 100 };
    rpbridge_send_sync(g->bridge, SUBSYS_GPIO, OP_IRQ_ENABLE, buf, 3, NULL, NULL, 100);
}

// Aufruf aus dem MFD-Event-Pfad, wenn EV_GPIO_IRQ ankommt:
void rpbr_gpio_event(struct rpbr_gpio *g, u8 pin) {
    generic_handle_irq(irq_find_mapping(g->gc.irq.domain, pin));
}
```

Der Rest (probe/remove, `gpiochip_add_data`, `gpiochip_irqchip_add_nested`) folgt dem Standardmuster aus `gpio-pca953x` oder `gpio-dln2`.

### 8.5 I²C-Child

```c
static int rpbr_i2c_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int num) {
    struct rpbr_i2c *ic = i2c_get_adapdata(a);
    for (int i = 0; i < num; i++) {
        struct rpbr_i2c_req tx = {
            .addr = msgs[i].addr,
            .flags = (msgs[i].flags & I2C_M_RD) ? RPBR_I2C_READ : 0,
            .len = msgs[i].len,
        };
        u16 rxlen = msgs[i].len;
        int err = rpbridge_send_sync(ic->bridge, SUBSYS_I2C, OP_XFER,
                                     &tx, sizeof(tx) + (tx.flags ? 0 : msgs[i].len),
                                     msgs[i].buf, &rxlen, 1000);
        if (err) return err;
    }
    return num;
}
static const struct i2c_algorithm rpbr_i2c_algo = {
    .master_xfer = rpbr_i2c_xfer,
    .functionality = rpbr_i2c_func,
};
```

`i2c_add_adapter()` registriert den Adapter; ab diesem Moment ist er mit `i2cdetect -y N` sichtbar und `i2c-dev` stellt `/dev/i2c-N` bereit.

### 8.6 SPI-, PWM-, ADC-Child (Kurzüberblick)

- **SPI**: `spi_controller` mit `.transfer_one = rpbr_spi_transfer_one`. Für `/dev/spidev*` ist das genug — das `spidev`-Modul bindet automatisch an Kinder des Controllers, wenn sie als Child-Device registriert werden (oder per modaliter DT-Overlay).
- **PWM**: `pwm_chip` mit `.apply = rpbr_pwm_apply`. Kanalzahl aus Capability.
- **ADC**: `iio_dev` mit `.read_raw`, `.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE)`. Streaming-Modus über `iio_triggered_buffer_setup()`.

### 8.7 CAN-Child

```c
static const struct net_device_ops rpbr_can_netdev_ops = {
    .ndo_open = rpbr_can_open,
    .ndo_stop = rpbr_can_close,
    .ndo_start_xmit = rpbr_can_xmit,
    .ndo_change_mtu = can_change_mtu,
};

static netdev_tx_t rpbr_can_xmit(struct sk_buff *skb, struct net_device *dev) {
    struct rpbr_can *cp = netdev_priv(dev);
    struct can_frame *cf = (struct can_frame *)skb->data;
    struct rpbr_can_tx tx = {
        .id = cf->can_id, .dlc = cf->can_dlc,
    };
    memcpy(tx.data, cf->data, cf->can_dlc);
    rpbridge_send_async(cp->bridge, SUBSYS_CAN, OP_SEND_FRAME, &tx, sizeof(tx));
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

// RX-Pfad: vom MFD-Event dispatcher
void rpbr_can_rx_event(struct rpbr_can *cp, const struct rpbr_can_rx *rx) {
    struct sk_buff *skb;
    struct can_frame *cf;
    skb = alloc_can_skb(cp->netdev, &cf);
    cf->can_id = rx->id; cf->can_dlc = rx->dlc;
    memcpy(cf->data, rx->data, rx->dlc);
    netif_rx(skb);
}
```

Registrierung mit `register_candev()` — `can0` erscheint in `ip link`.

### 8.8 Device-Tree-Overlay-Auto-Provisioning

**Idee**: Das Firmware-Dongle weiß, *welche Sensoren/Chips fest auf seinem eigenen PCB* an welche Adresse gehängt sind. Statt den Nutzer zu zwingen, manuell `echo lm75 0x48 > /sys/bus/i2c/devices/i2c-N/new_device` zu tippen, liefert die Capability ein **vorgefertigtes DT-Fragment** mit, das der Treiber zur Laufzeit appliziert.

**Fragment-Inhalt (Beispiel-Quelle `.dts` wird beim Firmware-Bauen zu `.dtb` kompiliert und als BLOB ins Firmware-Image gelinkt):**

```dts
/dts-v1/;
/plugin/;
/ {
    fragment@0 {
        target-path = "/rpbridge-i2c-0";
        __overlay__ {
            tmp117@48 {
                compatible = "ti,tmp117";
                reg = <0x48>;
            };
            eeprom@50 {
                compatible = "atmel,24c32";
                reg = <0x50>;
                pagesize = <32>;
            };
        };
    };
};
```

Der MFD-Treiber erstellt vor dem `mfd_add_devices()`-Aufruf **virtuelle DT-Nodes** unter `/rpbridge-i2c-N`, `/rpbridge-spi-N` usw., damit die Fragment-target-paths existieren. Dann:

```c
static int rpbridge_apply_dt_overlay(struct rpbridge *r) {
    int ovcs_id = 0;
    if (!r->caps.dt_frag_len) return 0;
    return of_overlay_fdt_apply(r->caps.dt_frag, r->caps.dt_frag_len, &ovcs_id, NULL);
}
```

Der `ovcs_id` wird im rpbridge-Kontext gespeichert und bei `disconnect()` mit `of_overlay_remove()` entsorgt, damit beim Wiederanstecken keine doppelten Nodes zurückbleiben.

**Vorsicht**: `of_overlay_fdt_apply` verlangt `CONFIG_OF_OVERLAY=y`. Das ist auf Desktop-Distributionen oft *nicht* aktiv. Fallback: der Treiber ruft stattdessen `i2c_new_client_device()` / `spi_new_device()` direkt auf Basis einer kleineren, eigenen Beschreibungsstruktur. Praxisempfehlung: **beides implementieren**, DT-Overlay als Premium-Pfad, direktes-`i2c_new_client_device` als universeller Fallback.

### 8.9 Mainline-Strategie

- Für die erste Version: out-of-tree-Modul mit DKMS-Paket.
- Nach Stabilisierung: einreichen in folgender Reihenfolge (das entspricht dem dln2-Vorgang, der mainline akzeptiert wurde): MFD-Kern + GPIO zuerst, danach I²C, SPI, PWM, ADC, CAN. Netlink/DT-Overlay-Auto-Provisioning erst einreichen, wenn die Kernteile drin sind — das ist der politisch heikelste Teil und verlangt Diskussion auf der LKML.

---

## 9. Beispielanwendungen

### 9.1 Temperatursensor BME280 am I²C

```bash
# Voraussetzung: Bridge gesteckt, Treiber geladen
$ ls /dev/i2c-*
/dev/i2c-7
$ i2cdetect -y 7
     0  1  2  3 ... 76 77
00:    -- -- -- ...
70: -- -- -- -- -- -- 76 --     <-- BME280 auf 0x76
$ sudo modprobe bmp280_i2c
$ echo bme280 0x76 | sudo tee /sys/bus/i2c/devices/i2c-7/new_device
$ cat /sys/bus/iio/devices/iio:device0/in_temp_input
24310      # 24,310 °C
```

Mit DT-Overlay-Auto-Provisioning entfällt der `new_device`-Schritt.

### 9.2 SocketCAN-Echo mit can-utils

```bash
$ ip link
...
6: can0: <NOARP,ECHO> mtu 16 qdisc noop state DOWN ...
$ sudo ip link set can0 type can bitrate 500000
$ sudo ip link set can0 up
$ candump can0 &
$ cansend can0 123#DEADBEEF
can0  123   [4]  DE AD BE EF
```

Kein Unterschied zu einer nativen CAN-Karte — genau der Punkt.

### 9.3 GPIO mit libgpiod

```bash
$ gpiodetect
gpiochip0 [rpbridge] (24 lines)
$ gpioset gpiochip0 5=1
$ gpiomon -r -f gpiochip0 7     # reagiert auf fallende Flanke
event:  FALLING EDGE offset: 7 timestamp: [1713784923.812 341 000]
```

### 9.4 SPI-Flash auslesen

```bash
$ flashrom -p linux_spi:dev=/dev/spidev0.0,spispeed=4000 -r dump.bin
```

`flashrom` spricht direkt `spidev`; unsere Bridge ist transparent.

### 9.5 ADC-Stream via IIO

```bash
$ iio_info | grep rpbridge
$ iio_readdev -b 256 iio:device0 voltage0 > samples.raw
```

---

## 10. OTA via BOOTSEL / UF2

### 10.1 Mechanismus

Der RP2350-Mask-ROM-Bootloader ist unzerstörbar und wird per Hardware-Pin (BOOTSEL) beim Power-On aktiviert. Er enumeriert dann als USB-Mass-Storage-Gerät „RP2350" und akzeptiert das **UF2-Format**; alternativ spricht ihn das `PICOBOOT`-Vendor-Protokoll an (das `picotool` intern nutzt).

Unser Firmware-Pfad:

1. Host sendet `SUBSYS_SYS` / `OP_REBOOT_BOOTSEL`.
2. Die Firmware ruft:

```c
#include "pico/bootrom.h"
// alle USB-Aktivitäten einfrieren:
reset_usb_boot(0, 0);   // erstes Arg: GPIO-Maske Activity-LED, zweites: Optionen
```

3. Der Chip re-enumeriert nach ~200 ms als `RP2350`-Massenspeicher + `PICOBOOT`-Device.
4. Host führt aus:

```bash
picotool load -fxv rpbridge.uf2
# -f: bei Bedarf Reset nach Flashen
# -x: nach Flashen die neue Firmware ausführen
# -v: Verify
```

5. Chip startet die neue Firmware, re-enumeriert als rpbridge.

### 10.2 Host-Skript

```bash
#!/usr/bin/env bash
set -euo pipefail
FW=${1:-rpbridge.uf2}
DEV=${2:-/dev/rpbridge0}      # udev-symlink, siehe §10.4

echo "[*] Triggering BOOTSEL on $DEV ..."
rpbridge-ctl "$DEV" reboot-bootsel

echo "[*] Waiting for RP2350 BOOTSEL device ..."
for i in $(seq 1 50); do
    if picotool info >/dev/null 2>&1; then break; fi
    sleep 0.1
done

echo "[*] Flashing $FW ..."
picotool load -fxv "$FW"

echo "[*] Waiting for rpbridge re-enumeration ..."
for i in $(seq 1 50); do
    [[ -c "$DEV" ]] && { echo "[+] Back online."; exit 0; }
    sleep 0.1
done
echo "[-] rpbridge did not come back" >&2
exit 1
```

`rpbridge-ctl` ist ein kleines C-Tool im Userspace, das den Vendor-Control-Request `OP_REBOOT_BOOTSEL` absetzt — zwei Dutzend Zeilen libusb-Code.

### 10.3 Linux-Treiber-Hook

Im Treiber ist `REBOOT_BOOTSEL` kein User-ioctl, sondern ein eigenes sysfs-Attribut:

```
/sys/bus/usb/drivers/rpbridge/<dev>/reboot_bootsel
```

Schreiben von `1` triggert den Befehl. `udev` kann darüberhinaus eine Regel bereitstellen, die das neu erscheinende BOOTSEL-Device erkennt und automatisch ein vorkonfiguriertes UF2 flasht — ist aber optional.

### 10.4 udev-Regeln

```
# /etc/udev/rules.d/70-rpbridge.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="XXXX", \
    MODE="0660", GROUP="plugdev", SYMLINK+="rpbridge%n"

# BOOTSEL-Modus
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000f", \
    MODE="0660", GROUP="plugdev", TAG+="rpbridge-bootsel"
```

### 10.5 fwupd-Integration (optional, später)

Langfristig lohnt sich ein **fwupd**-Plugin, das Updates via LVFS verteilt. Das kostet ~500 Zeilen C und einen Metadaten-Eintrag — deutlich weniger Reibung als ein eigenes Tool, und Enterprise-Nutzer akzeptieren das reibungslos.

---

## 11. Entwicklungs-Roadmap (empfohlene Milestones)

| M | Inhalt                                                                                      | Abschlusskriterium                              |
|---|---------------------------------------------------------------------------------------------|-------------------------------------------------|
| 1 | Pico-SDK-Skelett, TinyUSB-Vendor-Interface, `OP_ECHO`                                       | Host-Tool kann 1 kB Daten round-trippen         |
| 2 | Capability-Descriptor (minimal: nur GPIO + I²C), Pin-Broker, HW-I²C                         | `i2cdetect` findet Test-Sensor                  |
| 3 | MFD-Kerneltreiber + `gpio-rpbridge` + `i2c-rpbridge` nach dln2-Vorbild                      | libgpiod + BME280-Read ohne Custom-Userspace    |
| 4 | SPI, PWM, ADC                                                                               | spidev-test, IIO-ADC, PWM-LED-Dimmen            |
| 5 | CDC-ACM-UARTs + RS485-DE-Applet                                                             | `minicom` auf /dev/ttyACM0 mit 921 600 Bd       |
| 6 | CAN-over-PIO (can2040-Integration) + `rpbridge-can` Netdev                                  | `cansend`/`candump` gegen echte CAN-Gegenstelle |
| 7 | Event-Kanal + GPIO-IRQs + Timestamp-Sync                                                    | `gpiomon` triggert korrekt                      |
| 8 | DT-Overlay-Auto-Provisioning + udev-Regeln + Installer                                      | Stecken → Sensoren erscheinen automatisch        |
| 9 | BOOTSEL-OTA-Tool + Update-Dokumentation                                                     | `rpbridge-update` End-to-End                    |
| 10| PIO-Applets ausbauen (extra UARTs, 1-Wire, WS2812)                                          | optional / laufend                              |

---

## 12. Teststrategie

**Firmware** (auf dem MCU):
- `pico-sdk` Unit-Tests über `pico_host_sdl` für Logik (CRC, Framing, Capability-Serializer) — laufen auf x86.
- Loopback-Tests im Gerät: TX und RX eines Busses auf Pin-Ebene kurzschließen und per Self-Test durchlaufen (`SUBSYS_SYS/OP_SELFTEST`).
- can2040 bringt eine Testsuite mit — übernehmen.

**Host-Treiber**:
- `kunit` für Capability-Parser und Framing.
- Integrations-Tests mit `umockdev`: USB-Paketsequenzen aufzeichnen/replayen, Treiber gegen Aufzeichnung laufen lassen.
- End-to-End-Gerät-im-Loop mit realem RP2350, Jenkins-Runner mit angeschlossener Bridge und Referenzperipherie.

**Bus-Compliance**:
- I²C: Scope-Aufzeichnung rise-time, ACK-Timing, Clock-Stretching-Handhabe.
- SPI: Mode 0–3, CS-Hold-Time.
- CAN: Conformance mit externem CAN-Analyzer (PCAN-USB, Vector). Bus-Last bis 80 % testen.
- UART: 921 600 Bd für 24 h ohne Overrun.
- OTA: 1000×-Flash-Reboot-Zyklus (SMT-Flash-Endurance reicht; QSPI-Flash hat ≥100k Cycles).

**Fuzzing**:
- `syzkaller`-ähnlich über das Vendor-Endpoint — Protokoll muss gegen Müll immun sein.

---

## 13. Referenzen

- Raspberry Pi: *RP2350 Datasheet*; *Pico SDK Documentation* (https://datasheets.raspberrypi.com)
- TinyUSB: https://github.com/hathach/tinyusb
- can2040 (Kevin O'Connor): https://github.com/KevinOConnor/can2040
- Linux `drivers/mfd/dln2.c` — kanonisches Vorbild für Bus-Bridge-Treiber
- Linux `drivers/mfd/cros_ec*.c` — Vorbild für DT-Overlay-Integration
- Linux `Documentation/driver-api/usb/*.rst`
- Linux `Documentation/networking/can.rst` — SocketCAN-API
- `libgpiod`, `i2c-tools`, `can-utils`, `iio-utils`
- UF2-Format: https://github.com/microsoft/uf2
- pid.codes (VID/PID-Allokation für Open-Source): https://pid.codes

---

*Ende des Architekturdokuments.*
