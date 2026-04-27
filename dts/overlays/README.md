# Device Tree overlays

These overlays describe I²C/SPI clients that are **hard-wired on the
RPBridge PCB itself** (rev-B has none in v1, but future daughterboards
will). When the driver binds and the capability map advertises
`"features": ["dt-overlay"]`, the firmware ships a flattened overlay
blob alongside; the driver applies it with `of_overlay_fdt_apply()`
so the on-board clients become standard DT nodes the kernel can
match against.

Skip this directory entirely if you only plug into the user
connectors (Qwiic, PMOD, user header) — those sensors are handled
through the hot-plug path (`echo sensor 0x48 > /sys/bus/i2c/devices/i2c-N/new_device`)
or via udev rules, no DT overlay required.

## Naming convention

```
dts/overlays/
├── rpbridge-common.dtsi        # base: defines /rpbridge-i2c-0, /rpbridge-i2c-1, …
├── <daughterboard-name>.dts    # overlay for one specific add-on
└── README.md                   # this file
```

Compile with `dtc -I dts -O dtb -@ -o out.dtbo src.dts` on the host;
the resulting `.dtbo` is linked into the firmware as a BLOB and
served through the capability response.

## Example — BME280 on I²C bus 0 @ 0x76

`bme280-on-i2c0.dts`:

```dts
/dts-v1/;
/plugin/;

/ {
    fragment@0 {
        target-path = "/rpbridge-i2c-0";
        __overlay__ {
            #address-cells = <1>;
            #size-cells = <0>;

            bme280@76 {
                compatible = "bosch,bme280";
                reg = <0x76>;
            };
        };
    };
};
```

## Fallback for non-DT hosts

`CONFIG_OF_OVERLAY=y` is disabled in many desktop distributions. In
that case the driver falls back to calling
`i2c_new_client_device()` / `spi_new_device()` directly, driven by a
smaller compatibility table carried in the capability map. See the
ADR once it's written — for now, implement both paths.
