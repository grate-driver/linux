========================================================
Read-only file-based authenticity protection (fs-verity)
========================================================

Introduction
============

fs-verity (``fs/verity/``) is a library that filesystems can hook into
to support transparent integrity and authenticity protection of
read-only files.  Currently, it is supported by the ext4 and f2fs
filesystems.  Similar to fscrypt, not too much filesystem-specific
code is needed to support fs-verity.

fs-verity is similar to `dm-verity
<https://www.kernel.org/doc/Documentation/device-mapper/verity.txt>`_
but works on files rather than block devices.  On supported
filesystems, userspace can append a Merkle tree (hash tree) to a file,
then use an ioctl to enable fs-verity on it.  Then, the filesystem
transparently verifies all data read from the file against the Merkle
tree; reads that fail verification will fail.  The filesystem also
hides or moves the Merkle tree, and forbids changes to the file's
contents via the syscall interface.

Essentially, fs-verity is a way of efficiently hashing a file, subject
to the caveat that the enforcement of that hash happens on-demand as
reads occur.  The file hash that fs-verity computes is called the
"file measurement"; this is the hash of the Merkle tree's root hash
and certain other fs-verity metadata, and it takes constant time to
compute regardless of the size of the file.  Note: the value of the
fs-verity file measurement will differ from a regular hash of the
file, even when they use the same hash algorithm, e.g. SHA-256;
however, they achieve the same purpose.

Use cases
=========

In general, fs-verity does not replace or obsolete dm-verity.
dm-verity should still be used when it is possible to authenticate the
full block device, i.e. when the device is read-only.  fs-verity is
intended for use on read-write filesystems where dm-verity cannot be
used.

fs-verity is most useful for hashing large files where only a small
portion may be accessed.  For example, it's useful on Android
application package (APK) files, which typically contain many
translations, classes, and other resources that are infrequently or
even never accessed on a particular device.  It would be wasteful to
hash the entire file before starting the application.

Unlike an ahead-of-time hash, fs-verity also re-verifies data each
time it's paged in, which ensures the file measurement remains
correctly enforced even if the file contents are modified from
underneath the filesystem, e.g. by malicious disk firmware.

fs-verity can support various use cases, such as:

- Integrity protection (detecting accidental corruption)
- Auditing (logging file hashes before use)
- Authenticity protection (detecting malicious modification)

Note that the latter two are not features of fs-verity per se, but
rather fs-verity is a tool for supporting these use cases.  For
example, for the overall system to actually provide authenticity
protection, the file measurement itself must still be authenticated,
e.g. by comparing it with a known good value or by verifying a digital
signature of it.

This can be userspace driven, in which case fs-verity will only be
used (essentially) as a fast way of hashing the file contents, via the
`FS_IOC_MEASURE_VERITY`_ ioctl.  For authenticity protection, trusted
userspace code [#]_ still must verify the relevant portions of the
untrusted filesystem state before it is used in a security-critical
way, such as executing code from it.

For example, the trusted userspace code might verify that the file
located at ``/foo/bar/baz`` has an fs-verity file measurement of
``sha256:a83d5cd722ef0d070b23353c2d9f316c38425114da8bd007cb9e8499371a97b3``,
or that all security-critical files (e.g. executable code) have stored
alongside them a valid digital signature (signed by a known, trusted
public key) of their fs-verity file measurement, potentially combined
with other important file metadata such as path and SELinux label.

However, for ease of use, a subset of this policy logic (but not all
of it!) is also supported in the kernel by the `Built-in signature
verification`_ mechanism.  Support for fs-verity file hashes in IMA
(Integrity Measurement Architecture) policies is also planned.

.. [#] For example, on Android, "trusted userspace code" would be code
       running from the system or vendor partitions, which are
       read-only partitions authenticated by dm-verity tied into
       Verified Boot, as opposed to the userdata partition which is
       read-write.

Metadata format
===============

Merkle tree
-----------

fs-verity uses the same Merkle tree (hash tree) format as dm-verity;
the only difference is that fs-verity's Merkle tree is built over the
contents of a regular file rather than a block device.

Briefly, the file contents is divided into blocks, where the blocksize
is configurable but usually 4096 bytes.  The last block is zero-padded
if needed.  Each block is then hashed, producing the first level of
hashes.  Then, the hashes in this first level are grouped into
'blocksize'-byte blocks (zero-padding the ends as needed) and these
blocks are hashed, producing the second level of hashes.  This
proceeds up the tree until only a single block remains.  The hash of
this block is called the "Merkle tree root hash".  Note: if the entire
file contents fit in one block, then there are no hash blocks and the
"Merkle tree root hash" is simply the hash of the data block.

The blocks of the Merkle tree are stored on-disk starting from the
root level and then proceeding to store each level down to the "first"
(the level that gives the hashes of the data blocks).

The hash algorithm is configurable.  The default is SHA-256, but
SHA-512 is also supported.  The non-cryptographic checksum CRC-32C is
also supported for integrity-only use cases such as detecting bit
errors in read-only backup files.  A non-cryptographic checksum must
not be used if authenticity protection is desired.

In the recommended configuration of SHA-256 and 4K blocks, 128 hash
values fit in each block.  Thus, each level of the hash tree is 128
times smaller than the previous, and for large files the Merkle tree's
size converges to approximately 1/129 of the original file size.
However, for small files, the padding to a block boundary is
significant, making the space overhead proportionally more.

fs-verity descriptor
--------------------

For each file, fs-verity also uses an additional on-disk metadata
structure called the *fs-verity descriptor*.  This contains the
properties of the Merkle tree and some other information.  It begins
with a header in the following format::

    struct fsverity_descriptor {
            __u8 magic[8];
            __u8 major_version;
            __u8 minor_version;
            __u8 log_data_blocksize;
            __u8 log_tree_blocksize;
            __le16 data_algorithm;
            __le16 tree_algorithm;
            __le32 flags;
            __le32 reserved1;
            __le64 orig_file_size;
            __le16 auth_ext_count;
            __u8 reserved2[30];
    };

This structure contains:

- ``magic`` is the ASCII bytes "FSVerity".
- ``major_version`` is 1.
- ``minor_version`` is 0.
- ``log_data_blocksize`` and ``log_tree_blocksize`` are the log base 2
  of the block size (in bytes) of data blocks and Merkle tree blocks,
  respectively.  Currently, in both cases the kernel only supports
  page-sized blocks, i.e. on most architectures, 4096-byte blocks.
  Thus, usually both of these fields must be 12.
- ``data_algorithm`` and ``tree_algorithm`` are the hash algorithms
  used to hash data blocks and Merkle tree blocks, respectively.
  Currently the kernel requires these to have the same value.  The
  recommended value is FS_VERITY_ALG_SHA256.  See
  ``include/uapi/linux/fsverity.h`` for the list of allowed values.
- ``orig_file_size`` is the original size of the file in bytes.  This
  means the size excluding the verity metadata and padding.
- ``auth_ext_count`` is the number of authenticated extensions that
  follow.
- All other fields are zeroed.

Following the ``struct fsverity_descriptor``, there is a list of
"authenticated extensions".  Each extension is a variable-length
structure that begins with the following header::

    struct fsverity_extension {
            __le32 length;
            __le16 type;
            __le16 reserved;
    };

This structure contains:

- ``length`` is the length of this extension in bytes, including the
  header.
- ``type`` is the extension number.  See
  ``include/uapi/linux/fsverity.h`` for the allowed values.
- ``reserved`` must be 0.

Each extension begins on an 8-byte aligned boundary.  When an
extension's length is not a multiple of 8, it must be zero-padded to
the next 8-byte boundary, even if it is the last extension.  This zero
padding is not counted in the ``length`` field.

This first list of extensions is "authenticated", meaning that they
are included in the file measurement.  Currently, the following
authenticated extensions are supported.  Except where otherwise
indicated, extensions are optional and cannot be given multiple times:

- FS_VERITY_EXT_ROOT_HASH:  This is mandatory.  It gives the root hash
  of the Merkle tree, as a byte array.
- FS_VERITY_EXT_SALT: A salt to salt the hashes with, given as a byte
  array.  The salt is prepended to every block that is hashed.  Any
  length salt is supported.  Note that using a unique salt for every
  file should make it more difficult for fs-verity to be attacked
  across many files.  However, in principle this is unnecessary since
  simply choosing a strong cryptographic hash algorithm such as
  SHA-256 or SHA-512 should be sufficient.

Following the authenticated extensions, there is a list of
unauthenticated extensions.  These are *not* included in the file
measurement.  This list begins with a header::

        __le16 unauth_ext_count;
        __le16 padding[3];

``unauth_ext_count`` is the number of unauthenticated extensions.
This may be 0.

Like authenticated extensions, each unauthenticated extension begins
with the header ``struct fsverity_extension`` from above.

The following types of unauthenticated extensions are supported:

- FS_VERITY_EXT_PKCS7_SIGNATURE.  This is a DER-encoded PKCS#7 message
  containing the signed file measurement.  See `Built-in signature
  verification`_ for details.

fsveritysetup format
--------------------

When enabling fs-verity on a file via the `FS_IOC_ENABLE_VERITY`_
ioctl, the kernel requires that the verity metadata has been appended
to the file contents.  Specifically, the file must be arranged as:

#. Original file contents
#. Zero-padding to next block boundary
#. `Merkle tree`_
#. `fs-verity descriptor`_
#. fs-verity footer

We call this file format the "fsveritysetup format".  It is not
necessarily the on-disk format actually used by the filesystem, since
the filesystem is free to move things around during the ioctl.
However, the easiest way to implement fs-verity is to just keep this
arrangement in-place, as ext4 and f2fs do; see `Filesystem support`_.

Note that "block" here means the fs-verity block size, which is not
necessarily the same as the filesystem's block size.  For example, on
ext4, fs-verity can use 4K blocks on top of a filesystem formatted to
use a 1K block size.

The fs-verity footer is a structure of the following format::

    struct fsverity_footer {
            __le32 desc_reverse_offset;
            __u8 magic[8];
    };

``desc_reverse_offset`` is the distance in bytes from the end of the
fs-verity footer to the beginning of the fs-verity descriptor; this
allows software to find the fs-verity descriptor.  ``magic`` is the
ASCII bytes "FSVerity"; this allows software to quickly identify a
file as being in the "fsveritysetup" format as well as find the
fs-verity footer if zeroes have been appended.

The kernel cannot handle fs-verity footers that cross a page boundary.
Padding must be prepended as needed to meet this constaint.

Filesystem support
==================

ext4
----

ext4 supports fs-verity since kernel version TODO.

CONFIG_EXT4_FS_VERITY must be enabled in the kernel config.  Also, the
filesystem must have been formatted with ``-O verity``, or had
``tune2fs -O verity`` run on it.  These require e2fsprogs v1.44.4-2 or
later.  This e2fsprogs version is also required for e2fsck to
understand the verity feature.  Since "verity" is an RO_COMPAT
feature, once enabled earlier kernels will be unable to mount the
filesystem for writing, and earlier versions of e2fsck will be unable
to check the filesystem.

ext4 only allows fs-verity on extent-based files.

The EXT4_VERITY_FL flag in the inode is used to indicate that the
inode uses fs-verity.  This bit cannot be set directly; it can only be
set indirectly via `FS_IOC_ENABLE_VERITY`_.

When enabling verity on an inode, ext4 leaves the verity metadata
in-place in the `fsveritysetup format`_.  However, it changes the
on-disk i_size to the original file size, which allows the verity
feature to be RO_COMPAT rather than INCOMPAT.  Later, the fs-verity
footer is found by scanning backwards from the end of the last extent
rather than from i_size.

f2fs
----

f2fs supports fs-verity since kernel version TODO.

CONFIG_F2FS_FS_VERITY must be enabled in the kernel config.  Also, the
filesystem must have been formatted with ``-O verity``.  This requires
f2fs-tools v1.11.0 or later.

The FADVISE_VERITY_BIT flag in the inode is used to indicate that the
inode uses fs-verity.  This bit cannot be set directly; it can only be
set indirectly via `FS_IOC_ENABLE_VERITY`_.

When enabling verity on an inode, f2fs leaves the verity metadata
in-place in the `fsveritysetup format`_.  It leaves the on-disk i_size
as the full file size; however, the in-memory i_size is overridden
with the original size.

User API
========

FS_IOC_ENABLE_VERITY
--------------------

The FS_IOC_ENABLE_VERITY ioctl enables fs-verity on a regular file.
Userspace must have already appended verity metadata to the file,
using the file format described in `fsveritysetup format`_.
Additionally, the filesystem must support fs-verity.

The argument parameter for this ioctl is reserved and must be NULL.

This ioctl checks for write access to the inode; no capability is
required.  However, it must be executed on an O_RDONLY file
descriptor, and no processes may have the file open for writing.
(This is necessary to prevent various race conditions.)

On success, this ioctl returns 0, and the file becomes a verity file.
This means that:

- The filesystem marks the file as a verity file both in-memory and
  on-disk, e.g. by setting a bit in the inode.
- All later reads from the file are verified against the Merkle tree.
- The verity metadata at the end of the file is hidden or moved.
- Opening the file for writing or truncating it is no longer allowed.
- There is no way to disable verity on the file, other than by
  deleting it and replacing it with a copy.

If this ioctl fails, then no changes are made to the file.  The
reasons it might fail include:

- ``EACCES``: the process does not have write access to the file
- ``EBADMSG``: the file's fs-verity metadata is invalid
- ``EEXIST``: the file already has fs-verity enabled
- ``EINVAL``: a value was specified for the reserved argument
  parameter, or the file descriptor refers to neither a regular file
  nor a directory
- ``EIO``: an I/O error occurred
- ``EISDIR``: the file descriptor refers to a directory, not a regular
  file
- ``ENOTTY``: this type of filesystem does not implement fs-verity
- ``EOPNOTSUPP``: the kernel was not configured with fs-verity support
  for this filesystem, or the filesystem superblock has not had the
  'verity' feature enabled on it.  (See `Filesystem support`_.)
- ``EPERM``: the file is append-only
- ``EROFS``: the filesystem is read-only
- ``ETXTBSY``: the file is open for writing.  Note that this can be
  the caller's file descriptor, or another open file descriptor, or
  the file reference held by a writable memory map.

FS_IOC_MEASURE_VERITY
---------------------

The FS_IOC_MEASURE_VERITY ioctl retrieves the fs-verity measurement of
a regular file.  This is a digest that cryptographically summarizes
the file contents that are being enforced on reads.  The file must
have fs-verity enabled.

This ioctl takes in a pointer to a variable-length structure::

    struct fsverity_digest {
            __u16 digest_algorithm;
            __u16 digest_size; /* input/output */
            __u8 digest[];
    };

``digest_size`` is an input/output field.  On input, it must be
initialized to the number of bytes allocated for the variable-length
``digest`` field.

On success, 0 is returned and the kernel fills in the structure as
follows:

- ``digest_algorithm`` will be the hash algorithm used for the file
  measurement.  It will match the algorithm used in the Merkle tree,
  e.g. FS_VERITY_ALG_SHA256.  See ``include/uapi/linux/fsverity.h``
  for the list of possible values.
- ``digest_size`` will be the size of the digest in bytes, e.g. 32
  for SHA-256.  (This can be redundant with ``digest_algorithm``.)
- ``digest`` will be the actual bytes of the digest.

This ioctl is guaranteed to be very fast.  Due to fs-verity's use of a
Merkle tree, its running time is independent of the file size.

This ioctl can fail with the following errors:

- ``EFAULT``: invalid buffer was specified
- ``ENODATA``: the file is not a verity file
- ``ENOTTY``: this type of filesystem does not implement fs-verity
- ``EOPNOTSUPP``: the kernel was not configured with fs-verity support
  for this filesystem, or the filesystem superblock has not had the
  'verity' feature enabled on it.  (See `Filesystem support`_.)
- ``EOVERFLOW``: the file measurement is longer than the specified
  ``digest_size`` bytes.  Try providing a larger buffer.

Access semantics
================

fs-verity only implements reads, not writes.  Therefore, after it is
enabled on a given file, regardless of the mode bits filesystems will
forbid opening the file for writing as well as changing the size of
the file via truncate().  The error code received for this is EPERM.

However, fs-verity does not measure metadata such as owner, mode,
timestamps, and xattrs.  Therefore, changes to these are still
allowed.

For read-only access, fs-verity is intended to be transparent; no
changes to userspace applications should be needed.  However, astute
users may notice some slight differences in behavior:

- Direct I/O is not supported on verity files.  Attempts to use direct
  I/O on such files will fall back to buffered I/O.

- DAX (Direct Access) is not supported on verity files.

Note: read-only mmaps are supported, as is combining fs-verity and
fscrypt.

Verity files can be sparse; holes are still verified.

In-kernel policies
==================

Built-in signature verification
-------------------------------

With CONFIG_FS_VERITY_BUILTIN_SIGNATURES=y, fs-verity supports putting
a portion of an authentication policy (see `Use cases`_) in the
kernel.  Specifically, it adds support for:

1. At fs-verity module initialization time, a keyring ".fs-verity" is
   created.  The root user can add trusted X.509 certificates to this
   keyring using the add_key() system call, then (when done)
   optionally use keyctl_restrict_keyring() to prevent additional
   certificates from being added.

2. When a PKCS7_SIGNATURE extension containing a signed file
   measurement is found in a file's verity metadata, the kernel will
   verify this signature against the certificates in the ".fs-verity"
   keyring, and verify that it matches the actual file measurement.
   The extension must contain the PKCS#7 formatted signature in DER
   format, where the signed data is the file measurement as a ``struct
   fsverity_digest`` as described for `FS_IOC_MEASURE_VERITY`_ except
   that all fields must be little-endian rather than native endian.

3. A new sysctl "fs.verity.require_signatures" is made available.
   When set to 1, the kernel requires that all fs-verity files have a
   correctly signed file measurement as described in (2).

This is meant as a relatively simple mechanism that can be used to
provide some level of authenticity protection for fs-verity files, as
an alternative to doing the signature verification in userspace or
using IMA-appraisal.  However, with this mechanism, userspace programs
still need to check that the fs-verity bit is set, and there is no
protection against fs-verity files being swapped around.

Implementation details
======================

I/O path design
---------------

To support fs-verity, the filesystem's ``->readpage()`` and
``->readpages()`` methods are modified to verify the data pages before
they are marked Uptodate.  Merely hooking ``->read_iter()`` would be
insufficient, since ``->read_iter()`` is not used for memory maps.
fs-verity exposes functions to verify data:

- ``fsverity_verify_page()`` verifies an individual page
- ``fsverity_verify_bio()`` verifies all pages in a bio

Currently, fs-verity only supports the case where data blocks, hash
blocks, and pages all have the same size (usually 4096 bytes).

Filesystems that use bios call ``fsverity_verify_bio()`` after each
read bio completes.  To do this while also continuing to support
encryption (fscrypt), filesystems allocate a "post-read context" for
each bio and store it in ``->bi_private``::

    struct bio_post_read_ctx {
           struct bio *bio;
           struct work_struct work;
           unsigned int cur_step;
           unsigned int enabled_steps;
    };

``enabled_steps`` is a bitmask of the post-read steps that are
enabled.  The available steps are STEP_DECRYPT and STEP_VERITY.  These
steps can be enabled together, independently, or not at all.  If both
are enabled, then decryption is done first.  Since bio completion
callbacks cannot sleep, each post-read step is done by enqueueing the
struct on a workqueue, and then actual work happens in the work item.
Different workqueues are needed for encryption and verity because
verity work may require decrypting metadata pages from the file.

The bio completion callback sets PG_error for each page if either
decryption or verification failed.  Finally, after the work item(s)
complete, pages without PG_error are set Uptodate, and all pages are
unlocked.

A data page being set Uptodate and unlocked implies that it has been
verified, and such pages become visible to userspace via read(),
mmap(), etc.  Otherwise, the page is left in the PG_error && !Uptodate
state which results in the read() family of syscalls failing with EIO,
and accesses to the data via a memory map raising SIGBUS.  Note that
even if some pages in a file fail verification, pages that pass
verification can still be read.

To verify a data page, fs-verity reads the required hash page(s)
starting at the leaves and ascending to the root; then, the pages are
verified descending from the root.  Filesystems that store the verity
metadata past EOF implement reading hash pages using their usual
``->readpage{,s}()`` methods, with modifications:

- Verification is skipped for pages beyond ``i_size``.
- When checking whether a page is in the implicit hole beyond EOF,
  the full file size (including the verity metadata) is used rather
  than the original data i_size.  Note that this does not allow
  userspace to read or mmap the verity metadata.

The hash pages are also cached in the inode's address_space, similar
to data pages.  However, to simplify the verification logic, a hash
page being Uptodate doesn't imply that it has been verified; instead,
the PG_checked bit is used for this purpose.  Hash pages aren't locked
while being verified, so multiple threads may race to set PG_checked,
but this doesn't matter.

Thus, when ascending the tree reading hash pages, fs-verity can stop
as soon as it finds an already-checked hash page.  This optimization,
which is also used by dm-verity, results in excellent sequential read
performance since usually the deepest needed hash page will already be
cached and checked.  However, random reads perform worse.

Files may contain holes.  Normally, the filesystem's
``->readpage{,s}()`` methods will zero pages in holes and set them
Uptodate without issuing any bios.  To prevent this from being abused
to bypass fs-verity, filesystems call ``fsverity_verify_page()`` on
hole pages.

Like fscrypt, filesystems also disable direct I/O on verity files,
since direct I/O bypasses the normal read paths.

Userspace utility
=================

This document focuses on the kernel, but a userspace utility for
fs-verity can be found at:

	https://git.kernel.org/pub/scm/linux/kernel/git/ebiggers/fsverity-utils.git

See the README.md file in the fsverity-utils source tree for details,
including examples of setting up fs-verity protected files.

Tests
=====

To test fs-verity, use xfstests.  For example, using `kvm-xfstests
<https://git.kernel.org/pub/scm/fs/ext2/xfstests-bld.git/tree/Documentation/kvm-quickstart.md>`_::

    kvm-xfstests -c ext4,f2fs -g verity
