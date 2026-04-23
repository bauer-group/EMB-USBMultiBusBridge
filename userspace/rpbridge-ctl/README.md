# rpbridge-ctl

Command-line companion for the RPBridge USB multi-bus bridge.

```sh
rpbridge-ctl info
rpbridge-ctl i2c scan --bus 0
rpbridge-ctl gpio set 30 1
rpbridge-ctl flash firmware.uf2
rpbridge-ctl log --follow
```

## Installation

```sh
cargo install --path .                    # from this repo
cargo install rpbridge-ctl                # from crates.io (once published)
```

The binary ships as a single static-linked artefact — no runtime
dependency on libusb unless the `rusb-backend` feature is explicitly
chosen.

## Scripting

`rpbridge-ctl` exits non-zero on any error, prints structured output
on success, and supports `--serial SN` to disambiguate when multiple
bridges are plugged in. Combine with `jq` / `yq` for pipelines.
