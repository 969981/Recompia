// SPDX-License-Identifier: GPL-3.0-or-later
//! The OS random source: the relay's cookies and UDP tokens, and the launcher's install ids.

#[cfg(windows)]
pub fn os_random(buf: &mut [u8]) -> std::io::Result<()> {
    const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 2;
    #[link(name = "bcrypt")]
    extern "system" {
        fn BCryptGenRandom(alg: *mut std::ffi::c_void, buf: *mut u8, len: u32, flags: u32) -> i32;
    }
    let st = unsafe { BCryptGenRandom(std::ptr::null_mut(), buf.as_mut_ptr(), buf.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) };
    if st < 0 {
        return Err(std::io::Error::other(format!("BCryptGenRandom failed: 0x{:08x}", st as u32)));
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn os_random(buf: &mut [u8]) -> std::io::Result<()> {
    use std::io::Read;
    std::fs::File::open("/dev/urandom")?.read_exact(buf)
}
