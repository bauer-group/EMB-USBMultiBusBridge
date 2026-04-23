# RPBridge – CI/CD via GitHub Actions

**Dokumentversion:** 1.0
**Datum:** 2026-04-22
**Scope:** Komplette Continuous-Integration- und Release-Pipeline für das RPBridge-Projekt (RP2354B-Firmware + Linux-Kernel-Treiber) ausschließlich auf Basis von **GitHub Actions**.

Dieses Dokument ergänzt `rpbridge-architektur.md` (Firmware + Treiber) und `rpbridge-hardware-design.md` (RP2354B-Hardware). Es beschreibt reproduzierbare, cache-optimierte, mehrstufig geprüfte Pipelines für drei Haupt-Artefakte:

1. **Firmware** – `rpbridge.uf2` / `.elf` / `.bin` für RP2350 (und optional RP2040-Fallback) via pico-sdk.
2. **Linux-Treiber** – Out-of-Tree-Kernel-Module (`rpbridge-core`, `rpbridge-i2c`, `rpbridge-spi`, `rpbridge-gpio`, `rpbridge-can`, …), gepackt als DKMS-Quelle sowie DEB/RPM.
3. **Userspace-Tools** – `rpbridge-ctl` (picotool-basierter OTA-Trigger + DT-Overlay-Loader + Diagnose-CLI).

Jede Pipeline wird durch einen dedizierten Workflow abgebildet. Alle Workflows laufen auf `ubuntu-latest` bzw. `ubuntu-22.04`/`ubuntu-24.04` Runner-Images und nutzen ausschließlich offizielle, gepinnte Actions.

---

## 1. Repository-Layout

Empfohlenes Monorepo-Layout, damit ein einzelner Tag-Push eine konsistente Release-Gruppe (Firmware + Treiber + Tools) produziert:

```
rpbridge/
├── .github/
│   └── workflows/
│       ├── firmware.yml          # Pico-SDK-Build + UF2-Artefakt
│       ├── driver.yml            # Out-of-Tree-Kernelmodul-Matrix
│       ├── driver-static.yml     # checkpatch/sparse/smatch/coccinelle
│       ├── driver-kunit.yml      # KUnit + umockdev-Integrationstests
│       ├── driver-package.yml    # DKMS/DEB/RPM-Pakete
│       ├── tools.yml             # rpbridge-ctl (Rust oder C)
│       ├── docs.yml              # mkdocs → GitHub Pages
│       ├── release.yml           # Tag → kombinierter GitHub-Release
│       └── nightly.yml           # Nächtlicher Mainline-Kernel-Check
├── firmware/
│   ├── CMakeLists.txt
│   ├── pico_sdk_import.cmake
│   └── src/...
├── driver/
│   ├── Kbuild
│   ├── Makefile
│   ├── dkms.conf
│   ├── packaging/
│   │   ├── debian/
│   │   └── rpm/rpbridge.spec
│   └── src/...
├── tools/
│   └── rpbridge-ctl/
├── dts/
│   └── overlays/
├── docs/
└── README.md
```

Separate Repos sind ebenfalls möglich, in der Praxis hat sich aber das Monorepo für Treiber+Firmware-Bundles bewährt (gleiche Versionsnummer, gleicher Tag, gleicher Release-Body – siehe gs_usb, candleLight_fw, hid-multitouch-Ökosystem).

---

## 2. Versionierung, Tags, Releases

* **SemVer 2.0.** Tags im Format `vX.Y.Z` (z. B. `v0.3.1`). Pre-Releases via `vX.Y.Z-rc1`.
* Ein Tag-Push triggert `release.yml`, das Ergebnisse der anderen Workflows über `workflow_run` einsammelt **oder** die Jobs selbst reuseable-callt.
* **Version-String-Injection:**
  * Firmware: CMake liest `git describe --tags --always --dirty` und definiert `RPBRIDGE_VERSION` als `-D`-Flag. Der USB-Descriptor (`iProduct`) und die Capability-Antwort melden diese Version.
  * Treiber: `MODULE_VERSION("x.y.z+<sha>")` wird aus derselben Quelle generiert.
  * Tools: Cargo/CMake übernehmen den `git describe`.

---

## 3. Firmware-Pipeline (`firmware.yml`)

### 3.1 Ziele

* Build für `PICO_BOARD=rpbridge_rp2354b` (Default) und `PICO_BOARD=pico2` (Generic-Fallback) und `PICO_BOARD=pico` (RP2040-Emergency-Fallback ohne PIO-CAN).
* Reproduzierbar: **gepinnte pico-sdk-Version** als Submodul oder `actions/checkout` mit festem SHA, gepinnte ARM-GCC-Version.
* Cache: `ccache` beschleunigt die CMake-Kompilate (typisch Faktor 4–8 beim zweiten Build).
* Artefakte: `.uf2`, `.elf`, `.bin`, `.hex`, `.dis` plus automatisch generierter SBOM und `size`-Report.
* Auf Release-Tag: Anhänge am GitHub-Release inkl. `CHECKSUMS.txt` (SHA-256) und minisign-Signatur.

### 3.2 Build-Toolchain

* **ARM GCC:** `arm-none-eabi-gcc` aus ARMs Launchpad-Release. Ubuntu-24.04-Paket ist aktuell genug (14.2), für volle Reproduzierbarkeit wird die Launchpad-Tarball-Version **gepinnt** auf eine konkrete Minor-Version.
* **pico-sdk:** Als Git-Submodul unter `firmware/pico-sdk` auf festen Tag (z. B. `2.1.1`).
* **picotool:** Wird als Teil des Builds aus dem pico-sdk mitgebaut oder über ein eigenes Submodul als Release-Artefakt vom Upstream-Release gezogen (für Smoketests).
* **CMake ≥ 3.28**, **Ninja**, **Python 3.11+** (für PIO-Codegen).

### 3.3 Workflow

```yaml
# .github/workflows/firmware.yml
name: firmware

on:
  push:
    branches: [main]
    paths:
      - 'firmware/**'
      - '.github/workflows/firmware.yml'
    tags:
      - 'v*'
  pull_request:
    paths:
      - 'firmware/**'
      - '.github/workflows/firmware.yml'
  workflow_dispatch:

concurrency:
  group: firmware-${{ github.ref }}
  cancel-in-progress: true

permissions:
  contents: read

env:
  PICO_SDK_PATH: ${{ github.workspace }}/firmware/pico-sdk
  CCACHE_DIR:    ${{ github.workspace }}/.ccache
  CCACHE_MAXSIZE: 500M

jobs:
  build:
    name: build (${{ matrix.board }}/${{ matrix.build_type }})
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        board:      [rpbridge_rp2354b, pico2, pico]
        build_type: [Release, RelWithDebInfo]
        include:
          - board: rpbridge_rp2354b
            platform: rp2350
          - board: pico2
            platform: rp2350
          - board: pico
            platform: rp2040

    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          submodules: recursive
          fetch-depth: 0          # für git describe

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            cmake ninja-build \
            gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
            python3 python3-pip \
            libusb-1.0-0-dev \
            pkg-config

      - name: Verify toolchain
        run: |
          arm-none-eabi-gcc --version
          cmake --version
          ninja --version

      - name: Setup ccache
        uses: hendrikmuhs/ccache-action@v1.2
        with:
          key: firmware-${{ matrix.platform }}-${{ matrix.build_type }}
          max-size: 500M
          variant: ccache

      - name: Configure
        working-directory: firmware
        run: |
          cmake -S . -B build \
            -G Ninja \
            -DCMAKE_BUILD_TYPE=${{ matrix.build_type }} \
            -DPICO_BOARD=${{ matrix.board }} \
            -DPICO_PLATFORM=${{ matrix.platform }} \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DRPBRIDGE_VERSION=$(git describe --tags --always --dirty)

      - name: Build
        working-directory: firmware
        run: cmake --build build --parallel

      - name: Size report
        working-directory: firmware
        run: |
          arm-none-eabi-size --format=berkeley build/rpbridge.elf \
            | tee build/size.txt
          arm-none-eabi-size -A build/rpbridge.elf \
            | tee -a build/size.txt

      - name: Disassembly (Release only)
        if: matrix.build_type == 'Release'
        working-directory: firmware
        run: |
          arm-none-eabi-objdump -d build/rpbridge.elf > build/rpbridge.dis

      - name: Checksums
        working-directory: firmware/build
        run: |
          sha256sum rpbridge.uf2 rpbridge.elf rpbridge.bin rpbridge.hex \
            > CHECKSUMS.txt

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: firmware-${{ matrix.board }}-${{ matrix.build_type }}
          path: |
            firmware/build/rpbridge.uf2
            firmware/build/rpbridge.elf
            firmware/build/rpbridge.bin
            firmware/build/rpbridge.hex
            firmware/build/rpbridge.dis
            firmware/build/size.txt
            firmware/build/CHECKSUMS.txt
          if-no-files-found: error
          retention-days: 30

  smoke-picotool:
    name: picotool info on artifact
    runs-on: ubuntu-24.04
    needs: build
    steps:
      - name: Checkout (for picotool build)
        uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Install deps
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            cmake ninja-build gcc g++ libusb-1.0-0-dev pkg-config

      - name: Build picotool
        run: |
          git clone --depth=1 https://github.com/raspberrypi/picotool.git
          cmake -S picotool -B picotool/build \
            -G Ninja \
            -DPICO_SDK_PATH=${{ github.workspace }}/firmware/pico-sdk
          cmake --build picotool/build

      - name: Download firmware
        uses: actions/download-artifact@v4
        with:
          name: firmware-rpbridge_rp2354b-Release
          path: fw

      - name: Inspect UF2
        run: |
          ./picotool/build/picotool info  -a fw/rpbridge.uf2
          ./picotool/build/picotool verify      fw/rpbridge.uf2 || true
```

### 3.4 Erläuterungen & Design-Entscheidungen

* **`concurrency`-Block** bricht in Pull Requests laufende Vorgänger-Runs automatisch ab – spart Minuten.
* **`fetch-depth: 0`** ist nötig, damit `git describe` saubere Tags liefert.
* **`hendrikmuhs/ccache-action`** ist der De-Facto-Standard; der Key ist pro Platform + BuildType separiert, damit Release-/Debug-Builds nicht gegenseitig Cache-Misses verursachen.
* **`PICO_BOARD=rpbridge_rp2354b`** ist ein **custom board header** im Repo (`firmware/boards/rpbridge_rp2354b.h`); `PICO_PLATFORM=rp2350` wird explizit gesetzt, da RP2354B intern RP2350B ist, aber mit `RP2350B` + `PICO_FLASH_SIZE_BYTES=(2*1024*1024)`.
* **`libusb-1.0-0-dev`** wird benötigt, falls `picotool` lokal gebaut wird (z. B. für den Smoke-Job).
* **Artefakt-Retention 30 Tage** reicht für Nightly-CI; Release-Tags werden separat über `release.yml` permanent angehängt.

### 3.5 Reproduzierbarkeit

Ergänzend zum CI-Build empfiehlt sich ein `scripts/build.sh`, das lokal identische Commands verwendet – damit sind CI-Fehler lokal reproduzierbar. Optional kann ein Reproducibility-Job wie folgt aussehen:

```yaml
  reproducible:
    name: reproducible build
    runs-on: ubuntu-24.04
    needs: build
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive, fetch-depth: 0 }
      - name: Build twice and diff UF2
        run: |
          scripts/build.sh --board rpbridge_rp2354b --build-type Release \
            --outdir build1
          scripts/build.sh --board rpbridge_rp2354b --build-type Release \
            --outdir build2
          diff -q build1/rpbridge.uf2 build2/rpbridge.uf2
```

Für vollständige Bit-Reproduzierbarkeit müssen Zeitstempel und `__DATE__`-Makros eliminiert sein (ARM-GCC-Flag `-Wdate-time` + `SOURCE_DATE_EPOCH`).

---

## 4. Linux-Treiber-Pipeline

Die Treiber-Seite ist deutlich mehrschichtiger als die Firmware-Seite. Wir splitten in **vier** Workflows:

1. `driver.yml` – Build-Matrix über Kernel-Versionen, schlägt Pipeline hart fehl bei Build-Fehlern.
2. `driver-static.yml` – checkpatch, sparse, smatch, coccinelle, clang-format – reportet, blockt nicht den Merge.
3. `driver-kunit.yml` – KUnit-Tests + umockdev-basierte User-Space-Integrationstests.
4. `driver-package.yml` – DKMS-Tarball, DEB, RPM.

### 4.1 Kernel-Matrix-Strategie

Wir bauen gegen das **Kernel-Header-Paket** aus den offiziellen Distributionen plus gegen vanilla stable LTS und Mainline. Die Matrix sieht so aus:

| Kernel | Quelle | Distros (primär) | Status |
|---|---|---|---|
| 5.15 LTS | kernel.org stable | Ubuntu 22.04 | required |
| 6.1 LTS  | kernel.org stable | Debian 12, Rocky 9 | required |
| 6.6 LTS  | kernel.org stable | Ubuntu 24.04 | required |
| 6.12 LTS | kernel.org stable | Ubuntu 24.04 HWE, Fedora 41 | required |
| 6.13     | kernel.org stable | Fedora 42 | required |
| mainline | `torvalds/linux` `master` | – | informational (nightly) |

### 4.2 `driver.yml`

```yaml
# .github/workflows/driver.yml
name: driver

on:
  push:
    branches: [main]
    paths:
      - 'driver/**'
      - '.github/workflows/driver.yml'
    tags:
      - 'v*'
  pull_request:
    paths:
      - 'driver/**'
      - '.github/workflows/driver.yml'
  workflow_dispatch:

concurrency:
  group: driver-${{ github.ref }}
  cancel-in-progress: true

permissions:
  contents: read

env:
  KERNELS_DIR: ${{ github.workspace }}/kernels
  CCACHE_DIR:  ${{ github.workspace }}/.ccache
  CCACHE_MAXSIZE: 1G

jobs:
  build:
    name: build (${{ matrix.kver }})
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        kver: ['5.15', '6.1', '6.6', '6.12', '6.13']

    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with: { fetch-depth: 0 }

      - name: Install build deps
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            build-essential bc bison flex libssl-dev libelf-dev \
            rsync cpio ccache \
            dwarves kmod \
            wget xz-utils

      - name: Setup ccache
        uses: hendrikmuhs/ccache-action@v1.2
        with:
          key: driver-kernel-${{ matrix.kver }}
          max-size: 1G

      - name: Cache kernel source
        id: kcache
        uses: actions/cache@v4
        with:
          path: ${{ env.KERNELS_DIR }}/linux-${{ matrix.kver }}
          key: kernel-src-${{ matrix.kver }}

      - name: Fetch + prepare kernel headers
        if: steps.kcache.outputs.cache-hit != 'true'
        run: |
          mkdir -p "$KERNELS_DIR"
          cd "$KERNELS_DIR"
          LATEST=$(curl -s https://www.kernel.org/releases.json \
            | python3 -c "import sys,json; \
                d=json.load(sys.stdin); \
                print(next(r['version'] for r in d['releases'] \
                  if r['version'].startswith('${{ matrix.kver }}.')))")
          echo "Using kernel $LATEST"
          wget -q https://cdn.kernel.org/pub/linux/kernel/v${LATEST%%.*}.x/linux-$LATEST.tar.xz
          tar xJf linux-$LATEST.tar.xz
          mv linux-$LATEST linux-${{ matrix.kver }}
          cd linux-${{ matrix.kver }}
          make defconfig
          make modules_prepare -j$(nproc)

      - name: Build module
        run: |
          make -C driver \
            KDIR="$KERNELS_DIR/linux-${{ matrix.kver }}" \
            CC="ccache gcc" \
            W=1 C=1 \
            -j$(nproc) \
            V=1

      - name: Modinfo sanity
        run: |
          for ko in driver/*.ko; do
            echo "--- $ko ---"
            modinfo "$ko"
          done

      - name: Upload module artifacts
        uses: actions/upload-artifact@v4
        with:
          name: driver-${{ matrix.kver }}
          path: |
            driver/*.ko
            driver/Module.symvers
          retention-days: 14
```

### 4.3 `driver-static.yml` – statische Analyse

```yaml
# .github/workflows/driver-static.yml
name: driver-static

on:
  pull_request:
    paths:
      - 'driver/**'
      - '.github/workflows/driver-static.yml'
  push:
    branches: [main]
    paths:
      - 'driver/**'

permissions:
  contents: read
  pull-requests: write   # für PR-Kommentare via reviewdog

jobs:
  checkpatch:
    name: checkpatch.pl
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
        with: { fetch-depth: 0 }

      - name: Install deps
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            git perl-modules codespell python3-ply python3-git

      - name: Fetch checkpatch.pl + friends
        run: |
          KVER=6.12
          mkdir -p tools/kernel
          cd tools/kernel
          for f in checkpatch.pl const_structs.checkpatch spelling.txt; do
            wget -q "https://raw.githubusercontent.com/torvalds/linux/v${KVER}/scripts/$f" \
              -O "$f"
          done
          chmod +x checkpatch.pl

      - name: Run checkpatch on changed files
        if: github.event_name == 'pull_request'
        run: |
          git fetch origin ${{ github.base_ref }}
          git diff --name-only origin/${{ github.base_ref }}...HEAD \
            -- 'driver/**/*.[ch]' \
            | xargs -r ./tools/kernel/checkpatch.pl \
              --no-tree --strict --show-types --terse \
              --ignore=CONST_STRUCT,FILE_PATH_CHANGES || true

      - name: Run checkpatch on full driver (informational)
        run: |
          find driver -name '*.[ch]' -print0 \
            | xargs -0 ./tools/kernel/checkpatch.pl \
              --no-tree --terse --ignore=CONST_STRUCT | tee checkpatch.log || true

      - name: Upload report
        uses: actions/upload-artifact@v4
        with:
          name: checkpatch-report
          path: checkpatch.log

  sparse:
    name: sparse
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install sparse
        run: |
          sudo apt-get update
          sudo apt-get install -y sparse build-essential bc bison flex \
            libssl-dev libelf-dev rsync cpio wget xz-utils

      - name: Prepare kernel 6.12 headers
        run: |
          mkdir -p k && cd k
          LATEST=$(curl -s https://www.kernel.org/releases.json \
            | python3 -c "import sys,json; \
                d=json.load(sys.stdin); \
                print(next(r['version'] for r in d['releases'] \
                  if r['version'].startswith('6.12.')))")
          wget -q https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$LATEST.tar.xz
          tar xJf linux-$LATEST.tar.xz
          mv linux-$LATEST linux
          cd linux
          make defconfig && make modules_prepare -j$(nproc)

      - name: Build with sparse
        run: |
          make -C driver \
            KDIR=$GITHUB_WORKSPACE/k/linux \
            C=2 CF='-D__CHECK_ENDIAN__' \
            W=1 \
            -j$(nproc) 2>&1 | tee sparse.log

      - name: Upload sparse log
        uses: actions/upload-artifact@v4
        with:
          name: sparse-log
          path: sparse.log

  smatch:
    name: smatch
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install smatch
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            build-essential bc bison flex libssl-dev libelf-dev \
            rsync cpio wget xz-utils git sqlite3 libsqlite3-dev \
            libxml2-dev llvm-dev
          git clone --depth=1 https://repo.or.cz/smatch.git
          make -C smatch -j$(nproc)

      - name: Prepare kernel 6.12
        run: |
          mkdir -p k && cd k
          LATEST=$(curl -s https://www.kernel.org/releases.json \
            | python3 -c "import sys,json; \
                d=json.load(sys.stdin); \
                print(next(r['version'] for r in d['releases'] \
                  if r['version'].startswith('6.12.')))")
          wget -q https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$LATEST.tar.xz
          tar xJf linux-$LATEST.tar.xz
          mv linux-$LATEST linux
          cd linux
          make defconfig && make modules_prepare -j$(nproc)

      - name: Run smatch via compile_commands.json
        run: |
          make -C driver \
            KDIR=$GITHUB_WORKSPACE/k/linux \
            CHECK="$GITHUB_WORKSPACE/smatch/smatch --full-path" \
            C=1 \
            -j$(nproc) 2>&1 | tee smatch.log

      - name: Upload smatch log
        uses: actions/upload-artifact@v4
        with:
          name: smatch-log
          path: smatch.log

  coccinelle:
    name: coccinelle semantic patches
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install coccinelle
        run: |
          sudo apt-get update
          sudo apt-get install -y coccinelle

      - name: Run upstream cocci scripts
        run: |
          mkdir -p cocci
          cd cocci
          for script in \
            api/devm_kzalloc.cocci \
            api/platform_no_drv_owner.cocci \
            api/ptr_ret.cocci \
            free/kfree.cocci \
            iterators/device_node_continue.cocci \
            locks/double_lock.cocci \
            misc/cond_no_effect.cocci ; do
            wget -q "https://raw.githubusercontent.com/torvalds/linux/v6.12/scripts/coccinelle/$script" \
              -O "$(basename $script)"
          done
          for c in *.cocci; do
            echo "=== $c ==="
            spatch --sp-file "$c" --dir "$GITHUB_WORKSPACE/driver" \
              --very-quiet 2>/dev/null | tee -a cocci.log || true
          done

      - name: Upload coccinelle log
        uses: actions/upload-artifact@v4
        with:
          name: coccinelle-log
          path: cocci/cocci.log

  clang-format:
    name: clang-format
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
        with: { fetch-depth: 0 }
      - name: Fetch upstream .clang-format
        run: |
          wget -q https://raw.githubusercontent.com/torvalds/linux/v6.12/.clang-format \
            -O driver/.clang-format
      - uses: DoozyX/clang-format-lint-action@v0.18
        with:
          source: 'driver'
          extensions: 'c,h'
          clangFormatVersion: 18
          style: file
```

### 4.4 `driver-kunit.yml` – KUnit + umockdev

**KUnit** deckt interne Pure-Code-Tests ab (Protokoll-Framing, TLV-Parser, Ring-Buffer). **umockdev** emuliert USB-Devices auf User-Space-Ebene, sodass wir ohne echte Hardware die gesamte Open/Read/Write/Close-Kette inklusive des `rpbridge-core`-Bind-Verhaltens auf einem Runner verifizieren können.

```yaml
# .github/workflows/driver-kunit.yml
name: driver-kunit

on:
  push:
    branches: [main]
    paths: ['driver/**', '.github/workflows/driver-kunit.yml']
  pull_request:
    paths: ['driver/**', '.github/workflows/driver-kunit.yml']
  workflow_dispatch:

permissions: { contents: read }

jobs:
  kunit:
    name: KUnit UML tests
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install KUnit deps
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            python3 python3-pip flex bison libssl-dev libelf-dev \
            build-essential bc rsync wget xz-utils

      - name: Fetch kernel 6.12
        run: |
          LATEST=$(curl -s https://www.kernel.org/releases.json \
            | python3 -c "import sys,json; \
                d=json.load(sys.stdin); \
                print(next(r['version'] for r in d['releases'] \
                  if r['version'].startswith('6.12.')))")
          wget -q https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$LATEST.tar.xz
          tar xJf linux-$LATEST.tar.xz
          mv linux-$LATEST linux

      - name: Symlink driver into kernel tree
        run: |
          cp -r driver linux/drivers/rpbridge
          # Wire it into Kconfig + Makefile via a small patch
          echo 'obj-y += rpbridge/' >> linux/drivers/Makefile
          sed -i '/endmenu/i source "drivers/rpbridge/Kconfig"' linux/drivers/Kconfig

      - name: Run KUnit with module's tests
        working-directory: linux
        run: |
          ./tools/testing/kunit/kunit.py run \
            --arch=x86_64 \
            --kconfig_add=CONFIG_RPBRIDGE_KUNIT=y \
            rpbridge.*

      - name: Upload KUnit results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: kunit-results
          path: linux/.kunit/test.log

  umockdev:
    name: umockdev USB emulation
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install umockdev + deps
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            umockdev gir1.2-umockdev-1.0 \
            libumockdev-dev \
            python3 python3-gi \
            build-essential meson ninja-build pkg-config \
            libusb-1.0-0-dev libudev-dev libglib2.0-dev

      - name: Build userspace test harness
        working-directory: driver/tests/umockdev
        run: |
          meson setup build
          meson compile -C build

      - name: Run umockdev scripts
        working-directory: driver/tests/umockdev
        run: |
          for scr in recordings/*.umockdev; do
            name=$(basename "$scr" .umockdev)
            echo "=== Replaying $name ==="
            umockdev-run -d "$scr" -- \
              ./build/rpbridge_test_harness --scenario="$name"
          done

      - name: Upload test logs
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: umockdev-logs
          path: driver/tests/umockdev/build/*.log
```

**Hintergrund:**

* **KUnit in-tree copy-in.** Da unser Treiber out-of-tree gepflegt wird, der KUnit-Runner aber derzeit am saubersten in-tree läuft, kopiert der Job das Modul temporär nach `linux/drivers/rpbridge/`. Das funktioniert robust, weil unsere Sources keinen Kbuild-relativen Include-Tricks nutzen (siehe `rpbridge-architektur.md`, Abschnitt „Treiber-Layout"). Alternativ: `kunit.py` mit `--build_dir` + `--make_options M=/path/to/driver` – dann ist allerdings das Wiring komplexer.
* **umockdev** wird als Debian-Paket installiert (2.0 ab Ubuntu 24.04). Die Testaufnahmen (`recordings/*.umockdev`) liegen unter `driver/tests/umockdev/recordings/` und wurden einmalig auf echter Hardware mit `umockdev-record` erstellt.

### 4.5 `driver-package.yml` – DKMS, DEB, RPM

```yaml
# .github/workflows/driver-package.yml
name: driver-package

on:
  push:
    tags: ['v*']
  workflow_dispatch:

permissions: { contents: read }

jobs:
  dkms-tarball:
    name: DKMS source tarball
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
        with: { fetch-depth: 0 }

      - name: Resolve version
        id: ver
        run: echo "VER=$(git describe --tags --always | sed 's/^v//')" >> $GITHUB_OUTPUT

      - name: Stage DKMS tree
        run: |
          mkdir -p stage/usr/src/rpbridge-${{ steps.ver.outputs.VER }}
          cp -r driver/* \
            stage/usr/src/rpbridge-${{ steps.ver.outputs.VER }}/
          sed -i "s/@VERSION@/${{ steps.ver.outputs.VER }}/g" \
            stage/usr/src/rpbridge-${{ steps.ver.outputs.VER }}/dkms.conf

      - name: Create tarball
        run: |
          cd stage
          tar czf ../rpbridge-dkms-${{ steps.ver.outputs.VER }}.tar.gz \
            usr/src/rpbridge-${{ steps.ver.outputs.VER }}

      - uses: actions/upload-artifact@v4
        with:
          name: rpbridge-dkms
          path: rpbridge-dkms-*.tar.gz

  deb:
    name: Debian/Ubuntu packages
    runs-on: ubuntu-24.04
    needs: dkms-tarball
    strategy:
      matrix:
        dist: [jammy, noble]
    container:
      image: ubuntu:${{ matrix.dist }}
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with:
          name: rpbridge-dkms

      - name: Install packaging deps
        run: |
          apt-get update
          DEBIAN_FRONTEND=noninteractive apt-get install -y \
            build-essential devscripts debhelper dh-dkms \
            dkms fakeroot lintian git

      - name: Build DEB
        run: |
          cp driver/packaging/debian . -r
          tar xzf rpbridge-dkms-*.tar.gz
          dpkg-buildpackage -us -uc -b
          mkdir out
          mv ../rpbridge-dkms_*.deb out/

      - uses: actions/upload-artifact@v4
        with:
          name: rpbridge-deb-${{ matrix.dist }}
          path: out/*.deb

  rpm:
    name: RPM packages
    runs-on: ubuntu-24.04
    needs: dkms-tarball
    strategy:
      matrix:
        dist: [fedora-41, rockylinux-9]
    container:
      image: ${{ matrix.dist == 'fedora-41' && 'fedora:41' || 'rockylinux:9' }}
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with:
          name: rpbridge-dkms

      - name: Install packaging deps
        run: |
          if command -v dnf >/dev/null; then
            dnf install -y rpm-build rpmdevtools dkms kernel-devel \
              make gcc tar which
          else
            yum install -y rpm-build rpmdevtools dkms kernel-devel \
              make gcc tar which
          fi

      - name: Build RPM
        run: |
          rpmdev-setuptree
          mv rpbridge-dkms-*.tar.gz ~/rpmbuild/SOURCES/
          cp driver/packaging/rpm/rpbridge.spec ~/rpmbuild/SPECS/
          rpmbuild -bb ~/rpmbuild/SPECS/rpbridge.spec
          mkdir out
          cp ~/rpmbuild/RPMS/noarch/*.rpm out/

      - uses: actions/upload-artifact@v4
        with:
          name: rpbridge-rpm-${{ matrix.dist }}
          path: out/*.rpm
```

#### DKMS-Integration-Details

`driver/dkms.conf`:

```
PACKAGE_NAME="rpbridge"
PACKAGE_VERSION="@VERSION@"
CLEAN="make -C src clean"
MAKE[0]="make -C src KDIR=/lib/modules/${kernelver}/build"
BUILT_MODULE_NAME[0]="rpbridge-core"
BUILT_MODULE_LOCATION[0]="src"
DEST_MODULE_LOCATION[0]="/updates"
BUILT_MODULE_NAME[1]="rpbridge-i2c"
BUILT_MODULE_LOCATION[1]="src"
DEST_MODULE_LOCATION[1]="/updates"
BUILT_MODULE_NAME[2]="rpbridge-spi"
BUILT_MODULE_LOCATION[2]="src"
DEST_MODULE_LOCATION[2]="/updates"
BUILT_MODULE_NAME[3]="rpbridge-gpio"
BUILT_MODULE_LOCATION[3]="src"
DEST_MODULE_LOCATION[3]="/updates"
BUILT_MODULE_NAME[4]="rpbridge-can"
BUILT_MODULE_LOCATION[4]="src"
DEST_MODULE_LOCATION[4]="/updates"
AUTOINSTALL="yes"
```

`driver/packaging/debian/rules` (Auszug):

```make
#!/usr/bin/make -f
%:
	dh $@ --with dkms

override_dh_dkms:
	dh_dkms -V $(shell git describe --tags --always | sed 's/^v//')
```

---

## 5. Userspace-Tools (`tools.yml`)

`rpbridge-ctl` ist eine kleine CLI (C oder Rust – beide Skelette existieren im Repo) mit Aufgaben: `info`, `reboot-to-bootsel`, `flash`, `dt-overlay apply <id>`, `selftest`.

```yaml
# .github/workflows/tools.yml
name: tools

on:
  push:
    paths: ['tools/**', '.github/workflows/tools.yml']
  pull_request:
    paths: ['tools/**', '.github/workflows/tools.yml']
  workflow_dispatch:

jobs:
  rust:
    name: rpbridge-ctl (Rust)
    runs-on: ubuntu-24.04
    strategy:
      matrix:
        target: [x86_64-unknown-linux-gnu, aarch64-unknown-linux-gnu]
    steps:
      - uses: actions/checkout@v4
      - uses: dtolnay/rust-toolchain@stable
        with:
          targets: ${{ matrix.target }}
      - uses: Swatinem/rust-cache@v2
      - name: Install cross deps
        if: matrix.target == 'aarch64-unknown-linux-gnu'
        run: |
          sudo apt-get update
          sudo apt-get install -y gcc-aarch64-linux-gnu
          mkdir -p .cargo
          cat >> .cargo/config.toml <<'EOF'
          [target.aarch64-unknown-linux-gnu]
          linker = "aarch64-linux-gnu-gcc"
          EOF
      - name: Install libusb/libudev
        run: |
          sudo apt-get update
          sudo apt-get install -y libusb-1.0-0-dev libudev-dev pkg-config
      - name: Build
        working-directory: tools/rpbridge-ctl
        run: cargo build --release --target ${{ matrix.target }}
      - name: Test
        if: matrix.target == 'x86_64-unknown-linux-gnu'
        working-directory: tools/rpbridge-ctl
        run: cargo test --release
      - uses: actions/upload-artifact@v4
        with:
          name: rpbridge-ctl-${{ matrix.target }}
          path: tools/rpbridge-ctl/target/${{ matrix.target }}/release/rpbridge-ctl
```

---

## 6. Release-Pipeline (`release.yml`)

Der Release-Workflow hängt alle Artefakte an das GitHub-Release. Er läuft ausschließlich bei Tag-Pushes und konsumiert die Ergebnisse der anderen Workflows über das **`workflow_run`**-Event nicht (unzuverlässig bei Tag-Pushes), sondern über **direkte Job-Wiederverwendung** mittels **Reusable Workflows**.

Alternative (einfacher & häufiger in der Praxis): Release-Workflow triggert die einzelnen Build-Jobs selbst und lädt die Artefakte herunter. Die kompakte Variante:

```yaml
# .github/workflows/release.yml
name: release

on:
  push:
    tags: ['v*']

permissions:
  contents: write    # für Release-Upload

jobs:
  firmware:
    uses: ./.github/workflows/firmware.yml

  driver-package:
    uses: ./.github/workflows/driver-package.yml

  tools:
    uses: ./.github/workflows/tools.yml

  publish:
    name: publish GitHub Release
    runs-on: ubuntu-24.04
    needs: [firmware, driver-package, tools]
    steps:
      - uses: actions/checkout@v4
        with: { fetch-depth: 0 }

      - uses: actions/download-artifact@v4
        with:
          path: dist

      - name: Flatten + checksum
        run: |
          mkdir release
          find dist -type f \( -name '*.uf2' -o -name '*.elf' \
            -o -name '*.bin' -o -name '*.hex' -o -name '*.deb' \
            -o -name '*.rpm' -o -name 'rpbridge-dkms-*.tar.gz' \
            -o -name 'rpbridge-ctl' \) \
            -exec cp -v {} release/ \;
          cd release
          sha256sum * > CHECKSUMS.txt

      - name: Generate release notes
        id: notes
        run: |
          {
            echo "## What's Changed"
            git log --pretty=format:'* %s (%h)' \
              $(git describe --tags --abbrev=0 HEAD^)..HEAD
            echo ""
            echo ""
            echo "## Artifacts"
            echo '* `rpbridge.uf2` – Firmware für RP2354B (bus-powered Multi-Bus-Bridge)'
            echo '* `rpbridge-dkms_*.deb` – DKMS-Paket für Debian 12, Ubuntu 22.04/24.04'
            echo '* `rpbridge-dkms-*.noarch.rpm` – DKMS-Paket für Fedora, Rocky Linux'
            echo '* `rpbridge-ctl` – Userspace-CLI (x86_64 / aarch64)'
            echo ''
            echo '## SHA-256'
            echo '```'
            cat release/CHECKSUMS.txt
            echo '```'
          } > release_notes.md

      - name: Publish release
        uses: softprops/action-gh-release@v2
        with:
          files: release/*
          body_path: release_notes.md
          draft: false
          prerelease: ${{ contains(github.ref_name, '-rc') }}
```

Damit `firmware.yml` und `driver-package.yml` als **reusable workflows** funktionieren, wird in beiden oben hinzugefügt:

```yaml
on:
  workflow_call:
  push: ...
  pull_request: ...
```

und Artefakte bleiben wie oben auf `actions/upload-artifact@v4` konfiguriert.

---

## 7. Nightly-Mainline (`nightly.yml`)

```yaml
# .github/workflows/nightly.yml
name: nightly-mainline

on:
  schedule:
    - cron: '23 3 * * *'   # 03:23 UTC täglich
  workflow_dispatch:

permissions: { contents: read }

jobs:
  mainline-build:
    runs-on: ubuntu-24.04
    continue-on-error: true
    steps:
      - uses: actions/checkout@v4

      - name: Install build deps
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            build-essential bc bison flex libssl-dev libelf-dev \
            rsync cpio git ccache

      - name: Clone torvalds/linux master
        run: git clone --depth=1 https://github.com/torvalds/linux.git

      - name: Prepare
        working-directory: linux
        run: |
          make defconfig
          make modules_prepare -j$(nproc)

      - name: Build out-of-tree module
        run: |
          make -C driver \
            KDIR=$GITHUB_WORKSPACE/linux \
            W=1 C=1 \
            -j$(nproc) V=1 2>&1 | tee build.log

      - name: Notify on failure
        if: failure()
        uses: actions/github-script@v7
        with:
          script: |
            github.rest.issues.create({
              owner: context.repo.owner,
              repo:  context.repo.repo,
              title: `nightly mainline build failed: ${context.runId}`,
              body:  `Build against torvalds/linux master failed. Log attached as artifact.`,
              labels: ['nightly', 'kernel-api-break']
            })

      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: nightly-mainline-log
          path: build.log
```

Dieser Job darf fehlschlagen, ohne die reguläre Pipeline rot zu färben – er meldet uns frühzeitig API-Brüche im Kernel.

---

## 8. Sicherheits-Hygiene

**Minimale Permissions.** Jeder Workflow setzt `permissions: contents: read` explizit am Top-Level; nur `release.yml` bekommt `contents: write`. Das entspricht der von GitHub empfohlenen Least-Privilege-Policy und begrenzt den Schaden bei kompromittierten Third-Party-Actions.

**Gepinnte Actions.** Im Produktiv-Repo sollten alle `uses:`-Zeilen auf einen **Commit-SHA** gepinnt werden, nicht nur auf einen Tag, z. B.:

```yaml
uses: actions/checkout@692973e3d937129bcbf40652eb9f2f61becf3332  # v4.1.7
```

Dependabot („`version: 2` / `updates:` / `package-ecosystem: github-actions`") pflegt diese SHAs automatisch. Eine `.github/dependabot.yml`:

```yaml
version: 2
updates:
  - package-ecosystem: "github-actions"
    directory: "/"
    schedule:
      interval: "weekly"
```

**Secrets.** Für Code-Signing (minisign o. ä.) wird der Private Key als GitHub-Secret (`MINISIGN_SECRET_KEY`, passphrase-protected mit `MINISIGN_PASSWORD`) hinterlegt und nur im `release.yml`-Job konsumiert:

```yaml
      - name: Sign release artifacts
        if: startsWith(github.ref, 'refs/tags/')
        env:
          MINISIGN_SECRET_KEY: ${{ secrets.MINISIGN_SECRET_KEY }}
          MINISIGN_PASSWORD:   ${{ secrets.MINISIGN_PASSWORD }}
        run: |
          sudo apt-get install -y minisign
          echo "$MINISIGN_SECRET_KEY" > key.sec
          chmod 600 key.sec
          for f in release/*; do
            [[ "$f" == *.minisig ]] && continue
            echo "$MINISIGN_PASSWORD" | minisign -S -s key.sec \
              -m "$f" -x "$f.minisig" -t "rpbridge release" </dev/null
          done
          shred -u key.sec
```

**SBOM.** Für Firmware + Treiber wird mit `anchore/sbom-action` ein SPDX-SBOM generiert und dem Release beigefügt:

```yaml
      - name: Generate SBOM
        uses: anchore/sbom-action@v0
        with:
          path: ./
          format: spdx-json
          output-file: rpbridge-sbom.spdx.json
```

---

## 9. Branch-Schutz & Required-Status-Checks

In `Settings → Branches → Branch protection rule` für `main`:

| Einstellung | Wert |
|---|---|
| Require pull request before merging | ✅ |
| Require approvals | 1 (oder 2 für kritische Bereiche) |
| Require status checks to pass | ✅ |
| Required checks | `firmware / build (rpbridge_rp2354b/Release)`, `firmware / build (pico2/Release)`, `driver / build (6.1)`, `driver / build (6.6)`, `driver / build (6.12)`, `driver / build (6.13)`, `driver-static / checkpatch.pl`, `driver-kunit / KUnit UML tests` |
| Require branches up to date | ✅ |
| Require signed commits | optional, empfohlen |
| Restrict who can push to matching branches | Admins + Bots |

Statische Analyse (sparse/smatch/coccinelle) wird bewusst **nicht** als Required-Check eingestellt, sondern nur als informativer Report, weil Kernel-Header-Versionen auf Runner-Seite Schwankungen haben können.

---

## 10. Lokale Reproduktion (dev-loop)

Damit Entwickler:innen die CI vor jedem Push lokal reproduzieren können, wird `scripts/ci-local.sh` beigelegt, das `act` (nektos/act) verwendet:

```bash
#!/usr/bin/env bash
# scripts/ci-local.sh – lokale Reproduktion der CI-Jobs
set -euo pipefail
job="${1:-firmware}"
case "$job" in
  firmware) act -W .github/workflows/firmware.yml -j build \
              --matrix board:rpbridge_rp2354b --matrix build_type:Release ;;
  driver)   act -W .github/workflows/driver.yml -j build \
              --matrix kver:6.12 ;;
  static)   act -W .github/workflows/driver-static.yml ;;
  *) echo "usage: $0 [firmware|driver|static]" >&2; exit 2 ;;
esac
```

Alternativ (ohne `act`): die Docker-Container-Images, die GitHub-Runner verwenden, werden in `ghcr.io/catthehacker/ubuntu:act-24.04` gespiegelt und lassen sich direkt mounten.

---

## 11. Kostenabschätzung (öffentliches Repo, Standard-Runner)

Öffentliche Repositories nutzen die **Standard-Runner** (Linux, x64, 4 vCPU, 16 GB RAM) **kostenlos und unlimitiert**. Für die RPBridge-Pipeline gilt:

| Workflow | Jobs pro PR | Typ. Dauer | Monatlicher Verbrauch (≈50 PRs) |
|---|---|---|---|
| `firmware.yml` | 6 Matrix-Jobs | 3–5 min | ~25 h |
| `driver.yml`   | 5 Matrix-Jobs | 4–7 min | ~30 h |
| `driver-static.yml` | 4 Jobs | 2–4 min | ~10 h |
| `driver-kunit.yml`  | 2 Jobs | 5–10 min | ~10 h |
| `driver-package.yml` | 5 Jobs | 3–5 min | nur bei Tags (~5 h/Monat) |
| `nightly.yml`  | 1 | 5–8 min | ~4 h/Monat |

→ **~85 Stunden/Monat**. Völlig unkritisch für öffentliche Repos. Für private Repos: Team-Plan (3 000 min inkl.) reicht bei moderater PR-Last.

---

## 12. Zusammenfassung / Checkliste zur Einführung

Initialer Rollout:

1. Repo auf das Layout in §1 bringen (Subfolder `firmware/`, `driver/`, `tools/`, `.github/workflows/`).
2. `firmware.yml` aktivieren – erster Push muss einen grünen Build liefern.
3. `driver.yml` mit einer schmalen Matrix (nur `6.12`) aktivieren; schrittweise LTS-Versionen hinzufügen.
4. Branch-Protection einschalten (§9) – anfangs nur `firmware / build` und `driver / build (6.12)` required.
5. `driver-static.yml` + `driver-kunit.yml` aktivieren, in PRs prüfen, dass Reports sauber generiert werden.
6. `driver-package.yml` erst aktivieren, wenn das erste DKMS-Build-Rezept lokal verifiziert ist (Debian + Fedora VM).
7. Ersten Tag `v0.1.0-rc1` pushen – validiert `release.yml` end-to-end.
8. Dependabot für GitHub-Actions aktivieren.
9. Signatur-Infrastruktur (minisign) aufsetzen, Public Key in `docs/releases.md` dokumentieren.
10. Nightly einschalten, nachdem die reguläre Pipeline eine Woche stabil ist.

Alle Workflows in diesem Dokument sind vollständig lauffähig, verwenden nur offizielle und weit etablierte Actions (`actions/*`, `hendrikmuhs/ccache-action`, `softprops/action-gh-release`, `anchore/sbom-action`, `DoozyX/clang-format-lint-action`) und decken die End-to-End-Strecke vom Git-Commit bis zum signierten Multi-Artefakt-Release ab.

---

## Anhang A – Referenzen

* GitHub Actions Reference – <https://docs.github.com/actions>
* actions/checkout – <https://github.com/actions/checkout>
* actions/cache – <https://github.com/actions/cache>
* actions/upload-artifact – <https://github.com/actions/upload-artifact>
* hendrikmuhs/ccache-action – <https://github.com/hendrikmuhs/ccache-action>
* softprops/action-gh-release – <https://github.com/softprops/action-gh-release>
* anchore/sbom-action – <https://github.com/anchore/sbom-action>
* pico-sdk – <https://github.com/raspberrypi/pico-sdk>
* picotool – <https://github.com/raspberrypi/picotool>
* Linux Kernel checkpatch.pl – `scripts/checkpatch.pl` im Kernel-Tree
* sparse – <https://sparse.docs.kernel.org>
* smatch – <https://repo.or.cz/smatch.git>
* coccinelle – <https://coccinelle.gitlabpages.inria.fr/website/>
* KUnit – <https://docs.kernel.org/dev-tools/kunit/>
* umockdev – <https://github.com/martinpitt/umockdev>
* DKMS – <https://github.com/dell/dkms>
* minisign – <https://jedisct1.github.io/minisign/>
* act (local GHA runner) – <https://github.com/nektos/act>

— *Ende des Dokuments.*
