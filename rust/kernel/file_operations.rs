// SPDX-License-Identifier: GPL-2.0

//! File operations.
//!
//! C header: [`include/linux/fs.h`](../../../../include/linux/fs.h)

use core::convert::{TryFrom, TryInto};
use core::{marker, mem, ptr};

use alloc::boxed::Box;
use alloc::sync::Arc;

use crate::bindings;
use crate::c_types;
use crate::error::{Error, KernelResult};
use crate::user_ptr::{UserSlicePtr, UserSlicePtrReader, UserSlicePtrWriter};

/// Wraps the kernel's `struct file`.
///
/// # Invariants
///
/// The pointer [`File::ptr`] is non-null and valid.
pub struct File {
    ptr: *const bindings::file,
}

impl File {
    /// Constructs a new [`struct file`] wrapper.
    ///
    /// # Safety
    ///
    /// The pointer `ptr` must be non-null and valid for the lifetime of the object.
    unsafe fn from_ptr(ptr: *const bindings::file) -> File {
        // INVARIANTS: the safety contract ensures the type invariant will hold.
        File { ptr }
    }

    /// Returns the current seek/cursor/pointer position (`struct file::f_pos`).
    pub fn pos(&self) -> u64 {
        // SAFETY: `File::ptr` is guaranteed to be valid by the type invariants.
        unsafe { (*self.ptr).f_pos as u64 }
    }

    /// Returns whether the file is in blocking mode.
    pub fn is_blocking(&self) -> bool {
        // SAFETY: `File::ptr` is guaranteed to be valid by the type invariants.
        unsafe { (*self.ptr).f_flags & bindings::O_NONBLOCK == 0 }
    }
}

/// Equivalent to [`std::io::SeekFrom`].
///
/// [`std::io::SeekFrom`]: https://doc.rust-lang.org/std/io/enum.SeekFrom.html
pub enum SeekFrom {
    /// Equivalent to C's `SEEK_SET`.
    Start(u64),

    /// Equivalent to C's `SEEK_END`.
    End(i64),

    /// Equivalent to C's `SEEK_CUR`.
    Current(i64),
}

fn from_kernel_result<T>(r: KernelResult<T>) -> T
where
    T: TryFrom<c_types::c_int>,
    T::Error: core::fmt::Debug,
{
    match r {
        Ok(v) => v,
        Err(e) => T::try_from(e.to_kernel_errno()).unwrap(),
    }
}

macro_rules! from_kernel_result {
    ($($tt:tt)*) => {{
        from_kernel_result((|| {
            $($tt)*
        })())
    }};
}

unsafe extern "C" fn open_callback<T: FileOperations>(
    _inode: *mut bindings::inode,
    file: *mut bindings::file,
) -> c_types::c_int {
    from_kernel_result! {
        let ptr = T::open()?.into_pointer();
        (*file).private_data = ptr as *mut c_types::c_void;
        Ok(0)
    }
}

unsafe extern "C" fn read_callback<T: FileOperations>(
    file: *mut bindings::file,
    buf: *mut c_types::c_char,
    len: c_types::c_size_t,
    offset: *mut bindings::loff_t,
) -> c_types::c_ssize_t {
    from_kernel_result! {
        let mut data = UserSlicePtr::new(buf as *mut c_types::c_void, len)?.writer();
        let f = &*((*file).private_data as *const T);
        // No `FMODE_UNSIGNED_OFFSET` support, so `offset` must be in [0, 2^63).
        // See discussion in https://github.com/fishinabarrel/linux-kernel-module-rust/pull/113
        T::read(f, &File::from_ptr(file), &mut data, (*offset).try_into()?)?;
        let written = len - data.len();
        (*offset) += bindings::loff_t::try_from(written).unwrap();
        Ok(written.try_into().unwrap())
    }
}

unsafe extern "C" fn write_callback<T: FileOperations>(
    file: *mut bindings::file,
    buf: *const c_types::c_char,
    len: c_types::c_size_t,
    offset: *mut bindings::loff_t,
) -> c_types::c_ssize_t {
    from_kernel_result! {
        let mut data = UserSlicePtr::new(buf as *mut c_types::c_void, len)?.reader();
        let f = &*((*file).private_data as *const T);
        // No `FMODE_UNSIGNED_OFFSET` support, so `offset` must be in [0, 2^63).
        // See discussion in https://github.com/fishinabarrel/linux-kernel-module-rust/pull/113
        T::write(f, &mut data, (*offset).try_into()?)?;
        let read = len - data.len();
        (*offset) += bindings::loff_t::try_from(read).unwrap();
        Ok(read.try_into().unwrap())
    }
}

unsafe extern "C" fn release_callback<T: FileOperations>(
    _inode: *mut bindings::inode,
    file: *mut bindings::file,
) -> c_types::c_int {
    let ptr = mem::replace(&mut (*file).private_data, ptr::null_mut());
    T::release(T::Wrapper::from_pointer(ptr as _), &File::from_ptr(file));
    0
}

unsafe extern "C" fn llseek_callback<T: FileOperations>(
    file: *mut bindings::file,
    offset: bindings::loff_t,
    whence: c_types::c_int,
) -> bindings::loff_t {
    from_kernel_result! {
        let off = match whence as u32 {
            bindings::SEEK_SET => SeekFrom::Start(offset.try_into()?),
            bindings::SEEK_CUR => SeekFrom::Current(offset),
            bindings::SEEK_END => SeekFrom::End(offset),
            _ => return Err(Error::EINVAL),
        };
        let f = &*((*file).private_data as *const T);
        let off = T::seek(f, &File::from_ptr(file), off)?;
        Ok(off as bindings::loff_t)
    }
}

unsafe extern "C" fn unlocked_ioctl_callback<T: FileOperations>(
    file: *mut bindings::file,
    cmd: c_types::c_uint,
    arg: c_types::c_ulong,
) -> c_types::c_long {
    from_kernel_result! {
        let f = &*((*file).private_data as *const T);
        // SAFETY: This function is called by the kernel, so it must set `fs` appropriately.
        let mut cmd = IoctlCommand::new(cmd as _, arg as _);
        let ret = T::ioctl(f, &File::from_ptr(file), &mut cmd)?;
        Ok(ret as _)
    }
}

unsafe extern "C" fn compat_ioctl_callback<T: FileOperations>(
    file: *mut bindings::file,
    cmd: c_types::c_uint,
    arg: c_types::c_ulong,
) -> c_types::c_long {
    from_kernel_result! {
        let f = &*((*file).private_data as *const T);
        // SAFETY: This function is called by the kernel, so it must set `fs` appropriately.
        let mut cmd = IoctlCommand::new(cmd as _, arg as _);
        let ret = T::compat_ioctl(f, &File::from_ptr(file), &mut cmd)?;
        Ok(ret as _)
    }
}

unsafe extern "C" fn fsync_callback<T: FileOperations>(
    file: *mut bindings::file,
    start: bindings::loff_t,
    end: bindings::loff_t,
    datasync: c_types::c_int,
) -> c_types::c_int {
    from_kernel_result! {
        let start = start.try_into()?;
        let end = end.try_into()?;
        let datasync = datasync != 0;
        let f = &*((*file).private_data as *const T);
        let res = T::fsync(f, &File::from_ptr(file), start, end, datasync)?;
        Ok(res.try_into().unwrap())
    }
}

pub(crate) struct FileOperationsVtable<T>(marker::PhantomData<T>);

impl<T: FileOperations> FileOperationsVtable<T> {
    pub(crate) const VTABLE: bindings::file_operations = bindings::file_operations {
        open: Some(open_callback::<T>),
        release: Some(release_callback::<T>),
        read: if T::TO_USE.read {
            Some(read_callback::<T>)
        } else {
            None
        },
        write: if T::TO_USE.write {
            Some(write_callback::<T>)
        } else {
            None
        },
        llseek: if T::TO_USE.seek {
            Some(llseek_callback::<T>)
        } else {
            None
        },

        check_flags: None,
        compat_ioctl: if T::TO_USE.compat_ioctl {
            Some(compat_ioctl_callback::<T>)
        } else {
            None
        },
        copy_file_range: None,
        fallocate: None,
        fadvise: None,
        fasync: None,
        flock: None,
        flush: None,
        fsync: if T::TO_USE.fsync {
            Some(fsync_callback::<T>)
        } else {
            None
        },
        get_unmapped_area: None,
        iterate: None,
        iterate_shared: None,
        iopoll: None,
        lock: None,
        mmap: None,
        mmap_supported_flags: 0,
        owner: ptr::null_mut(),
        poll: None,
        read_iter: None,
        remap_file_range: None,
        sendpage: None,
        setlease: None,
        show_fdinfo: None,
        splice_read: None,
        splice_write: None,
        unlocked_ioctl: if T::TO_USE.ioctl {
            Some(unlocked_ioctl_callback::<T>)
        } else {
            None
        },
        write_iter: None,
    };
}

/// Represents which fields of [`struct file_operations`] should be populated with pointers.
pub struct ToUse {
    /// The `read` field of [`struct file_operations`].
    pub read: bool,

    /// The `write` field of [`struct file_operations`].
    pub write: bool,

    /// The `llseek` field of [`struct file_operations`].
    pub seek: bool,

    /// The `unlocked_ioctl` field of [`struct file_operations`].
    pub ioctl: bool,

    /// The `compat_ioctl` field of [`struct file_operations`].
    pub compat_ioctl: bool,

    /// The `fsync` field of [`struct file_operations`].
    pub fsync: bool,
}

/// A constant version where all values are to set to `false`, that is, all supported fields will
/// be set to null pointers.
pub const USE_NONE: ToUse = ToUse {
    read: false,
    write: false,
    seek: false,
    ioctl: false,
    compat_ioctl: false,
    fsync: false,
};

/// Defines the [`FileOperations::TO_USE`] field based on a list of fields to be populated.
#[macro_export]
macro_rules! declare_file_operations {
    () => {
        const TO_USE: $crate::file_operations::ToUse = $crate::file_operations::USE_NONE;
    };
    ($($i:ident),+) => {
        const TO_USE: kernel::file_operations::ToUse =
            $crate::file_operations::ToUse {
                $($i: true),+ ,
                ..$crate::file_operations::USE_NONE
            };
    };
}

/// Allows the handling of ioctls defined with the `_IO`, `_IOR`, `_IOW`, and `_IOWR` macros.
///
/// For each macro, there is a handler function that takes the appropriate types as arguments.
pub trait IoctlHandler: Sync {
    /// Handles ioctls defined with the `_IO` macro, that is, with no buffer as argument.
    fn pure(&self, _file: &File, _cmd: u32, _arg: usize) -> KernelResult<i32> {
        Err(Error::EINVAL)
    }

    /// Handles ioctls defined with the `_IOR` macro, that is, with an output buffer provided as
    /// argument.
    fn read(&self, _file: &File, _cmd: u32, _writer: &mut UserSlicePtrWriter) -> KernelResult<i32> {
        Err(Error::EINVAL)
    }

    /// Handles ioctls defined with the `_IOW` macro, that is, with an input buffer provided as
    /// argument.
    fn write(
        &self,
        _file: &File,
        _cmd: u32,
        _reader: &mut UserSlicePtrReader,
    ) -> KernelResult<i32> {
        Err(Error::EINVAL)
    }

    /// Handles ioctls defined with the `_IOWR` macro, that is, with a buffer for both input and
    /// output provided as argument.
    fn read_write(&self, _file: &File, _cmd: u32, _data: UserSlicePtr) -> KernelResult<i32> {
        Err(Error::EINVAL)
    }
}

/// Represents an ioctl command.
///
/// It can use the components of an ioctl command to dispatch ioctls using
/// [`IoctlCommand::dispatch`].
pub struct IoctlCommand {
    cmd: u32,
    arg: usize,
    user_slice: Option<UserSlicePtr>,
}

impl IoctlCommand {
    /// Constructs a new [`IoctlCommand`].
    ///
    /// # Safety
    ///
    /// The caller must ensure that `fs` is compatible with `arg` and the original caller's
    /// context. For example, if the original caller is from userland (e.g., through the ioctl
    /// syscall), then `arg` is untrusted and `fs` should therefore be `USER_DS`.
    unsafe fn new(cmd: u32, arg: usize) -> Self {
        let user_slice = {
            let dir = (cmd >> bindings::_IOC_DIRSHIFT) & bindings::_IOC_DIRMASK;
            if dir == bindings::_IOC_NONE {
                None
            } else {
                let size = (cmd >> bindings::_IOC_SIZESHIFT) & bindings::_IOC_SIZEMASK;

                // SAFETY: We only create one instance of the user slice, so TOCTOU issues are not
                // possible. The `set_fs` requirements are imposed on the caller.
                UserSlicePtr::new(arg as _, size as _).ok()
            }
        };

        Self {
            cmd,
            arg,
            user_slice,
        }
    }

    /// Dispatches the given ioctl to the appropriate handler based on the value of the command. It
    /// also creates a [`UserSlicePtr`], [`UserSlicePtrReader`], or [`UserSlicePtrWriter`]
    /// depending on the direction of the buffer of the command.
    ///
    /// It is meant to be used in implementations of [`FileOperations::ioctl`] and
    /// [`FileOperations::compat_ioctl`].
    pub fn dispatch<T: IoctlHandler>(&mut self, handler: &T, file: &File) -> KernelResult<i32> {
        let dir = (self.cmd >> bindings::_IOC_DIRSHIFT) & bindings::_IOC_DIRMASK;
        if dir == bindings::_IOC_NONE {
            return T::pure(handler, file, self.cmd, self.arg);
        }

        let data = self.user_slice.take().ok_or(Error::EFAULT)?;
        const READ_WRITE: u32 = bindings::_IOC_READ | bindings::_IOC_WRITE;
        match dir {
            bindings::_IOC_WRITE => T::write(handler, file, self.cmd, &mut data.reader()),
            bindings::_IOC_READ => T::read(handler, file, self.cmd, &mut data.writer()),
            READ_WRITE => T::read_write(handler, file, self.cmd, data),
            _ => Err(Error::EINVAL),
        }
    }

    /// Returns the raw 32-bit value of the command and the ptr-sized argument.
    pub fn raw(&self) -> (u32, usize) {
        (self.cmd, self.arg)
    }
}

/// Corresponds to the kernel's `struct file_operations`.
///
/// You implement this trait whenever you would create a `struct file_operations`.
///
/// File descriptors may be used from multiple threads/processes concurrently, so your type must be
/// [`Sync`].
pub trait FileOperations: Sync + Sized {
    /// The methods to use to populate [`struct file_operations`].
    const TO_USE: ToUse;

    /// The pointer type that will be used to hold ourselves.
    type Wrapper: PointerWrapper<Self>;

    /// Creates a new instance of this file.
    ///
    /// Corresponds to the `open` function pointer in `struct file_operations`.
    fn open() -> KernelResult<Self::Wrapper>;

    /// Cleans up after the last reference to the file goes away.
    ///
    /// Note that the object is moved, so it will be freed automatically unless the implementation
    /// moves it elsewhere.
    ///
    /// Corresponds to the `release` function pointer in `struct file_operations`.
    fn release(_obj: Self::Wrapper, _file: &File) {}

    /// Reads data from this file to userspace.
    ///
    /// Corresponds to the `read` function pointer in `struct file_operations`.
    fn read(&self, _file: &File, _data: &mut UserSlicePtrWriter, _offset: u64) -> KernelResult {
        Err(Error::EINVAL)
    }

    /// Writes data from userspace to this file.
    ///
    /// Corresponds to the `write` function pointer in `struct file_operations`.
    fn write(&self, _data: &mut UserSlicePtrReader, _offset: u64) -> KernelResult<isize> {
        Err(Error::EINVAL)
    }

    /// Changes the position of the file.
    ///
    /// Corresponds to the `llseek` function pointer in `struct file_operations`.
    fn seek(&self, _file: &File, _offset: SeekFrom) -> KernelResult<u64> {
        Err(Error::EINVAL)
    }

    /// Performs IO control operations that are specific to the file.
    ///
    /// Corresponds to the `unlocked_ioctl` function pointer in `struct file_operations`.
    fn ioctl(&self, _file: &File, _cmd: &mut IoctlCommand) -> KernelResult<i32> {
        Err(Error::EINVAL)
    }

    /// Performs 32-bit IO control operations on that are specific to the file on 64-bit kernels.
    ///
    /// Corresponds to the `compat_ioctl` function pointer in `struct file_operations`.
    fn compat_ioctl(&self, _file: &File, _cmd: &mut IoctlCommand) -> KernelResult<i32> {
        Err(Error::EINVAL)
    }

    /// Syncs pending changes to this file.
    ///
    /// Corresponds to the `fsync` function pointer in `struct file_operations`.
    fn fsync(&self, _file: &File, _start: u64, _end: u64, _datasync: bool) -> KernelResult<u32> {
        Err(Error::EINVAL)
    }
}

/// Used to convert an object into a raw pointer that represents it.
///
/// It can eventually be converted back into the object. This is used to store objects as pointers
/// in kernel data structures, for example, an implementation of [`FileOperations`] in `struct
/// file::private_data`.
pub trait PointerWrapper<T> {
    /// Returns the raw pointer.
    fn into_pointer(self) -> *const T;

    /// Returns the instance back from the raw pointer.
    ///
    /// # Safety
    ///
    /// The passed pointer must come from a previous call to [`PointerWrapper::into_pointer()`].
    unsafe fn from_pointer(ptr: *const T) -> Self;
}

impl<T> PointerWrapper<T> for Box<T> {
    fn into_pointer(self) -> *const T {
        Box::into_raw(self)
    }

    unsafe fn from_pointer(ptr: *const T) -> Self {
        Box::<T>::from_raw(ptr as _)
    }
}

impl<T> PointerWrapper<T> for Arc<T> {
    fn into_pointer(self) -> *const T {
        Arc::into_raw(self)
    }

    unsafe fn from_pointer(ptr: *const T) -> Self {
        Arc::<T>::from_raw(ptr)
    }
}
