// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// rpbridge-bench — latency / throughput benchmarks for RPBridge.
//
// Emits a `benchmark.json` file the CI pipeline feeds into Grafana for
// regression alerting (see docs/drafts/rpbridge-protocol-and-modern-stack.md
// §7 and docs/drafts/rpbridge-cicd.md). Keeping this out of the CLI
// avoids cluttering the user-facing tool with measurement knobs.

use anyhow::Result;
use clap::{Parser, Subcommand};

#[derive(Parser, Debug)]
#[command(
    name = "rpbridge-bench",
    version,
    about = "Latency + throughput benchmarks against a live RPBridge device"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Measure RPBP PING round-trip latency distribution.
    Ping {
        /// Number of PING/PONG pairs to issue.
        #[arg(long, default_value_t = 1000)]
        count: u32,
        /// Payload size in bytes (ignored on PING, kept for API parity).
        #[arg(long, default_value_t = 0)]
        size: u16,
    },
    /// Measure bulk throughput on the vendor RPBP pipe.
    Bulk {
        /// Total bytes to move.
        #[arg(long, default_value_t = 10 * 1024 * 1024)]
        bytes: u64,
    },
}

fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::new("info"))
        .compact()
        .init();

    let cli = Cli::parse();

    match cli.cmd {
        Cmd::Ping { count, size } => {
            tracing::info!("ping: count={count} size={size} (scaffold — no device yet)");
            anyhow::bail!("device transport is not implemented in the scaffold")
        }
        Cmd::Bulk { bytes } => {
            tracing::info!("bulk: bytes={bytes} (scaffold — no device yet)");
            anyhow::bail!("device transport is not implemented in the scaffold")
        }
    }
}
