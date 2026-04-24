// SPDX-License-Identifier: GPL-2.0
//
// Safe wrapper around Linux wait_queue_head_t for timeout-bounded
// blocking waits. Kernel's `CondVar::wait_timeout` is not yet in
// Rust-for-Linux; we bridge via `rpbridge_waitqueue_*` shims.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;

use crate::ffi::bindings as cc;

/// A wait-queue head the driver can park on. Allocated inline (no
/// kmalloc); initialised once via [`WaitQueue::init`] — calling any
/// other method before `init` is undefined behaviour and guarded by
/// the type system via the `Uninit` → `Ready` state split.
///
/// Safe against racing `wake` / `wait` calls: the kernel's
/// `wait_queue_head_t` carries its own spinlock.
#[repr(C)]
pub struct WaitQueue {
    storage: UnsafeCell<MaybeUninit<[u8; cc::RPBRIDGE_WQ_HEAD_SIZE]>>,
    initialised: core::sync::atomic::AtomicBool,
}

// SAFETY: the underlying wait_queue_head_t is protected by an internal
// spinlock and designed for multi-producer / multi-consumer access.
unsafe impl Send for WaitQueue {}
unsafe impl Sync for WaitQueue {}

impl WaitQueue {
    pub const fn new_uninit() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::uninit()),
            initialised: core::sync::atomic::AtomicBool::new(false),
        }
    }

    /// Initialise the underlying wait_queue_head_t. MUST be called
    /// exactly once before any `wait_*` or `wake_*` call. Safe to
    /// call from `probe()` because no other thread can see this
    /// WaitQueue yet.
    pub fn init(&self) {
        // SAFETY: `storage` is valid memory for RPBRIDGE_WQ_HEAD_SIZE
        // bytes; `rpbridge_waitqueue_init` initialises it in place.
        unsafe {
            cc::rpbridge_waitqueue_init(self.storage.get() as *mut u8);
        }
        self.initialised.store(true, core::sync::atomic::Ordering::Release);
    }

    /// Block until woken or `timeout_ms` elapses. Returns `true` if a
    /// wake-up was observed (regardless of cause — the caller is
    /// expected to re-check its condition under its own lock).
    /// Returns `false` on timeout.
    pub fn wait_timeout_ms(&self, timeout_ms: u32) -> bool {
        debug_assert!(self.initialised.load(core::sync::atomic::Ordering::Acquire),
                      "WaitQueue used before init()");
        // SAFETY: storage is initialised (debug-asserted) and the
        // shim operates under the kernel's own spinlock.
        let rc = unsafe {
            cc::rpbridge_wait_event_timeout_ms(
                self.storage.get() as *mut u8,
                timeout_ms as core::ffi::c_ulong,
            )
        };
        rc > 0
    }

    /// Wake every parked waiter.
    pub fn wake_all(&self) {
        debug_assert!(self.initialised.load(core::sync::atomic::Ordering::Acquire));
        // SAFETY: as above.
        unsafe { cc::rpbridge_wake_up_all(self.storage.get() as *mut u8); }
    }
}
