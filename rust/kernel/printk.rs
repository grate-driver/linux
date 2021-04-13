// SPDX-License-Identifier: GPL-2.0

//! Printing facilities.
//!
//! C header: [`include/linux/printk.h`](../../../../include/linux/printk.h)
//!
//! Reference: <https://www.kernel.org/doc/html/latest/core-api/printk-basics.html>

use core::cmp;
use core::fmt;

use crate::bindings;
use crate::c_types::c_int;

#[doc(hidden)]
pub fn printk(s: &[u8]) {
    // Do not copy the trailing `NUL` from `KERN_INFO`.
    let mut fmt_str = [0; bindings::KERN_INFO.len() - 1 + b"%.*s\0".len()];
    fmt_str[..bindings::KERN_INFO.len() - 1]
        .copy_from_slice(&bindings::KERN_INFO[..bindings::KERN_INFO.len() - 1]);
    fmt_str[bindings::KERN_INFO.len() - 1..].copy_from_slice(b"%.*s\0");

    // TODO: I believe `printk` never fails.
    unsafe { bindings::printk(fmt_str.as_ptr() as _, s.len() as c_int, s.as_ptr()) };
}

// From `kernel/print/printk.c`.
const LOG_LINE_MAX: usize = 1024 - 32;

#[doc(hidden)]
pub struct LogLineWriter {
    data: [u8; LOG_LINE_MAX],
    pos: usize,
}

impl LogLineWriter {
    /// Creates a new [`LogLineWriter`].
    pub fn new() -> LogLineWriter {
        LogLineWriter {
            data: [0u8; LOG_LINE_MAX],
            pos: 0,
        }
    }

    /// Returns the internal buffer as a byte slice.
    pub fn as_bytes(&self) -> &[u8] {
        &self.data[..self.pos]
    }
}

impl Default for LogLineWriter {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Write for LogLineWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let copy_len = cmp::min(LOG_LINE_MAX - self.pos, s.as_bytes().len());
        self.data[self.pos..self.pos + copy_len].copy_from_slice(&s.as_bytes()[..copy_len]);
        self.pos += copy_len;
        Ok(())
    }
}

/// Prints to the kernel console at `KERN_INFO` level.
///
/// Mimics the interface of [`std::println!`].
///
/// [`std::println!`]: https://doc.rust-lang.org/std/macro.println.html
#[macro_export]
macro_rules! println {
    () => ({
        $crate::printk::printk("\n".as_bytes());
    });
    ($fmt:expr) => ({
        $crate::printk::printk(concat!($fmt, "\n").as_bytes());
    });
    ($fmt:expr, $($arg:tt)*) => ({
        use ::core::fmt;
        let mut writer = $crate::printk::LogLineWriter::new();
        let _ = fmt::write(&mut writer, format_args!(concat!($fmt, "\n"), $($arg)*)).unwrap();
        $crate::printk::printk(writer.as_bytes());
    });
}
