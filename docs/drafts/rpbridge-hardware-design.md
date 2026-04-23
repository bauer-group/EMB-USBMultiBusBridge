# rpbridge — Hardware-Design

*USB-bus-powered Multi-Bus-Developer-Bridge auf Basis RP2354B. Schweizer Taschenmesser für Embedded-Peripherie am Entwickler-Desktop.*

**Dokument-Revision:** V2 (Draft) · Konsolidierte Revisionsrunde: 2-Lagen-PCB, vereinfachte Statusanzeige (1× RGB), reale Jumper statt Lötjumper, schlanker 5-poliger Debug-Header, PTC im Eingangskreis mit Rückspeiseschutz, keine USB-PD, keine Strom-Wahl-Jumper.

**Zielbauteil**: Raspberry Pi Foundation **RP2354B** (RP2350B-Die im QFN-80 mit **2 MB stacked QSPI-Flash** im selben Package). Das spart ein externes Flash-IC, reduziert BOM-Teile um 1, Platinenfläche und eliminiert die hochfrequenten QSPI-Routings komplett.

---

## 1. Designziele

1. **Bus-powered über USB** (5 V, ≤ 500 mA spec, praktisch 900 mA+ an jedem modernen Host) — kein Netzteil, keine Polbarkeitssorgen, Plug-and-Play am Laptop.
2. **Alle gewünschten Busse gleichzeitig verfügbar**: 2× I²C, 2× SPI, 3× UART (davon 1× mit wählbarem Physical Layer RS232/RS485/TTL via Shutdown-Logik), 1× CAN 2.0B (PIO, nur Transceiver), 8× GPIO (inkl. PWM/Interrupts), 4× ADC, 1-Wire.
3. **Professionelle Konnektoren** mit etablierten Pinouts — jeder Bus hat den „richtigen" Stecker für sein Ökosystem.
4. **Schutzbeschaltung** auf allen extern geführten Signalen (ESD, Überspannung, Verpolung) sowie **PTC-Rücksetz-Sicherung im USB-Eingangskreis** und **Schottky-Rückspeiseschutz** zum Host.
5. **Schlanke, klare Statusanzeige** über **eine einzelne diskrete RGB-LED** (3,3-V-direkt-GPIO-getrieben) plus eine diskrete grüne Power-LED. Keine LED-Strips, keine Level-Shifter, keine 5-V-LED-Last.
6. **Kompakt und kostenoptimiert**: **80 × 55 mm, 2-Lagen-PCB** (ausreichend für USB 2.0 Full-Speed und alle vorhandenen Bus-Frequenzen), Standard-Extrusionsgehäuse.
7. **Produktionsreif**: definierte BOM, Testpads, **5-poliger Debug-Header (SWDIO/SWCLK/GND + Debug-UART-TX/-RX)**, echte mechanische Jumper für Terminator-Zuschaltung.

**Abgrenzung**: keine galvanische Isolation (weder CAN noch RS485/RS232). Wenn isoliert, dann als Steckmodul-Erweiterung auf einem Daughter-Header — hält die Hauptplatine einfach und günstig. Keine USB-PD-Aushandlung, kein alternativer externer Power-Eingang; die Platine wird ausschließlich vom USB-Host versorgt.

---

## 2. System-Blockdiagramm

```
                    USB-C (Data + 5 V, classic USB 2.0 FS, kein PD)
                      │
           ┌──────────┴──────────┐
           │   ESD + Data-Choke  │   (USBLC6-2SC6)
           └──────────┬──────────┘
                      │
         VBUS ──▶ Schottky (Reverse-Feed-Block) ──▶ PTC 500 mA ──▶ Ferrit ──▶ +5V_Board
                                                                      │
                                                                      ├─▶ AP2112K-3.3 (LDO, bis 600 mA) ─▶ +3V3
                                                                      │
                                                                      └─▶ +5V am User-Header (direkt, keine weitere Sicherung)
                                                                          (gemeinsames GND mit externer Peripherie)

           +3V3 ─▶ RP2354B  (2× Cortex-M33, 520 KB SRAM, 3× PIO, 2 MB int. Flash)
                   │
                   ├── PIO0 ─▶ CAN-TX/RX ─▶ SN65HVD230 ─▶ DE-9
                   ├── UART0 (HW) ─▶ Phy-Bank (RS232 / RS485 / TTL, nur einer aktiv)
                   │                   │
                   │                   ├─▶ MAX3232 (SHDN) ─▶ DE-9
                   │                   ├─▶ SN65HVD72 (EN)  ─▶ Klemme (mit echtem Jumper-Terminator)
                   │                   └─▶ TTL-Header (direkt)
                   ├── UART1 (HW) ─▶ TTL-Header (Debug/Logging, 5-Pin-Debug-Port)
                   ├── PIO1 ─▶ weitere UARTs bei Bedarf
                   ├── I²C0/I²C1 ─▶ Qwiic + I²C-Header
                   ├── SPI0 ─▶ PMOD
                   ├── SPI1 ─▶ Breakout-Header
                   ├── ADC×4 ─▶ 2×10-User-Header
                   ├── GPIO/PWM ─▶ 2×10-User-Header
                   ├── PIO2 ─▶ 1-Wire / DE-Toggle / Reserve
                   └── 3× GPIO ─▶ diskrete RGB-LED (common-anode, 3,3 V, Strom via R je Kanal)
```

---

## 3. Power-Architektur

### 3.1 Grund-Regel

Die Platine bekommt **ausschließlich über den USB-C-Port** Energie. Keine Barrel-Jacks, keine Jumper zur Quellenwahl, keine Batterie. Angeschlossene externe Peripherie:

* teilt sich mit der Platine nur **GND** (Signalbus), **oder**
* wird zusätzlich vom **+5V-Rail** oder **+3V3-Rail** gespeist, das am User-Header herausgeführt ist — innerhalb der Strombudget-Grenzen (siehe §3.5).

Ein **Schottky-Diode zwischen USB-VBUS und dem Board-5V-Knoten** verhindert, dass ein versehentlich von der externen Peripherie eingespeistes 5V den USB-Host rückwärts speist. Eine **rücksetzbare PTC-Sicherung** direkt hinter dem Schottky schützt Host und Platine, sobald das Gesamtsystem mehr als ca. 500 mA zieht.

### 3.2 Rail-Übersicht

| Rail        | Quelle                          | Spannung | Max-Strom          | Versorgt                                   |
|-------------|---------------------------------|----------|--------------------|--------------------------------------------|
| VBUS (USB)  | USB-C-Host                      | ~5,0 V   | spec 500 mA, typ. 900 mA+ | Eingangsschutz, Board-5V-Knoten     |
| +5V_BOARD   | VBUS → Schottky → PTC → Ferrit  | ~4,7 V   | 1 A PTC-Trip       | LDO-Input, 5V-User-Header                  |
| +3V3        | AP2112K-3.3 LDO                 | 3,30 V   | 600 mA (LDO-spec, ~500 mA thermisch) | MCU, alle Transceiver, Pull-Ups, RGB-LED |
| +3V3_ANA    | Ferrit + RC aus +3V3            | 3,30 V   | < 20 mA            | ADC-AVDD + VREF                            |
| +1V1_Core   | interner SMPS im RP2354B        | 1,10 V   | —                  | MCU-Core (chipintern)                      |

Auslegung: Das USB-Rail liefert ≈900 mA an jedem halbwegs modernen Laptop. Wir planen **1 A nominell** ein und lösen mit der PTC bei ~500–600 mA aus, um innerhalb der USB-Spezifikation zu bleiben und den Host zu schützen. Der LDO-Ausgang ist auf **bis zu 600 mA** dimensioniert — der AP2112K-3.3TRG1 reicht dafür locker, weil die LED-Last jetzt bei unter 15 mA liegt (eine einzige RGB-LED @ 30 % Helligkeit, statt vier WS2812 @ 30 %).

### 3.3 Eingangsschutz (VBUS-Kette)

```
USB-C VBUS ──► Schottky MBR0520 ──► PTC 500 mA reset ──► Ferrit BLM18PG471SN1 ──► +5V_BOARD
                         │                                          │
                         │                                          │
            TVS SMAJ5.0A ┤ (bidir. zu GND)                Bulk 22 µF X5R + 100 nF
                         │                                          │
                        GND                                        GND
```

Designbegründung der Elemente:

* **Schottky MBR0520** (Vf ≈ 0,35 V @ 500 mA): blockt Rückspeisung vom Board-Knoten in den USB-Host. Der Spannungsabfall bedeutet, dass +5V_BOARD nominell 4,7 V ist — unkritisch für den LDO (Dropout 250 mV @ 500 mA) und für die Transceiver. Wenn der Spannungsabfall doch stört (z. B. für eine externe 5V-Peripherie, die zwingend ≥4,75 V braucht), kann stattdessen ein **Ideal-Diode-Controller mit P-MOSFET** bestückt werden (Footprint-kompatibler DNP: **LM66100** o. ä.).
* **PTC 500 mA reset (z. B. Bel Fuse MF-NSMF050X-2)**: schützt Host und Board bei Kurzschluss oder Überlast auf der externen Seite. Wird die Gesamtstromaufnahme längere Zeit > ~500 mA, erhöht die PTC ihren Widerstand drastisch und entkoppelt das Board. Nach Wegfall der Last kühlt sie ab und fängt sich selbst. **Keine Glassicherung, keine Pflege, keine User-Intervention.**
* **Ferrit BLM18PG471SN1** (470 Ω @ 100 MHz): bricht HF-Rückkopplung zwischen Kabel und Platine, wichtig für EMV.
* **TVS SMAJ5.0A** (bidirektional, Clamping-Spannung ≈ 9,2 V): clamped transiente Überspannungen aus defekten Kabeln oder heiß-eingesteckten Ports.

### 3.4 3,3-V-LDO

**AP2112K-3.3TRG1** (Diodes Inc.)

* I_out bis **600 mA**, Dropout 250 mV @ 500 mA → bei +5V_BOARD ≈ 4,7 V (nach Schottky) bleibt die Regelung stabil.
* Ruhestrom 55 µA, PSRR 55 dB @ 1 kHz.
* SOT-25, thermal pad, simpel zu bestücken.

Da die WS2812-Kette entfällt und nur noch **eine einzige diskrete RGB-LED @ max. 30 mA all-on** gespeist wird, hat der LDO jetzt reichlich Luft: Worst-Case-Last am +3V3-Rail ≈ 160–200 mA (MCU + Transceiver + Pull-Ups + LED). Der LDO bleibt im linearen Betrieb, thermisch entspannt (~200 mW Verlustleistung, Package hält ≥800 mW ohne externes Wärmemanagement).

Kondensatoren: 10 µF X5R am Input, 22 µF X5R am Output, 100 nF direkt am IC.

### 3.5 Strombudget (realistisch, alle Busse aktiv)

| Block                                        | typ.   | max. Annahme |
|----------------------------------------------|--------|--------------|
| RP2354B @ 150 MHz, beide Cores aktiv, alle PIO | 35 mA  | 70 mA        |
| SN65HVD230 CAN TX aktiv (dominant)           | 17 mA  | 70 mA        |
| SN65HVD72 RS485 TX aktiv                     | 8 mA   | 50 mA        |
| MAX3232 RS232 idle                           | 1 mA   | 15 mA        |
| I²C Pull-Ups (2× 2,2 kΩ × 2 Busse)          | ~4 mA  | 6 mA         |
| RGB-LED 1 Stk. @ 30 % all-white              | 3 mA   | 9 mA         |
| User-Header +3V3 Last                        | 20 mA  | 100 mA       |
| User-Header +5V Last                         | 20 mA  | 200 mA (PTC-limitiert) |
| **Summe (LDO-Ausgang, +3V3-Pfad)**           | ~90 mA | **~320 mA** (deutlich unter LDO-Grenze) |
| **Summe (USB-VBUS gesamt)**                  | ~130 mA | **~480 mA** (unter PTC-Trip) |

Unkritisch. Wir haben bewusst Spielraum zwischen Worst-Case-Board-Last (~280 mA) und der PTC-Trip-Schwelle (~500 mA), damit die externe User-Peripherie zusätzliche 150–200 mA ziehen kann, ohne das Sicherungselement auszulösen.

### 3.6 Analog-Supply

Der RP2350-ADC hat eine dedizierte `ADC_AVDD`-Pin-Gruppe:

```
+3V3 ──[Ferrit BLM15BB471SN1]──┬── +3V3_ANA ── ADC_AVDD
                                │
                               [100 nF || 1 µF || 10 nF]
                                │
                               GND_ANA (Sternpunkt zur Hauptmasse)
```

Optionaler, bestückbarer Footprint für **REF3330** (3,0 V ±0,15 %, 30 µA) an VREFP — DNP bleibt, solange AVDD = VREF ausreicht (übliche 12-Bit-Aufgaben).

---

## 4. USB-Interface

### 4.1 Connector & elektrischer Standard

**USB-C Receptacle, 16-Pin, Mid-Mount** — z. B. **GCT USB4085-GF-A**. Mechanisch robust, verpolungssicher, weit verbreitet.

Elektrisch: **USB 2.0 Full-Speed (12 Mbps)**. **Kein USB-PD**, keine Rolle-Aushandlung, keine alternativen Modi. Der USB-C-Stecker dient rein als physisches Interface — der Datenstack bleibt klassisches USB 2.0.

### 4.2 CC-Resistor-Konfiguration (Device/Sink)

```
CC1 ── 5,1 kΩ (1 %) ── GND
CC2 ── 5,1 kΩ (1 %) ── GND
```

Signalisiert dem Host eindeutig „Default USB 2.0 Sink, 500 mA @ 5 V". Fertig. Kein CC-Multiplexer, keine PD-Controller-ICs, keine Komplikation.

### 4.3 Signal-Integrität D+/D-

Auf einer **2-Lagen-Leiterplatte** ist eine präzise 90-Ω-Differenz-Impedanz aufwändig, aber: **USB Full-Speed (12 Mbit/s)** ist in der Praxis hinsichtlich Impedanz sehr tolerant. Ausreichend ist:

* **Differentielles Routing** (enges, paralleles Paar), Zielbreite/-abstand bei 1,55 mm FR4 und 35 µm Cu: **W = 0,35 mm, S = 0,20 mm** (ergibt rechnerisch ~90 Ω über GND-Kupferfläche auf der Unterseite).
* **Length-matched ±0,2 mm** (weniger kritisch als bei Hi-Speed).
* **Länge ≤ 50 mm** zwischen USB-C-Pads und MCU.
* **Keine Vias im Paar.**
* **USBLC6-2SC6** ESD-Schutz unmittelbar nach dem Connector.

Dafür wird der Bereich unter und neben dem USB-Paar auf der Unterseite als **ununterbrochene GND-Gießfläche** ausgelegt (siehe §11).

### 4.4 VBUS-Detection

VBUS über **100 kΩ / 49,9 kΩ Teiler** (ergibt ~1,65 V aus 5 V) auf einen freien GPIO (oder den dedizierten VBUS_DETECT-Pin des RP2350-USB-Subsystems, wenn verfügbar). Firmware nutzt das Signal für sauberen USB-Suspend.

---

## 5. MCU-Kern RP2354B

### 5.1 Pin-Supply-Übersicht (QFN-80)

| Gruppe      | Pins                    | Versorgt mit            |
|-------------|-------------------------|-------------------------|
| IOVDD       | 4× über Package verteilt | +3V3                    |
| DVDD (Core) | 2×                       | intern aus VREG_VOUT    |
| USB_VDD     | 1                        | +3V3                    |
| ADC_AVDD    | 1                        | +3V3_ANA                |
| VREG_IN     | 1                        | +3V3                    |
| VREG_VOUT   | 1 (Pin-out chipintern)   | 2,2 µF Keramik nach GND |

Jede IOVDD-Gruppe: **100 nF 0402 direkt am Pin** (< 2 mm Leiterbahn). DVDD: 4,7 µF + 100 nF. VREG_VOUT: 2,2 µF Keramik (wie im RP2350-Hardware-Design-Guide empfohlen).

### 5.2 Quarz

**12 MHz**, ±30 ppm, 18 pF CL — z. B. **Abracon ABLS-12.000MHZ-B4Y-T** oder **Murata CSTCR-12M0G55** (mit integrierten Lastkondensatoren, spart zwei Caps). Alternativ diskret mit 2× 27 pF C0G. Länge XIN/XOUT ↔ Quarz: **≤ 10 mm**, GND-geschirmt.

### 5.3 Reset und BOOTSEL

* **RUN** (Reset): 10 kΩ Pull-Up auf +3V3, Taster (6×6 mm Tact) gegen GND, 100 nF Entprell-Cap.
* **BOOTSEL**: Taster gegen GND (interner Pull-Up aktiv).

Beide Taster frontseitig, in einer Reihe, erkennbar beschriftet.

### 5.4 Debug-Port — 5-polig

Statt des bisherigen 10-poligen Cortex-Debug-Headers ein **schlanker 1×5-Header im 2,54-mm-Raster**:

| Pin | Signal   | Funktion                                       |
|-----|----------|------------------------------------------------|
| 1   | SWDIO    | SWD Datenleitung                               |
| 2   | SWCLK    | SWD Takt                                       |
| 3   | GND      | Referenzmasse                                  |
| 4   | DBG-UART TX | Vom RP2354B → externer Host (Log-Out)       |
| 5   | DBG-UART RX | Vom externen Host → RP2354B (optional Input) |

Pinout bewusst entkoppelt vom ARM-Standard — ein Adapterkabel (5 × Dupont auf J-Link/Picoprobe) wird mit der Platine dokumentiert. Der Debug-Port ist **für Sonderfälle und Bring-up** gedacht; im Produktivbetrieb flasht man ausschließlich über USB-BOOTSEL/UF2 (siehe Architektur-Dokument §9). Footprint zusätzlich für **Tag-Connect TC2030-NL** auslegen (3 Pins SWDIO/SWCLK/GND, damit bleibt der Footprint minimal).

---

## 6. GPIO-Masterbelegung

Die GPIO-Zuweisungen sind *Default*; die Firmware konfiguriert sie zur Laufzeit um, soweit PIO/HW-Matrix das erlaubt.

| GPIO | Default-Funktion               | Destination                                     |
|------|--------------------------------|-------------------------------------------------|
| GP0  | UART0 TX                       | Phy-Bank (RS232/RS485/TTL)                      |
| GP1  | UART0 RX                       | Phy-Bank                                        |
| GP2  | I²C0 SDA                       | Qwiic 1+2 & I²C0-Header                         |
| GP3  | I²C0 SCL                       | Qwiic 1+2 & I²C0-Header                         |
| GP4  | UART1 TX (Debug-UART)          | 5-Pin-Debug-Header                              |
| GP5  | UART1 RX (Debug-UART)          | 5-Pin-Debug-Header                              |
| GP6  | **RS485 DE (PIO2-getrieben)**  | SN65HVD72 DE/RE                                 |
| GP7  | **RS485 EN** (Transceiver-Enable) | SN65HVD72 shutdown                           |
| GP8  | **RS232 /SHDN** (Transceiver-Enable) | MAX3232 FORCEON                          |
| GP9  | SPI0 INT (User-IRQ)            | PMOD SPI0                                       |
| GP10 | SPI1 SCK                       | SPI1-Header                                     |
| GP11 | SPI1 MOSI                      | SPI1-Header                                     |
| GP12 | SPI1 MISO                      | SPI1-Header                                     |
| GP13 | SPI1 CS0                       | SPI1-Header                                     |
| GP14 | SPI1 CS1                       | SPI1-Header                                     |
| GP15 | SPI1 CS2                       | SPI1-Header                                     |
| GP16 | SPI0 MISO                      | PMOD                                            |
| GP17 | SPI0 CS                        | PMOD                                            |
| GP18 | SPI0 SCK                       | PMOD                                            |
| GP19 | SPI0 MOSI                      | PMOD                                            |
| GP20 | **CAN TX (PIO0)**              | SN65HVD230 TXD                                  |
| GP21 | **CAN RX (PIO0)**              | SN65HVD230 RXD                                  |
| GP22 | **RGB-LED Rot** (Kathode, Stromsenke) | RGB-LED Rot-Anschluss                    |
| GP23 | **RGB-LED Grün** (Kathode)     | RGB-LED Grün-Anschluss                          |
| GP24 | **RGB-LED Blau** (Kathode)     | RGB-LED Blau-Anschluss                          |
| GP25 | USER BUTTON                    | Frontpanel-Taster                               |
| GP26 | **ADC0**                       | User-Header „A0"                                |
| GP27 | **ADC1**                       | User-Header „A1"                                |
| GP28 | **ADC2**                       | User-Header „A2"                                |
| GP29 | **ADC3**                       | User-Header „A3"                                |
| GP30 | GPIO/PWM #1                    | User-Header Pin                                 |
| GP31 | GPIO/PWM #2                    | User-Header Pin                                 |
| GP32 | **1-Wire (PIO2)**              | 3-Pin-Header „1-Wire"                           |
| GP33 | PIO-UART #2 TX                 | UART-Header #2                                  |
| GP34 | I²C1 SDA                       | Qwiic 3 & I²C1-Header                           |
| GP35 | I²C1 SCL                       | Qwiic 3 & I²C1-Header                           |
| GP36 | PIO-UART #2 RX                 | UART-Header #2                                  |
| GP37–GP45 | GPIO/PWM #3–#11           | 2×10-User-Header                                |
| GP46 | VBUS_DETECT                    | 100 kΩ/49,9 kΩ Teiler aus VBUS                  |
| GP47 | Reserve / Test                 | Testpad                                         |

**PIO-Allocation**:
* PIO0 (4 SMs): **exklusiv can2040** (3 SMs aktiv, 1 frei, Block dediziert).
* PIO1 (4 SMs): 2 für PIO-UART #2, 2 reserviert für weitere PIO-UART-Kanäle.
* PIO2 (4 SMs): 1 RS485-DE-Precision-Toggle, 1 1-Wire, 2 Reserve (z. B. Logic-Capture, zweite 1-Wire-Instanz).

---

## 7. Kommunikationsbus-Peripherie

### 7.1 CAN 2.0B — nur Transceiver, CAN-Controller via PIO (can2040)

**TI SN65HVD230QDR** (3,3 V, SOIC-8, Q-grade −40…+125 °C). Wir nutzen den Chip ausschließlich als Bit-Level-Transceiver — der CAN-Controller läuft komplett in Software (K. O'Connor, `can2040`) auf PIO0 + einem ARM-Kern.

```
GP20 (CAN TX, PIO0) ──► TXD
GP21 (CAN RX, PIO0) ◄── RXD

                    SN65HVD230
                    VCC = +3V3
                    Rs  = 10 kΩ → GND  (normal slope mode)
                    VREF = NC (DNP)

                                CANH ─────────────────► DE-9 Pin 7
                                                        │
                     [120 Ω]◄── Terminator-Jumper JP1 ──┤
                                                        │
                                CANL ─────────────────► DE-9 Pin 2
```

**Terminierung**: ein einziger **120-Ω-Widerstand zwischen CANH und CANL**, **zuschaltbar über einen echten mechanischen Jumper** (2-poliger Pinheader 2,54 mm + Shunt). Offener Jumper = kein Terminator (wenn das Gerät als Stub in der Mitte eines CAN-Busses hängt). Shunt gesetzt = Terminator aktiv (wenn das Gerät am Busende hängt). **Keine Split-Termination mit GND-Mittelanzapfung** — das ist für diese Anwendung Overkill und hat in der ursprünglichen Version den Kern-Zweck des Jumpers verfehlt.

**ESD-/EMV-Schutz**:

* **PESD1CAN** (Nexperia, bidir. ESD-Dioden CANH↔GND und CANL↔GND).
* **Common-Mode-Choke Würth WE-CMB 744-231-580** (51 µH / 700 mA) in den CAN-Leitungen zwischen Transceiver und Connector — hält leitungsgebundene Emissionen unter EN-55032-Class-B-Schwellen.

**DE-9-Pinout (CiA 303-1-kompatibel), male connector**:

```
  1: reserved        6: reserved
  2: CAN_L           7: CAN_H
  3: CAN_GND         8: reserved
  4: reserved        9: CAN_V+ (DNP bei uns, nicht versorgt)
  5: optional Shield
```

### 7.2 RS485 — SN65HVD72 mit echtem Jumper-Terminator

**TI SN65HVD72DGK** (3,3 V, VSSOP-8, 20 Mbps). Explizite DE/RE-Steuerung durch die PIO (GP6), damit das Abschalten des Treibers zeitgenau am Stopbit passiert.

```
UART0 TX (via Phy-Bank) ───────►  D
UART0 RX (via Phy-Bank) ◄───────  R
GP6 (PIO2) ────────────────────►  DE/RE  (gebrückt)
GP7 ───────────────────────────►  EN     (Transceiver-Enable, high = aktiv)

                                   ┌──► A ──► Klemme 1
                   SN65HVD72       │
                                   └──► B ──► Klemme 2
                                       GND ─► Klemme 3 (Signal-Ground)
```

**Fail-Safe-Bias**: 680 Ω Pull-Up von A nach +3V3, 680 Ω Pull-Down von B nach GND. Bei leerem Bus liest der Empfänger eine definierte „1".

**Terminator**: **120 Ω A↔B, zuschaltbar über echten mechanischen Jumper JP2** (2-poliger Pinheader + Shunt). Kein Lötjumper — Jumper-Stift erreichbar, werkzeuglos umzustecken.

**Schutz**: **SMAJ6.5CA** bidir. TVS zwischen A↔GND und B↔GND, 10 Ω Serienwiderstände in beide Leitungen als letzte Verteidigungslinie.

**Connector**: **3-Pin Phoenix-Klemme PTSM-0,5/3-2,5** (2,5 mm Raster, steckbar, werkzeuglos).

### 7.3 RS232 — MAX3232

**Analog Devices MAX3232EIDR** (SOIC-16, 3,0–5,5 V, interne ±6-V-Ladungspumpe). Shutdown-Steuerung über GP8.

```
UART0 TX ──► T1IN         T1OUT ──► DE-9 Pin 3 (TX)
UART0 RX ◄── R1OUT        R1IN  ◄── DE-9 Pin 2 (RX)
```

Zweiter Kanal (T2/R2) bleibt nicht bestückt, kann für optionales RTS/CTS nachgerüstet werden.

**DE-9 female (DTE)**: 1=DCD NC, 2=RX, 3=TX, 4=DTR NC, 5=GND, 6=DSR NC, 7=RTS opt., 8=CTS opt., 9=RI NC.

**Ladungspumpen-Caps**: 4× 100 nF X7R (C1+/C1-, C2+/C2-, V+, V-).

### 7.4 UART-Phy-Auswahl — Option B (Enable-basiert, parallele Transceiver)

Bestätigte Designentscheidung: **alle drei Physical Layer hängen parallel an UART0**, exakt einer ist zur Zeit aktiv. Auswahl über die Enable-/Shutdown-Pins:

| Ziel        | SN65HVD72 EN (GP7) | MAX3232 SHDN (GP8) | TTL-Header |
|-------------|--------------------|--------------------|------------|
| **RS485**   | high (aktiv)       | low (aus)          | (Serienwiderstand isoliert) |
| **RS232**   | low (tri-state)    | high (aktiv)       | (Serienwiderstand isoliert) |
| **TTL**     | low                | low                | direkt getrieben |
| **Aus**     | low                | low                | (Pins tri-state)  |

Vorteile gegenüber der in V1 diskutierten Option A (TS3A27518-Mux-IC):

* ein IC weniger, 0,70 € BOM-Ersparnis pro Stück,
* kürzere Signalpfade ohne Mux-Parasitics,
* Firmware-Logik ist eine triviale Wahrheitstabelle,
* die Transceiver tri-staten ihre Ausgänge sauber im Shutdown, kein Konflikt.

Einziger Preis: die drei Transceiver teilen sich die TX/RX-Netze physisch. Layout-seitig wird TX vom MCU an einem T-Punkt aufgeteilt und mit 100 Ω Serie zu jedem Transceiver geführt; RX hat ebenfalls 100 Ω Serie aus jedem Transceiver auf den gemeinsamen Knoten zur MCU. Das ist unkritisch bei den vorgesehenen Baudraten (≤ 1 Mbaud TTL, ≤ 230 k RS232, ≤ 10 M RS485).

Der **TTL-UART-Header** (1×4 Pins VCC/TX/RX/GND, FTDI-Pinout) hängt direkt an der Knoten-Seite mit **220 Ω Serie** und **CDSOD323**-ESD-Array.

### 7.5 I²C ×2

| Bus   | SDA  | SCL  | Pull-Up | Connector                                      |
|-------|------|------|---------|------------------------------------------------|
| I²C0  | GP2  | GP3  | 2,2 kΩ nach +3V3 | 2× Qwiic JST-SH + 1× 4-Pin 0,1"-Header |
| I²C1  | GP34 | GP35 | 2,2 kΩ nach +3V3 | 1× Qwiic JST-SH + 1× 4-Pin 0,1"-Header |

Pull-Up-Wert passend für 400 kHz Fast-Mode; für 100-kHz-Standard auch 4,7 kΩ ok. CDSOD323-Array auf SDA/SCL direkt am Connector.

Kein Level-Shifter bestückt — moderne I²C-Sensoren sind quasi alle 3,3 V. **Footprint für BSS138-Level-Shifter als DNP vorgesehen**, falls spezifische 5-V-Legacy-Teile nötig werden.

### 7.6 SPI ×2

* **SPI0 via PMOD Type 2A** (Digilent-Standard): 2×6 Header, Pinout CS/MOSI/MISO/SCK/GND/VCC + INT/RST/weitere universalaktive Pins. Stecker: Male THT 0,1".
* **SPI1 Breakout-Header** 2×3 Pin (VCC/GND/SCK/MOSI/MISO/CS0) + 1×2 Zusatz für CS1/CS2. Zielgruppe: Flash-Dumper, Displays, SD-Adapter.

Jede SCK/MOSI-Leitung bekommt 22 Ω Serienwiderstand zur EMV-Dämpfung (bei kurzen Board-Strecken bestückt, für lange externe Kabel optional).

### 7.7 UART-Header (TTL)

Zwei **1×4 Pin 0,1"-Header** (VCC = +3V3, TX, RX, GND), FTDI-kompatibel:

* Header #1: UART0-TTL (Phy-Bank-Ausgang).
* Header #2: PIO-UART #2 (GP33/GP36).

Jede Leitung: 220 Ω Serie + CDSOD323-ESD. Das **Debug-UART (UART1, GP4/GP5)** läuft ausschließlich auf den 5-poligen Debug-Header (§5.4), damit Debug- und User-UARTs nicht durcheinandergeraten.

### 7.8 1-Wire

3-Pin-Header **V+/DQ/GND**. 4,7 kΩ Pull-Up DQ → +3V3. DQ an GP32 (PIO2-Applet). ESD: **PESD1IVN** + 100 Ω Serie.

### 7.9 GPIO/PWM/ADC-Header (2×10)

Zentraler User-Header, 20 Pins 0,1" Raster:

```
  1  GP30 (GPIO/PWM)        2  +3V3
  3  GP26 (ADC0, „A0")      4  GP31 (GPIO/PWM)
  5  GP27 (ADC1, „A1")      6  GP37 (GPIO/PWM)
  7  GP28 (ADC2, „A2")      8  GP38 (GPIO/PWM)
  9  GP29 (ADC3, „A3")     10  GP39 (GPIO/PWM)
 11  GP40 (GPIO/PWM)       12  +5V (vom Board, nach PTC)
 13  GP41 (GPIO)           14  GP42 (GPIO)
 15  GP43 (GPIO)           16  GP44 (GPIO)
 17  GP45 (GPIO)           18  Testpad / Reserve
 19  GND                   20  GND
```

* **+3V3** und **+5V** werden direkt aus den Board-Rails herausgeführt. Kein zusätzlicher PTC — die Eingangs-PTC (§3.3) schützt das Gesamtsystem.
* Alle GPIOs: 220 Ω Serie + CDSOD323-ESD-Array (2 Stück für 8 Pins je Array).
* Silkscreen beschriftet jede Zelle eindeutig.

---

## 8. Statusanzeige

### 8.1 Eine einzige RGB-LED, 3,3 V, GPIO-direkt

**Bauteil**: diskrete RGB-LED **LTST-C19HE1WT** (Lite-On, 0605 PLCC-4, common-anode) oder Äquivalent (z. B. Wurth 150141M173100 / Kingbright AAA3528). Durchlassspannungen typisch:

* Rot: Vf ≈ 1,95 V
* Grün: Vf ≈ 3,00 V
* Blau: Vf ≈ 3,05 V

**Treiber-Konzept** — common-anode, GPIO als Stromsenke, pro Farbe ein Vorwiderstand:

```
+3V3 ─── Anode
         │
         ├──[R_R = 150 Ω]──► GP22 (Rot)     | I = (3,3−1,95)/150 ≈ 9 mA
         ├──[R_G = 100 Ω]──► GP23 (Grün)    | I = (3,3−3,00)/100 ≈ 3 mA
         └──[R_B = 100 Ω]──► GP24 (Blau)    | I = (3,3−3,05)/100 ≈ 2,5 mA
```

Die **Stromaufnahme liegt weit unter der GPIO-Sink-Kapazität des RP2350** (konfigurierbare Drive-Strength 2/4/8/12 mA, Default 4 mA, absolute Grenze 12 mA pro Pin). Wir bleiben pro Kanal unter 10 mA, passend zu Drive-Strength-Stufe „8 mA". Bei All-On (Weiß) zieht die LED ≈ 15 mA aus der 3,3-V-Schiene — vernachlässigbar.

**Wahrnehmbare Helligkeit gleichen wir nicht über die Widerstände aus** (Vf unterschiedlich → gleiche R nützt nichts), sondern über **Firmware-PWM** (alle drei Kanäle auf PWM-fähigen GPIOs). Dadurch bekommen wir sauberes Farbmischen. PWM-Frequenz 1 kHz reicht völlig (flimmerfrei, unhörbar).

**Status-Kodierung** (Firmware, Default, konfigurierbar):

| Farbe            | Bedeutung                                   |
|------------------|---------------------------------------------|
| Grün pulsierend (Heartbeat) | System OK, USB aktiv              |
| Grün blinkend    | Bus-Activity (I²C/SPI/UART/CAN-Verkehr)     |
| Gelb/Orange      | CAN Error-Passive oder RS485-Timeout        |
| Rot              | Bus-Off oder Firmware-Fault                 |
| Blau             | USB-Suspend                                 |
| Violett/Magenta  | OTA / BOOTSEL-Mode                          |
| Weiß kurz        | User-Interaction (Taster, Host-Command ACK) |

Das deckt den gesamten ursprünglich für vier WS2812 geplanten Informationsumfang über eine Farb-Zeit-Kodierung ab und ist bei kurzem Blick auf das Gerät intuitiver als eine Kette diskreter LEDs.

### 8.2 Power-LED (diskret)

Zusätzlich eine **grüne 0805-LED mit 1 kΩ an +3V3** als rein elektrische Power-Anzeige (nicht Firmware-gesteuert). Glüht, sobald die Platine Strom hat — nützlich für die Fehlersuche, falls die Firmware hängt und die RGB-LED nicht reagiert.

### 8.3 Taster

* **RESET** (Tact 6×6 mm)
* **BOOTSEL** (Tact 6×6 mm)
* **USER** (GP25, frei belegbar)

Alle drei mit 100 nF Debounce + interner Pull-Up (bzw. 10 kΩ extern für RESET).

---

## 9. Schutzbeschaltung — Zusammenfassung

| Port                  | Schutz                                                                  |
|-----------------------|-------------------------------------------------------------------------|
| USB-C D+/D-           | USBLC6-2SC6 ESD                                                         |
| USB-C VBUS            | Schottky (Reverse-Block) + PTC 500 mA + TVS SMAJ5.0A + Ferrit           |
| CAN                   | PESD1CAN ESD, Common-Mode-Choke, Jumper-Terminator 120 Ω                |
| RS485                 | SMAJ6.5CA TVS, Fail-Safe-Bias, 10 Ω Serie, Jumper-Terminator 120 Ω     |
| RS232                 | integrierter ±15-kV-Schutz im MAX3232                                   |
| I²C (Qwiic + Header)  | CDSOD323-Array auf SDA/SCL                                              |
| SPI (PMOD + SPI1)     | CDSOD323-Array auf SCK/MOSI/MISO                                        |
| UART TTL Header       | CDSOD323 + 220 Ω Serie                                                  |
| GPIO/ADC Header       | 2× CDSOD323-Array für 8 Pins + 220 Ω Serie                              |
| 1-Wire                | PESD1IVN + 100 Ω Serie                                                  |
| Debug-Port (5-pin)    | keine aktive Schutzbeschaltung nötig (nur im Labor genutzt)             |

---

## 10. BOM (Kerngruppe)

| Ref     | Part                              | Package     | Zweck                               | Stück |
|---------|-----------------------------------|-------------|-------------------------------------|-------|
| U1      | **RP2354B**                       | QFN-80      | MCU + 2 MB integrierter Flash       | 1     |
| U2      | **AP2112K-3.3TRG1**               | SOT-25      | 3V3 LDO (bis 600 mA)                | 1     |
| U3      | **USBLC6-2SC6**                   | SOT-23-6    | USB ESD                             | 1     |
| U4      | **SN65HVD230QDR**                 | SOIC-8      | CAN Transceiver, 3,3 V              | 1     |
| U5      | **SN65HVD72DGK**                  | VSSOP-8     | RS485 Transceiver, 3,3 V            | 1     |
| U6      | **MAX3232EIDR**                   | SOIC-16     | RS232 Transceiver, 3,3 V            | 1     |
| LED1    | Power-LED grün 0805               | 0805        | Power-Indikator (Hardware-seitig)   | 1     |
| LED2    | **diskrete RGB-LED common-anode** (z. B. LTST-C19HE1WT) | PLCC-4 3,5×2,8 mm | Status-/Aktivitäts-Anzeige | 1     |
| X1      | 12 MHz Quarz, 18 pF               | 3225        | MCU-Oszillator                      | 1     |
| J1      | USB-C Buchse 16-Pin, Mid-Mount    | SMD         | Host-Interface                      | 1     |
| J2      | DE-9 male RA                      | THT         | CAN                                 | 1     |
| J3      | DE-9 female RA                    | THT         | RS232                               | 1     |
| J4      | Phoenix PTSM-0,5/3-2,5            | THT         | RS485-Klemme                        | 1     |
| J5–J7   | Qwiic JST-SH 4-Pin RA             | SMD         | I²C                                 | 3     |
| J8      | 2×6 0,1"-Header                   | THT         | PMOD SPI0                           | 1     |
| J9      | 2×3 0,1"-Header + 1×2             | THT         | SPI1 + CS1/CS2                      | 1+1   |
| J10–J11 | 1×4 0,1"-Header                   | THT         | TTL-UART #1/#2                      | 2     |
| J12     | 2×10 0,1"-Header                  | THT         | GPIO/PWM/ADC                        | 1     |
| J13     | 1×3 0,1"-Header                   | THT         | 1-Wire                              | 1     |
| J14     | **1×5 0,1"-Header**               | THT         | Debug (SWDIO/SWCLK/GND + Debug-UART) | 1    |
| JP1     | 1×2 0,1"-Header + Shunt           | THT         | **CAN-Terminator (mechanisch)**     | 1     |
| JP2     | 1×2 0,1"-Header + Shunt           | THT         | **RS485-Terminator (mechanisch)**   | 1     |
| SW1–SW3 | Tact 6×6 mm                       | SMD/THT     | RESET / BOOTSEL / USER              | 3     |
| F1      | **PTC 500 mA reset** (MF-NSMF050X-2) | 1812     | **USB-Eingangs-Sicherung**          | 1     |
| D1      | **MBR0520 Schottky**              | SOD-123     | **VBUS-Reverse-Feed-Blockade**      | 1     |
| D2      | SMAJ5.0A TVS                      | DO-214AC    | VBUS-Clamp                          | 1     |
| D3–D4   | SMAJ6.5CA TVS bidir.              | DO-214AC    | RS485 A/B                           | 2     |
| D5      | PESD1CAN                          | SOT-23      | CAN-ESD                             | 1     |
| D6–D8   | CDSOD323-T08L-Q ESD-Array         | SOD-323     | GPIO/UART/SPI ESD                   | 3–4   |
| L1–L2   | BLM18PG471 Ferrit                 | 0603        | VBUS / Supply-Entkopplung           | 2     |
| L3      | BLM15BB471 Ferrit                 | 0402        | ADC-AVDD                            | 1     |
| L4      | WE-CMB 744-231-580                | SMD         | CAN Common-Mode-Choke               | 1     |
| R-div   | 5,1 kΩ 1 %                        | 0402        | USB-C CC1/CC2                       | 2     |
| R-term  | **120 Ω 1 %**                     | 0603        | Terminator CAN (fest) + RS485 (fest), jeweils Jumper-geschaltet | 2 |
| R-pull  | 2,2 kΩ (I²C), 680 Ω (RS485-Bias)  | 0402        | Pull-Ups                            | 4+2   |
| R-LED   | 150 Ω (Rot), 100 Ω (Grün+Blau)    | 0402        | RGB-LED-Strom                       | 3     |
| R-ser   | 22 Ω / 100 Ω / 220 Ω              | 0402        | SPI-, UART-Knoten-, GPIO-Serie      | 20+   |
| C_bulk  | 22 µF X5R 10 V                    | 0805        | LDO-Out, VBUS                       | 2     |
| C_caps  | 100 nF X7R, 4,7 µF, 2,2 µF        | 0402/0603   | Abblock (MCU-Pins, Transceiver, VREG) | ~25  |

**Geschätzte BOM-Summe** (Einzelstück / 100 Stück):

* Halbleiter-Gruppe: ~**9 €** / **5,60 €**
* Passive: ~**2,50 €** / **1,20 €**
* Connectoren (inkl. 2× Jumper-Header+Shunt): ~**7,50 €** / **3,80 €**
* **PCB 2-Lagen 80×55 mm**: ~**2 €** / **0,50 €** (statt ~4 €/1,20 € bei 4 Lagen)

**Total**: ~**21 € / 11,10 € pro Einheit** bei 100er-Serie. Rund 1,60 €/Einheit günstiger als die V1-Version, hauptsächlich durch PCB (2 statt 4 Lagen), Wegfall des Level-Shifters und die Reduktion von 4 WS2812 auf 1 diskrete RGB-LED.

---

## 11. PCB-Design-Guidelines — **2-Lagen-Layout**

### 11.1 Stackup

| Lage  | Kupfer | Dielektrikum | Zweck                                              |
|-------|--------|--------------|----------------------------------------------------|
| L1 (Top)    | 35 µm | —            | Signale, alle aktiven Bauteile, Connectoren  |
| Core  | —      | 1,55 mm FR4  | Standard JLCPCB 2-layer 1,6 mm                     |
| L2 (Bottom) | 35 µm | —            | **durchgehende GND-Gießfläche**, minimale Signalführung (Jumper-Rückleitungen, Pull-Up-Netze) |

Das Layout konzentriert sich auf: **möglichst ununterbrochene GND-Fläche unter dem gesamten Board**, Signale primär auf Top, Bottom als GND-Reference. Alle Rückleitungen (Decoupling-Caps, ESD-Dioden-GNDs) gehen über Vias direkt auf L2.

### 11.2 Impedanz

* **USB D+/D- (90 Ω diff.)**: mit den o. g. W/S-Werten rechnerisch erreichbar, **über durchgehender GND-Fläche** geroutet. Impedance-Control beim Fertiger nicht zwingend bestellen (Mehrkosten), da USB-FS tolerant ist.
* **CAN H/L**: als **differentielles Paar** geroutet, **50 Ω single-ended** nicht erforderlich (CAN-Bitraten ≤ 1 Mbit/s).
* Alle anderen Signale: Slew-Rate-moderate, keine Impedanz-Kontrolle nötig.

### 11.3 Plazierungs-Prinzipien

* USB-C-Connector **links** (Eingang).
* MCU **mittig** auf dem Board, Transceiver-Gruppe geclustert an ihren jeweiligen Connectoren (CAN rechts, RS232 rechts, RS485 rechts).
* **Jumper JP1 (CAN-Term) und JP2 (RS485-Term)** **nahe am jeweiligen Connector**, von der Oberseite gut erreichbar, mit Silkscreen-Beschriftung „TERM 120Ω".
* Header-Reihe (User-GPIO, PMOD, SPI1) **oberhalb der MCU**.
* **RGB-LED und Taster an der Vorderkante** des Boards.
* **3 mm Keepout** um den USB-C-Connector (mechanische Biegestress-Zone).

### 11.4 Decoupling

* Je IOVDD/DVDD/USB_VDD/VREG_IN-Pin: **100 nF 0402** < 2 mm Leiterbahn, GND-Via direkt neben dem Cap.
* LDO Input/Output: je 10 µF / 22 µF X5R nahe am IC.
* Transceiver-VCCs: je 100 nF + 4,7 µF.

### 11.5 Ground-Handling auf 2 Lagen

* L2 ist **pure GND-Plane**. Wenn einzelne Signale auf L2 nötig werden (Pull-Up-Netz, Jumper-Stub), werden sie als **kurze Insel** geführt, **mit umlaufendem GND-Guard**, um Plane-Splits zu minimieren.
* **Analog-GND** (ADC) wird sternförmig an **einem** Punkt mit Main-GND verbunden (Ferrit + eine Via).
* CAN-Shield (DE-9 Pin 5): über 1 MΩ + 10 nF an GND (Hybrid-Ground).

### 11.6 Clock-Routing

* 12 MHz-Quarz mit GND-Guard-Ring, unterhalb auf L2 solides GND.
* Keine schnellen Signale (USB, CAN-PIO) parallel zu XIN/XOUT näher als 3 mm.

### 11.7 CAN-Layout

* CAN_H/CAN_L als Paar, length-matched.
* Common-Mode-Choke **vor** dem ESD-Array und **vor** dem Connector.
* Jumper JP1 direkt zwischen den beiden CAN-Leitungen, Terminator-R in Reihe mit dem Jumper-Pin — so ist R und Jumper immer zusammen aktiv.

### 11.8 Test-/Programming-Punkte

* Debug-Header J14 (5-pin) permanent bestückt.
* **Tag-Connect TC2030-NL**-Footprint (3 Pins SWDIO/SWCLK/GND) für Factory-Test ohne Header-Bestückung.
* Testpads (1,5 mm Pad mit Silkscreen-Label) auf: +3V3, +5V_BOARD, VBUS, GND (×2), BOOTSEL, RUN.

---

## 12. Mechanik und Gehäuse

### 12.1 Formfaktor

**80 × 55 × 15 mm**. Passt in die Hemdtasche, steht stabil auf dem Schreibtisch, lässt sich in Standard-Extrusionsgehäuse einhausen.

### 12.2 PCB-Features

* **4× M3-Befestigungslöcher** (3,2 mm, 6 mm Cu-Keepout) in den Ecken.
* **ENIG-Finish** empfohlen (robuste Kontakte für wiederholtes Stecken).
* Silkscreen **ober- und unterseitig**, jeder Connector eindeutig beschriftet, beide Terminator-Jumper mit „TERM" + Wert gekennzeichnet.

### 12.3 Gehäuse-Optionen

* **V1 (Dev-Kit)**: offenes Board, 4× Silikon-Standfüße, keine Umhausung.
* **V2 (Produkt)**: **Hammond 1455C801** (80×54×23 mm Alu-Extrusion), Frontplatten als Lasercut-Acryl oder CNC-Alu.
* **V-Community**: 3D-Druck-Enclosure (PETG, schraubenlos rastend), STL im Repo.

---

## 13. Bring-up-Checkliste

### 13.1 Power-On (ohne Firmware)

1. Sichtkontrolle: keine Brücken, Schottky-/Elko-/TVS-Polaritäten korrekt.
2. Kurzschlusstest VBUS, +5V_BOARD, +3V3 gegen GND — alle im MΩ-Bereich.
3. Strombegrenztes Labornetzteil (100 mA Limit, 5,0 V) an USB-C-VBUS. Schottky-Abfall messen (~0,35 V → +5V_BOARD ≈ 4,65 V).
4. LDO-Messung: +3V3 = 3,30 V ± 50 mV.
5. Ruhestrom ohne Firmware (ROM-BOOTSEL): 18–25 mA erwartet.

### 13.2 SWD-Kontakt

6. Debugger an 5-Pin-Header (SWDIO/SWCLK/GND). `openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg` → `reset halt`. Target muss antworten.

### 13.3 UF2-Flash per BOOTSEL

7. RESET + BOOTSEL: Gerät erscheint als USB-MSC „RP2350". Drag&Drop-Test mit minimaler Test-Firmware (USB-CDC-Echo + RGB-LED-Selbsttest Rot/Grün/Blau/Weiß).

### 13.4 Peripherie-Selbsttest

8. UART-Loopback auf TTL-Header (TX↔RX-Brücke), 921 600 Bd × 1 MB Datenstrom.
9. I²C-Scan beider Busse mit Test-Sensor (SHT40) auf Qwiic.
10. SPI-Loopback (MOSI↔MISO-Brücke), 1/10/24 MHz.
11. CAN-Test gegen CANable/PEAK, 125k/500k/1M bit/s, **JP1 bestückt und offen** testen.
12. RS485: Gegenstelle auf A/B, **JP2 bestückt/offen** testen, Fail-Safe-Bias verifizieren.
13. RS232: Loopback gegen Host-USB-Seriell-Dongle.
14. ADC: Referenzspannungen 3,3 V, 1,65 V, 0 V an A0..A3, Linearitätstest.
15. PWM: 1 k / 10 k / 100 kHz bei 10/50/90 % Duty auf GPIO-PWM-Pins.
16. RGB-LED: Firmware-Farbzyklus, Helligkeitskalibrierung per PWM (visuell „sauberes Weiß" bei ≈ 30 % Duty).

### 13.5 EMV-Pre-Compliance

17. Conducted Emissions am USB-Kabel (LISN) nach EN 55032 Class B.
18. ESD-HBM ±8 kV auf allen externen Pins.
19. EFT-Burst 1 kV auf USB-Kabel nach EN 61000-4-4.

(Pre-Compliance, nicht zertifizierend — für Laborselbstbau und Pilotkleinserien ausreichend.)

---

## 14. Änderungen gegenüber V1

| Bereich            | V1                                                        | V2 (dieses Dokument)                                                   |
|--------------------|-----------------------------------------------------------|------------------------------------------------------------------------|
| PCB                | 4-Lagen-Stackup, impedanzkontrolliert                     | **2-Lagen**, durchgehende GND-Fläche auf L2, USB-FS-tolerant geroutet  |
| Status-LED         | 4× WS2812 (5 V) + 74AHCT1G125 Level-Shifter + 3 diskrete  | **1× diskrete RGB-LED (3,3 V)** GPIO-direkt mit R pro Kanal + 1 diskrete Power-LED |
| LED-Treiber        | 5-V-Rail-abhängig, Level-Shifter-Pflicht                  | Kein Level-Shifter, rein 3,3 V, LED-Kanäle via PWM-GPIO                |
| Strom-Wahl         | Jumper USB/EXT + Barrel-Jack                              | **Nur USB-C**, keine Jumper, keine externe Quelle                      |
| USB-PD             | nicht implementiert, aber Option für später offen         | **Explizit nein** — klassisches USB 2.0 FS, 5,1 kΩ CC-Sink             |
| Eingangsschutz     | TVS + Ferrit + Reverse-Schottky                           | **TVS + Ferrit + Reverse-Schottky + PTC 500 mA (self-reset)**          |
| CAN-Terminator     | Split-Termination 2× 60 Ω + Lötjumper zu GND-Cap          | **Single 120 Ω + echter mechanischer 2-Pin-Jumper** (Shunt)            |
| RS485-Terminator   | 120 Ω + Lötjumper                                         | **120 Ω + echter mechanischer 2-Pin-Jumper**                           |
| Debug-Port         | 10-polig Cortex-Debug Standard                            | **5-polig: SWDIO/SWCLK/GND + Debug-UART-TX/-RX**                       |
| UART-Phy-Mux       | Option A (TS3A27518 Mux-IC) diskutiert, Option B empfohlen | **Option B final**: alle drei Transceiver parallel, EN/SHDN-Auswahl   |
| 3,3-V-Versorgung   | AP2112K mit LED-Last ~250 mA                              | **AP2112K locker**, Hauptlast 160–200 mA inkl. Transceiver             |

---

## 15. Nächste Schritte

**Design-Phase**:

1. Schematic-Refresh in KiCad 9 auf Basis dieser Spezifikation (alle V2-Änderungen einpflegen).
2. Symbol-/Footprint-Review gegen die BOM (Digi-Key/Mouser-Abgleich, Stock-Check).
3. 2-Lagen-Layout 80×55 mm.
4. Netlist-Check, DRC, 3D-Preview gegen Hammond-1455C801-Gehäuse.
5. Gerber-Export. 5er-Prototyp bei JLCPCB oder Aisler (~1–2 Wochen Turnaround, 2-Lagen-Standard-Service).
6. SMT-Assembly — wahlweise Handbestückung (Heißluft, Stencil) oder Assembly-Service.

**Bring-up & Firmware**:

7. Bring-up nach §13 durchlaufen.
8. Firmware-Port aus `rpbridge-architektur.md` auf die finalen GP-Zuweisungen (§6).
9. Integrations-Schluckauf-Test: `i2cdetect` + `candump` + `libgpiod` parallel.
10. Revision B nur bei echten Bring-up-Findings (kein „vielleicht-später").

**Dokumentation (V1.0-Release)**:

* Quickstart (A4).
* Pin-Map-Tischreferenz (Kreditkartengröße, laminiert).
* Beispielscripts im GitHub-Repo (siehe Architektur-Dokument §9).

---

## 16. Referenzen

* **RP2350 Datasheet + Hardware Design Guide** (Raspberry Pi Foundation)
* **RP2354 Announcement + Errata Sheet**
* **can2040 Hardware Notes** (K. O'Connor, GitHub)
* **CiA 303-1** CAN Cabling and DE-9 Pin Assignment
* **PMOD Interface Specification Rev. 1.3.1** (Digilent)
* **SparkFun Qwiic Connect System** — Mechanik und Pinning
* **TI SN65HVD230 / SN65HVD72 / MAX3232** Datasheets (Applikationsbeispiele Fail-Safe-Bias, Terminator)
* **Hammond 1455 Series Datasheet**
* **Bel Fuse MF-NSMF050X-2** (PTC-Sicherung) Datasheet
* **Diodes Inc. AP2112K** Datasheet (LDO-Thermal-Derating)

---

*Ende des Hardware-Designs V2.*
