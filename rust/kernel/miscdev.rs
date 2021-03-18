// SPDX-License-Identifier: GPL-2.0

//! Miscellaneous devices.
//!
//! C header: [`include/linux/miscdevice.h`](../../../../include/linux/miscdevice.h)
//!
//! Reference: <https://www.kernel.org/doc/html/latest/driver-api/misc_devices.html>

use crate::error::{Error, KernelResult};
use crate::file_operations::{FileOperations, FileOperationsVtable};
use crate::{bindings, c_types, CStr};
use alloc::boxed::Box;
use core::marker::PhantomPinned;
use core::pin::Pin;

/// A registration of a miscellaneous device.
pub struct Registration {
    mdev: Option<bindings::miscdevice>,
    _pin: PhantomPinned,
}

impl Registration {
    /// Creates a new [`Registration`] but does not register it yet.
    ///
    /// It is allowed to move.
    pub fn new() -> Self {
        Self {
            mdev: None,
            _pin: PhantomPinned,
        }
    }

    /// Registers a miscellaneous device.
    ///
    /// Returns a pinned heap-allocated representation of the registration.
    pub fn new_pinned<T: FileOperations>(
        name: CStr<'static>,
        minor: Option<i32>,
    ) -> KernelResult<Pin<Box<Self>>> {
        let mut r = Pin::from(Box::try_new(Self::new())?);
        r.as_mut().register::<T>(name, minor)?;
        Ok(r)
    }

    /// Registers a miscellaneous device with the rest of the kernel.
    ///
    /// It must be pinned because the memory block that represents the
    /// registration is self-referential. If a minor is not given, the kernel
    /// allocates a new one if possible.
    pub fn register<T: FileOperations>(
        self: Pin<&mut Self>,
        name: CStr<'static>,
        minor: Option<i32>,
    ) -> KernelResult {
        // SAFETY: We must ensure that we never move out of `this`.
        let this = unsafe { self.get_unchecked_mut() };
        if this.mdev.is_some() {
            // Already registered.
            return Err(Error::EINVAL);
        }

        this.mdev = Some(bindings::miscdevice::default());
        let dev = this.mdev.as_mut().unwrap();
        dev.fops = &FileOperationsVtable::<T>::VTABLE;
        dev.name = name.as_ptr() as *const c_types::c_char;
        dev.minor = minor.unwrap_or(bindings::MISC_DYNAMIC_MINOR as i32);
        let ret = unsafe { bindings::misc_register(dev) };
        if ret < 0 {
            this.mdev = None;
            return Err(Error::from_kernel_errno(ret));
        }
        Ok(())
    }
}

impl Default for Registration {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: The only method is `register()`, which requires a (pinned) mutable
// `Registration`, so it is safe to pass `&Registration` to multiple threads
// because it offers no interior mutability.
unsafe impl Sync for Registration {}

impl Drop for Registration {
    /// Removes the registration from the kernel if it has completed
    /// successfully before.
    fn drop(&mut self) {
        if let Some(ref mut dev) = self.mdev {
            unsafe {
                bindings::misc_deregister(dev);
            }
        }
    }
}
