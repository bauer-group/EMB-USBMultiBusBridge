// SPDX-License-Identifier: GPL-2.0
//
// Safe wrapper around Linux `pwm_chip` registration.

use core::ffi::c_void;
use core::ptr::NonNull;

use kernel::prelude::*;

use crate::ffi::bindings as cc;

#[derive(Copy, Clone, Debug)]
pub enum Polarity { Normal, Inversed }

pub struct PwmState {
    pub period_ns:  u64,
    pub duty_ns:    u64,
    pub enabled:    bool,
    pub polarity:   Polarity,
}

pub trait PwmBackend: Send + Sync {
    fn apply(&self, hwpwm: u32, state: &PwmState) -> i32;
    fn get_state(&self, hwpwm: u32) -> Result<PwmState, i32>;
}

pub struct ChipHandle {
    chip: NonNull<cc::pwm_chip>,
    backend_raw: *mut dyn PwmBackend,
}

unsafe impl Send for ChipHandle {}
unsafe impl Sync for ChipHandle {}

impl ChipHandle {
    pub fn register<B: PwmBackend + 'static>(
        parent: NonNull<cc::device>,
        npwm:   u32,
        backend: B,
    ) -> Result<Self, i32> {
        let boxed: Box<dyn PwmBackend> = Box::new(backend, GFP_KERNEL)
            .map_err(|_| cc::ENOMEM)?;
        let backend_raw: *mut dyn PwmBackend = Box::into_raw(boxed);

        // SAFETY: parent live; shim allocates.
        let chip = unsafe {
            cc::rpbridge_pwm_chip_new(
                parent.as_ptr(),
                npwm,
                backend_raw as *mut c_void,
            )
        };
        let chip = NonNull::new(chip).ok_or(cc::ENOMEM)?;
        // SAFETY: chip is freshly allocated.
        let rc = unsafe { cc::rpbridge_pwm_chip_register(chip.as_ptr()) };
        if rc != 0 {
            unsafe { cc::rpbridge_pwm_chip_free(chip.as_ptr()); }
            let _reclaimed = unsafe { Box::from_raw(backend_raw) };
            return Err(-rc);
        }
        Ok(ChipHandle { chip, backend_raw })
    }
}

impl Drop for ChipHandle {
    fn drop(&mut self) {
        // SAFETY: self.chip was returned by rpbridge_pwm_chip_new.
        unsafe { cc::rpbridge_pwm_chip_free(self.chip.as_ptr()); }
        let _reclaimed = unsafe { Box::from_raw(self.backend_raw) };
    }
}

/* ───── C → Rust trampolines ────────────────────────────────────── */

fn cookie_as_backend<'a>(cookie: *mut c_void) -> &'a dyn PwmBackend {
    // SAFETY: cookie is the backend_raw pointer stored in the chip.
    unsafe { &*(cookie as *const dyn PwmBackend) }
}

fn c_state_to_rust(c_state: *const cc::pwm_state) -> PwmState {
    // SAFETY: c_state is a kernel-side pwm_state we only read from.
    let period = unsafe { cc::rpbridge_pwm_state_period_ns(c_state as *mut _) };
    let duty   = unsafe { cc::rpbridge_pwm_state_duty_ns(c_state as *mut _) };
    let en     = unsafe { cc::rpbridge_pwm_state_enabled(c_state as *mut _) };
    let pol_i  = unsafe { cc::rpbridge_pwm_state_polarity(c_state as *mut _) };
    PwmState {
        period_ns: period,
        duty_ns:   duty,
        enabled:   en,
        polarity:  if pol_i == cc::PWM_POLARITY_INVERSED {
            Polarity::Inversed
        } else {
            Polarity::Normal
        },
    }
}

fn rust_state_to_c(state: &PwmState, c_state: *mut cc::pwm_state) {
    // SAFETY: c_state is a kernel-side pwm_state we are writing into.
    unsafe {
        cc::rpbridge_pwm_state_set_period(c_state, state.period_ns);
        cc::rpbridge_pwm_state_set_duty(c_state,   state.duty_ns);
        cc::rpbridge_pwm_state_set_enabled(c_state, state.enabled);
        cc::rpbridge_pwm_state_set_polarity(c_state, match state.polarity {
            Polarity::Normal   => cc::PWM_POLARITY_NORMAL,
            Polarity::Inversed => cc::PWM_POLARITY_INVERSED,
        });
    }
}

#[no_mangle]
pub extern "C" fn rpbridge_pwm_cb_apply(
    cookie: *mut c_void,
    hwpwm:  u32,
    state:  *const cc::pwm_state,
) -> i32 {
    let backend = cookie_as_backend(cookie);
    let st = c_state_to_rust(state);
    backend.apply(hwpwm, &st)
}

#[no_mangle]
pub extern "C" fn rpbridge_pwm_cb_get_state(
    cookie: *mut c_void,
    hwpwm:  u32,
    state:  *mut cc::pwm_state,
) -> i32 {
    let backend = cookie_as_backend(cookie);
    match backend.get_state(hwpwm) {
        Ok(st) => { rust_state_to_c(&st, state); 0 }
        Err(e) => -e,
    }
}
