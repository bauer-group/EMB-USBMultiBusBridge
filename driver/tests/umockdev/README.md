# umockdev replay tests

[umockdev](https://github.com/martinpitt/umockdev) records a real
USB device's sysfs state + ioctl traffic and replays it against an
unmodified driver running on a machine without the hardware present.
This is how we exercise `rpbridge.ko`'s USB-descriptor handling and
aux_device spawn logic in CI without shipping hardware to the
runner.

## Files

- `rpbridge-attach.umockdev` — `umockdev-record` capture of an
  RPBridge rev-B attaching to a Linux 6.13+ box. Regenerate from a
  real device (see "Recording" below) whenever the USB descriptor
  set changes.
- `rpbridge-attach.ioctl` — the matching ioctl script. umockdev-run
  needs both to replay bulk transfers.
- `replay.sh` — the test runner. Loads `rpbridge.ko` under
  `umockdev-wrapper`, asserts the expected aux_device children show
  up under `/sys/bus/auxiliary/devices/`, and diffs the output
  against `expected-children.txt`.
- `expected-children.txt` — reference list of aux_device nodes the
  driver MUST create given the recorded capability map.

## Running

```sh
cd driver/tests/umockdev
./replay.sh /path/to/rpbridge.ko
```

Exit status `0` → every expected child appeared. Non-zero → diff is
printed to stderr pointing at missing or unexpected children.

## Recording (once per descriptor-set change)

On a machine with real RPBridge rev-B hardware, the kernel source,
and umockdev ≥ 0.17:

```sh
# 1. Plug the device in and make sure rpbridge.ko is NOT loaded.
sudo modprobe -r rpbridge
# 2. Record sysfs + ioctl traffic.
sudo umockdev-record --output=rpbridge-attach.umockdev /dev/bus/usb/*/* > /dev/null
sudo umockdev-record -i rpbridge-attach.ioctl /dev/bus/usb/*/*
# 3. Commit both files. replay.sh will reproduce them.
```

umockdev is distributed under LGPL-2.1 — its output files embed
only the sysfs tree + ioctl trace of the recorded device, with no
host-specific data.

## What this layer CANNOT cover

- **Firmware behaviour.** The replay is deterministic; it cannot
  tell you if the firmware mis-handles an unexpected host command.
  That's what the E2E checklist exists for.
- **Device-side timing bugs.** Replay frames arrive synchronously;
  the real wire has jitter. Timing-sensitive bugs need hardware.
- **USB-stall / reset-recovery.** The recording is of the happy
  path; stall recovery needs fault injection, which is a separate
  `fault_umockdev` effort (future).

## When to regenerate the recording

- The firmware's capability map changes shape (new bus entry, new
  feature flag, renamed field).
- The USB descriptor set changes (new interface, string-index
  reshuffle, MS OS 2.0 GUID change).
- The device's VID/PID is reassigned (we're on pid.codes 1209:FFFE
  during dev; a real allocation will be a recording refresh).

If none of the above changed, an existing recording is valid
indefinitely — USB descriptors are immutable for the life of a
given firmware.
