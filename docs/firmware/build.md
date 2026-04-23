# Firmware — building and flashing

```sh
git submodule update --init --recursive firmware/third_party/pico-sdk \
                                        firmware/third_party/can2040

cmake -S firmware -B firmware/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build --parallel
```

Artefacts land in `firmware/build/`:

| File               | Purpose                                                |
|--------------------|--------------------------------------------------------|
| `rpbridge.uf2`     | Mass-storage drag-and-drop target for the BOOTSEL bootloader. |
| `rpbridge.elf`     | With debug info; for `gdb`, SEGGER, picoprobe.         |
| `rpbridge.bin`     | Raw flash image, useful with external programmers.     |
| `rpbridge.hex`     | Intel HEX, for factory programmers.                    |
| `rpbridge.dis`     | Disassembly listing (release builds only).             |
| `size.txt`         | `arm-none-eabi-size` summary.                          |

## Flash options

| Method                          | Command                                             |
|---------------------------------|-----------------------------------------------------|
| BOOTSEL mass-storage            | Copy `.uf2` to the `RPI-RP2` volume.                |
| `picotool`                      | `picotool load -fxv firmware/build/rpbridge.uf2`    |
| From userspace CLI              | `rpbridge-ctl flash firmware/build/rpbridge.uf2`    |
| SWD debugger (picoprobe, J-Link)| `openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "program rpbridge.elf verify reset exit"` |

## Cross-platform

The build works on Linux, macOS and Windows (WSL recommended on
Windows). Toolchain is the same on every host — `arm-none-eabi-gcc`
plus CMake and Ninja.
