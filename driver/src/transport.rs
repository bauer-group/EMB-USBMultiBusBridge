// SPDX-License-Identifier: GPL-2.0
//
// RPBP transport layer — the kernel-unaware bookkeeping that sits
// between the subsystem clients and the USB URB pump. Two pieces:
//
//   * `Sequencer` — per-channel monotonic u16 seq allocator that wraps
//     at 65 536 and honours the RPBP spec's "seq != last + 1 → EPROTO"
//     rule.
//
//   * `CompletionTable` — fixed-size table of outstanding requests.
//     Each slot carries the (channel, seq) key, a state machine, and
//     either the awaited payload or an error. Slots are allocated on
//     request submission and released on response consumption.
//
// Sync is handled by the caller (the kernel adapter wraps this in a
// kernel::sync::Mutex plus a CondVar for waiters). The logic here is
// intentionally pure so it can be exhaustively tested from `cargo
// test` on a host build — see the unit tests at the bottom.

use kernel::prelude::*;

use crate::rpbp::{CHAN_CONTROL, Status};

/* ───── Sequencer ──────────────────────────────────────────────────── */

/// Per-channel monotonic sequence generator. The spec says `seq` is
/// 16-bit and wraps at 65 536. One instance per channel; the control
/// channel and stream channels all get their own.
pub struct Sequencer {
    next: u16,
}

impl Sequencer {
    pub const fn new() -> Self { Self { next: 0 } }

    /// Allocate the next seq and advance. Wraps mod 2¹⁶.
    pub fn take(&mut self) -> u16 {
        let s = self.next;
        self.next = self.next.wrapping_add(1);
        s
    }

    /// Reset to 0 — called on RESET_CHANNEL.
    pub fn reset(&mut self) { self.next = 0; }

    /// Peek the next seq without advancing — diagnostic only.
    #[cfg(test)]
    pub fn peek(&self) -> u16 { self.next }
}

/* ───── Completion table ───────────────────────────────────────────── */

/// How many control-channel requests can be in flight at once.
/// Sized to match the firmware's dispatch fan-out. If a client tries
/// to submit a 17th request while 16 are pending, it blocks until a
/// slot frees up or its timeout fires.
pub const MAX_INFLIGHT: usize = 16;

/// A returnable index into the completion table. The caller keeps this
/// until it takes the result. Plain `u8` instead of a wrapped newtype
/// so it packs into `Option<SlotIdx>` as a single byte, keeping the
/// slot-index state cheap on the hot path.
pub type SlotIdx = u8;

#[derive(Debug)]
pub enum SlotState {
    /// Waiting for the device's CMD_RESPONSE / ERROR frame.
    Pending,
    /// Device acknowledged, payload is ready for the caller.
    Completed { status: Status, result: KVec<u8> },
    /// Slot was timed-out or cancelled before the device replied.
    /// We keep the slot allocated so a late response lands on a
    /// known-dead slot instead of corrupting a new request that
    /// reused the (channel, seq) key.
    Dead,
}

struct Slot {
    channel: u16,
    seq:     u16,
    state:   SlotState,
}

pub struct CompletionTable {
    slots: [Option<Slot>; MAX_INFLIGHT],
}

impl CompletionTable {
    pub const fn new() -> Self {
        // `None` for each slot — `[None; N]` requires Copy on the
        // Option, which doesn't compose with KVec. Build element-wise.
        const NONE: Option<Slot> = None;
        Self { slots: [NONE; MAX_INFLIGHT] }
    }

    /// Reserve a slot for a new in-flight request on `channel` with
    /// the given `seq`. Returns the slot index, or `EBUSY` if all
    /// slots are in use.
    pub fn allocate(&mut self, channel: u16, seq: u16) -> Result<SlotIdx, i32> {
        for (i, slot) in self.slots.iter_mut().enumerate() {
            if slot.is_none() {
                *slot = Some(Slot {
                    channel, seq,
                    state: SlotState::Pending,
                });
                return Ok(i as SlotIdx);
            }
        }
        Err(crate::errno::EBUSY)
    }

    /// Device replied — mark the matching (channel, seq) slot as
    /// completed and stash the payload. Returns the slot index, or
    /// `ENOENT` when no slot matches (late response, slot timed out
    /// or never existed). Callers typically wake the corresponding
    /// waiter after this returns Ok.
    pub fn fulfill(
        &mut self, channel: u16, seq: u16,
        status: Status, result: KVec<u8>,
    ) -> Result<SlotIdx, i32> {
        for (i, slot_opt) in self.slots.iter_mut().enumerate() {
            let Some(slot) = slot_opt else { continue };
            if slot.channel != channel || slot.seq != seq { continue; }
            if !matches!(slot.state, SlotState::Pending) {
                // Already completed or marked dead — drop the payload.
                return Err(crate::errno::ENOENT);
            }
            slot.state = SlotState::Completed { status, result };
            return Ok(i as SlotIdx);
        }
        Err(crate::errno::ENOENT)
    }

    /// Take the result out of a completed slot and free it. Returns
    /// the status + payload. `EAGAIN` if the slot is still pending;
    /// `ENOENT` if the slot is out of range or already freed;
    /// `ECANCELED` if the slot was timed out.
    pub fn take_result(&mut self, idx: SlotIdx)
        -> Result<(Status, KVec<u8>), i32>
    {
        let i = idx as usize;
        if i >= MAX_INFLIGHT { return Err(crate::errno::ENOENT); }
        let Some(slot) = self.slots[i].as_mut() else {
            return Err(crate::errno::ENOENT);
        };
        match core::mem::replace(&mut slot.state, SlotState::Dead) {
            SlotState::Completed { status, result } => {
                self.slots[i] = None;
                Ok((status, result))
            }
            SlotState::Pending => {
                // Put the state back — caller checked too early.
                slot.state = SlotState::Pending;
                Err(crate::errno::EAGAIN)
            }
            SlotState::Dead => {
                self.slots[i] = None;
                Err(crate::errno::ECANCELED)
            }
        }
    }

    /// Time-out: mark a slot dead so a late response doesn't corrupt
    /// a reused key, but keep the slot allocated for the lifetime of
    /// the in-flight frame on the wire. The caller must call
    /// `reap_dead()` once it's sure no late response can arrive
    /// (typically after a channel reset).
    pub fn cancel(&mut self, idx: SlotIdx) {
        let i = idx as usize;
        if i >= MAX_INFLIGHT { return; }
        if let Some(slot) = self.slots[i].as_mut() {
            slot.state = SlotState::Dead;
        }
    }

    /// Drop all dead slots. Called after a channel reset frees any
    /// outstanding seq numbers.
    pub fn reap_dead(&mut self, channel: u16) {
        for slot_opt in self.slots.iter_mut() {
            let keep = match slot_opt {
                Some(slot) if slot.channel == channel &&
                              matches!(slot.state, SlotState::Dead) => false,
                _ => true,
            };
            if !keep { *slot_opt = None; }
        }
    }

    /// Diagnostic helper — how many slots are live (pending, completed
    /// or dead).
    pub fn in_use(&self) -> usize {
        self.slots.iter().filter(|s| s.is_some()).count()
    }
}

/* ───── Convenience: one-channel round-trip handle ─────────────────── */

/// A typed wrapper the kernel adapter hands to the subsystem children.
/// They call `round_trip()` which blocks until either a response comes
/// back or the timeout fires. The adapter plugs in a real wait-queue
/// under `#[cfg(not(test))]`; the `#[cfg(test)]` path uses a
/// crossbeam-style rendezvous for deterministic tests.
///
/// The real kernel implementation lives in `core_driver.rs`; this type
/// is declared here so its public surface is versioned alongside the
/// transport bookkeeping.
pub trait RoundTrip {
    /// Submit `request` (already framed) and block until the device
    /// responds. The returned bytes are the CMD_RESPONSE payload,
    /// *without* the subsys/opcode/status header.
    fn round_trip(&self, channel: u16, request: &[u8])
        -> Result<(Status, KVec<u8>), i32>;

    /// Submit without waiting (fire-and-forget). Used for
    /// RESET_CHANNEL and stream credit refills.
    fn submit_oneway(&self, channel: u16, frame: &[u8]) -> Result<(), i32>;
}

/* ───── MockRoundTrip — test utility shared by every subsystem ──────── */

/// A deterministic `RoundTrip` impl for kernel-side unit tests. Each
/// test pre-programs a sequence of canned responses; submitted
/// requests are captured verbatim so assertions can verify the
/// builder layer emitted the right bytes.
///
/// Usage:
///
///     let mock = Arc::new(MockRoundTrip::new());
///     mock.queue_response(Status::Ok, b"\x01\x02");  // canned payload
///     let client = Client::new(mock.clone());
///     client.some_op()?;
///     let req = mock.pop_request();
///     assert_eq!(req, vec![/* expected bytes */]);
#[cfg(any(test, feature = "mock-transport"))]
pub struct MockRoundTrip {
    inner: kernel::sync::Mutex<MockInner>,
}

#[cfg(any(test, feature = "mock-transport"))]
struct MockInner {
    /// Queued responses, FIFO. Popped off index 0 on each round_trip.
    pub responses:         KVec<(Status, KVec<u8>)>,
    /// Captured requests in submission order.
    pub captured_requests: KVec<KVec<u8>>,
    pub oneway_captured:   KVec<KVec<u8>>,
    /// Index into `responses` of the next slot to consume. Avoids the
    /// O(n) cost of remove(0) while still emulating FIFO semantics.
    pub response_cursor:   usize,
    pub request_cursor:    usize,
}

#[cfg(any(test, feature = "mock-transport"))]
impl MockRoundTrip {
    pub fn new() -> Self {
        Self {
            inner: kernel::new_mutex!(
                MockInner {
                    responses:         KVec::new(),
                    captured_requests: KVec::new(),
                    oneway_captured:   KVec::new(),
                    response_cursor:   0,
                    request_cursor:    0,
                },
                "mock"
            ),
        }
    }

    pub fn queue_response(&self, status: Status, payload: &[u8]) {
        let mut v = KVec::new();
        v.extend_from_slice(payload, GFP_KERNEL).unwrap();
        let mut g = self.inner.lock();
        let _ = g.responses.push((status, v), GFP_KERNEL);
    }

    /// Pop the next captured request in FIFO order. Returns an empty
    /// KVec if no requests are pending — tests should explicitly
    /// check `request_count` before asserting byte content.
    pub fn pop_request(&self) -> KVec<u8> {
        let mut g = self.inner.lock();
        let cursor = g.request_cursor;
        if cursor >= g.captured_requests.len() {
            return KVec::new();
        }
        g.request_cursor = cursor + 1;
        // Clone the captured bytes rather than drain the Vec — this
        // keeps the Vec stable so callers can inspect later.
        let mut out = KVec::new();
        let src = g.captured_requests.as_slice()[cursor].as_slice();
        let _ = out.extend_from_slice(src, GFP_KERNEL);
        out
    }

    pub fn request_count(&self) -> usize {
        let g = self.inner.lock();
        g.captured_requests.len() - g.request_cursor
    }

    pub fn oneway_count(&self) -> usize {
        self.inner.lock().oneway_captured.len()
    }
}

#[cfg(any(test, feature = "mock-transport"))]
impl RoundTrip for kernel::sync::Arc<MockRoundTrip> {
    fn round_trip(&self, _channel: u16, request: &[u8])
        -> Result<(Status, KVec<u8>), i32>
    {
        let mut captured = KVec::new();
        captured.extend_from_slice(request, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        let mut g = self.inner.lock();
        let _ = g.captured_requests.push(captured, GFP_KERNEL);
        let cursor = g.response_cursor;
        if cursor >= g.responses.len() {
            return Err(crate::errno::EIO);
        }
        g.response_cursor = cursor + 1;
        // Clone out — avoid moving out of the KVec which would need
        // swap_remove gymnastics.
        let (status, payload) = &g.responses.as_slice()[cursor];
        let mut payload_copy = KVec::new();
        payload_copy.extend_from_slice(payload.as_slice(), GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        Ok((*status, payload_copy))
    }

    fn submit_oneway(&self, _channel: u16, frame: &[u8]) -> Result<(), i32> {
        let mut captured = KVec::new();
        captured.extend_from_slice(frame, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        let mut g = self.inner.lock();
        let _ = g.oneway_captured.push(captured, GFP_KERNEL);
        Ok(())
    }
}

/* ───── Tests ──────────────────────────────────────────────────────── */

#[cfg(test)]
mod tests {
    use super::*;

    fn kv(bytes: &[u8]) -> KVec<u8> {
        let mut v = KVec::new();
        v.extend_from_slice(bytes, GFP_KERNEL).unwrap();
        v
    }

    #[test]
    fn sequencer_wraps() {
        let mut s = Sequencer::new();
        for i in 0u16..=u16::MAX { assert_eq!(s.take(), i); }
        assert_eq!(s.take(), 0);
    }

    #[test]
    fn sequencer_reset_returns_to_zero() {
        let mut s = Sequencer::new();
        s.take(); s.take(); s.take();
        s.reset();
        assert_eq!(s.take(), 0);
    }

    #[test]
    fn allocate_fulfill_take_happy_path() {
        let mut t = CompletionTable::new();
        let idx = t.allocate(CHAN_CONTROL, 42).unwrap();
        assert_eq!(t.in_use(), 1);

        let _ = t.fulfill(CHAN_CONTROL, 42, Status::Ok, kv(b"hello")).unwrap();
        let (status, result) = t.take_result(idx).unwrap();
        assert!(matches!(status, Status::Ok));
        assert_eq!(result.as_slice(), b"hello");
        assert_eq!(t.in_use(), 0);
    }

    #[test]
    fn fulfill_unknown_returns_enoent() {
        let mut t = CompletionTable::new();
        assert_eq!(
            t.fulfill(CHAN_CONTROL, 42, Status::Ok, kv(b"x")).unwrap_err(),
            crate::errno::ENOENT,
        );
    }

    #[test]
    fn take_before_fulfill_returns_eagain() {
        let mut t = CompletionTable::new();
        let idx = t.allocate(CHAN_CONTROL, 42).unwrap();
        assert_eq!(
            t.take_result(idx).unwrap_err(),
            crate::errno::EAGAIN,
        );
    }

    #[test]
    fn cancel_then_late_fulfill_is_dropped() {
        let mut t = CompletionTable::new();
        let idx = t.allocate(CHAN_CONTROL, 42).unwrap();
        t.cancel(idx);
        assert!(t.fulfill(CHAN_CONTROL, 42, Status::Ok, kv(b"x")).is_err());
    }

    #[test]
    fn table_rejects_when_full() {
        let mut t = CompletionTable::new();
        for seq in 0..MAX_INFLIGHT { t.allocate(CHAN_CONTROL, seq as u16).unwrap(); }
        assert_eq!(
            t.allocate(CHAN_CONTROL, 999).unwrap_err(),
            crate::errno::EBUSY,
        );
    }

    #[test]
    fn reap_dead_frees_slots() {
        let mut t = CompletionTable::new();
        let a = t.allocate(CHAN_CONTROL, 1).unwrap();
        let _b = t.allocate(CHAN_CONTROL, 2).unwrap();
        t.cancel(a);
        t.reap_dead(CHAN_CONTROL);
        assert_eq!(t.in_use(), 1);
    }

    #[test]
    fn take_already_taken_returns_enoent() {
        let mut t = CompletionTable::new();
        let idx = t.allocate(CHAN_CONTROL, 5).unwrap();
        t.fulfill(CHAN_CONTROL, 5, Status::Ok, kv(b"x")).unwrap();
        t.take_result(idx).unwrap();
        assert!(t.take_result(idx).is_err());
    }

    #[test]
    fn cancel_then_take_returns_ecanceled() {
        let mut t = CompletionTable::new();
        let idx = t.allocate(CHAN_CONTROL, 7).unwrap();
        t.cancel(idx);
        assert_eq!(
            t.take_result(idx).unwrap_err(),
            crate::errno::ECANCELED,
        );
    }

    #[test]
    fn mismatched_channel_does_not_fulfill() {
        let mut t = CompletionTable::new();
        let idx = t.allocate(CHAN_CONTROL, 42).unwrap();
        // Same seq on a different channel must not land on our slot.
        assert!(t.fulfill(99, 42, Status::Ok, kv(b"x")).is_err());
        // Original slot still pending.
        assert_eq!(t.take_result(idx).unwrap_err(), crate::errno::EAGAIN);
    }
}
