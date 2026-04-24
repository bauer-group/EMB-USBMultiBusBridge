// SPDX-License-Identifier: GPL-2.0
//
// Placeholder — auxiliary_device management is wrapped here in a
// later commit (M11d). For now, just re-export the bindings types
// so callers don't need to reach into ffi::bindings directly.

use crate::ffi::bindings as cc;

pub type AuxDevicePtr = *mut cc::auxiliary_device;
