// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// rpbridge-ctl — command-line control utility.
//
// The subcommand tree aims to cover everything a developer needs
// day-to-day without memorising bus-specific dance moves:
//
//   info                        — identify + capabilities dump
//   selftest                    — loopback + self-diagnostics
//   flash <firmware.uf2>        — BOOTSEL-trigger + picotool-load
//   reboot-bootsel              — just put the device into UF2 mode
//   i2c scan [--bus N]          — ACK sweep
//   i2c read  ... / i2c write   — single transaction
//   spi xfer                    — single transfer
//   gpio set / gpio get / gpio monitor
//   adc read <ch>
//   pwm set <ch> <freq> <duty>
//   log --follow                — stream the debug UART log
//
// The binary is deliberately thin — all heavy lifting lives in the
// `rpbridge` library crate so the same operations are reachable from
// Python/.NET/Node via FFI (planned; see workspace README).

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

#[derive(Parser, Debug)]
#[command(
    name = "rpbridge-ctl",
    version,
    about = "Command-line control for the RPBridge USB multi-bus bridge",
    long_about = "Issue RPBP v1 commands, perform firmware updates, scan \
                  buses, drive GPIOs and more — with the same tool on \
                  Linux, macOS and Windows."
)]
struct Cli {
    /// Select by serial number when multiple bridges are plugged in.
    #[arg(long, env = "RPBRIDGE_SERIAL")]
    serial: Option<String>,

    /// Verbose logging (repeat for more detail).
    #[arg(short, long, action = clap::ArgAction::Count)]
    verbose: u8,

    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Print identity and capability information.
    Info,

    /// Run the on-device self-test suite (loopbacks + sanity checks).
    Selftest,

    /// Reboot the device into its ROM BOOTSEL / UF2 mode.
    RebootBootsel,

    /// Reboot into BOOTSEL, flash the given UF2, and wait for re-enumeration.
    Flash {
        /// Path to the signed UF2 image.
        firmware: std::path::PathBuf,
    },

    /// Follow the debug UART log (CDC-ACM #3).
    Log {
        /// Keep following and print incremental output (like `tail -f`).
        #[arg(long)]
        follow: bool,
    },

    /// I²C operations.
    I2c {
        #[command(subcommand)]
        op: I2cOp,
    },

    /// GPIO operations.
    Gpio {
        #[command(subcommand)]
        op: GpioOp,
    },
}

#[derive(Subcommand, Debug)]
enum I2cOp {
    /// ACK-sweep the bus for responding 7-bit addresses.
    Scan {
        /// I²C bus index (0 or 1).
        #[arg(long, default_value_t = 0)]
        bus: u8,
    },
}

#[derive(Subcommand, Debug)]
enum GpioOp {
    /// Drive a GPIO line high or low.
    Set {
        /// Pin number on the RPBridge GPIO chip.
        pin: u8,
        /// `0` for low, `1` for high.
        value: u8,
    },
    /// Read a GPIO line level.
    Get {
        /// Pin number on the RPBridge GPIO chip.
        pin: u8,
    },
}

fn setup_logging(verbose: u8) {
    let filter = match verbose {
        0 => "warn",
        1 => "info",
        2 => "debug",
        _ => "trace",
    };
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::new(filter))
        .with_target(false)
        .compact()
        .init();
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    setup_logging(cli.verbose);

    match &cli.cmd {
        Cmd::Info => cmd_info(&cli),
        Cmd::Selftest => cmd_todo("selftest"),
        Cmd::RebootBootsel => cmd_todo("reboot-bootsel"),
        Cmd::Flash { firmware } => cmd_todo_flash(firmware),
        Cmd::Log { follow } => cmd_todo_log(*follow),
        Cmd::I2c { op } => match op {
            I2cOp::Scan { bus } => cmd_todo_i2c_scan(*bus),
        },
        Cmd::Gpio { op } => match op {
            GpioOp::Set { pin, value } => cmd_todo_gpio_set(*pin, *value),
            GpioOp::Get { pin } => cmd_todo_gpio_get(*pin),
        },
    }
}

fn cmd_info(_cli: &Cli) -> Result<()> {
    // Scaffold path: we cannot enumerate a real device yet. Report the
    // protocol version this tool was built for and the local tool
    // identity, then surface the not-yet-implemented error clearly.
    println!(
        "rpbridge-ctl {} (protocol RPBP v{}.{}.{})",
        env!("CARGO_PKG_VERSION"),
        rpbridge_protocol::VERSION,
        0,
        0
    );

    match rpbridge::Device::open_first() {
        Ok(dev) => {
            let caps = dev.capabilities();
            println!(
                "connected device : {}",
                caps.board.as_deref().unwrap_or("<unknown>")
            );
            println!(
                "firmware         : {}",
                caps.firmware.as_deref().unwrap_or("<unknown>")
            );
            Ok(())
        }
        Err(err) => Err(anyhow::anyhow!(err)).context("opening the bridge"),
    }
}

fn cmd_todo(what: &str) -> Result<()> {
    anyhow::bail!(
        "subcommand '{}' is not implemented yet in the scaffold — \
         tracked in the workspace roadmap",
        what
    )
}

fn cmd_todo_flash(_fw: &std::path::Path) -> Result<()> {
    cmd_todo("flash")
}

fn cmd_todo_log(_follow: bool) -> Result<()> {
    cmd_todo("log")
}

fn cmd_todo_i2c_scan(_bus: u8) -> Result<()> {
    cmd_todo("i2c scan")
}

fn cmd_todo_gpio_set(_pin: u8, _value: u8) -> Result<()> {
    cmd_todo("gpio set")
}

fn cmd_todo_gpio_get(_pin: u8) -> Result<()> {
    cmd_todo("gpio get")
}
