// SPDX-License-Identifier: GPL-3.0-or-later
//! Physical memory available to launcher jobs; swap is never part of the budget.

#[derive(Clone, Copy, Debug)]
pub struct Memory {
    pub total: u64,
    pub available: u64,
}

#[cfg(target_os = "linux")]
pub fn probe() -> Option<Memory> {
    let text = std::fs::read_to_string("/proc/meminfo").ok()?;
    let mut memory = meminfo(&text)?;
    // The kernel's MemAvailable describes the host, even inside a limited systemd service/container. Every ancestor can
    // impose a tighter limit. Account for current usage, including other processes, less inactive file cache; no swap.
    let membership = std::fs::read_to_string("/proc/self/cgroup").ok()?;
    let relative = membership.lines().find_map(|s| s.strip_prefix("0::"))?;
    cgroup_memory(&mut memory, std::path::Path::new("/sys/fs/cgroup"), relative)?;
    Some(memory)
}

#[cfg(target_os = "linux")]
fn meminfo(text: &str) -> Option<Memory> {
    let field = |key: &str| text.lines().find_map(|l| l.strip_prefix(key)?.split_whitespace().next()?.parse::<u64>().ok()?.checked_mul(1024));
    let total = field("MemTotal:")?;
    Some(Memory { total, available: field("MemAvailable:")?.min(total) })
}

#[cfg(target_os = "linux")]
fn cgroup_memory(memory: &mut Memory, root: &std::path::Path, membership: &str) -> Option<()> {
    use std::path::Component;
    let relative = std::path::Path::new(membership.trim_start_matches('/'));
    // A cgroup namespace may hide its parents. Do not resolve '..' outside the visible hierarchy.
    if relative.components().any(|c| !matches!(c, Component::Normal(_))) {
        return None;
    }
    let mut dir = root.join(relative);
    if !dir.is_dir() { return None; }
    loop {
        for file in ["memory.max", "memory.high"] {
            let value = match std::fs::read_to_string(dir.join(file)) {
                Ok(value) => value,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
                Err(_) => return None,
            };
            if value.trim() != "max" {
                let limit = value.trim().parse::<u64>().ok()?;
                let used = std::fs::read_to_string(dir.join("memory.current")).ok()?.trim().parse::<u64>().ok()?;
                // Written shader files remain in page cache after the translator exits. Inactive file pages are
                // reclaimable; counting them as anonymous memory can incorrectly refuse the next translation/retry.
                let reclaimable = std::fs::read_to_string(dir.join("memory.stat")).ok().and_then(|s| {
                    s.lines().find_map(|l| l.strip_prefix("inactive_file ")?.trim().parse::<u64>().ok())
                }).unwrap_or(0);
                memory.total = memory.total.min(limit);
                memory.available = memory.available.min(limit.saturating_sub(used.saturating_sub(reclaimable)));
            }
        }
        if dir == root {
            break;
        }
        if !dir.pop() || !dir.starts_with(root) {
            return None;
        }
    }
    Some(())
}

#[cfg(target_os = "macos")]
pub fn probe() -> Option<Memory> {
    unsafe {
        let mut total: u64 = 0;
        let mut len = std::mem::size_of::<u64>();
        if libc::sysctlbyname(c"hw.memsize".as_ptr(), &mut total as *mut _ as *mut _, &mut len, std::ptr::null_mut(), 0) != 0 || total == 0 {
            return None;
        }
        let mut page_size: u64 = 0;
        let mut len = std::mem::size_of::<u64>();
        if libc::sysctlbyname(c"hw.pagesize".as_ptr(), &mut page_size as *mut _ as *mut _, &mut len, std::ptr::null_mut(), 0) != 0 {
            page_size = 16384;
        }
        let mut stats: libc::vm_statistics64 = std::mem::zeroed();
        let mut count = libc::HOST_VM_INFO64_COUNT;
        if libc::host_statistics64(libc::mach_host_self(), libc::HOST_VM_INFO64, &mut stats as *mut _ as *mut _, &mut count) != libc::KERN_SUCCESS {
            return None;
        }
        let pages = stats.free_count as u64 + stats.inactive_count as u64 + stats.speculative_count as u64;
        Some(Memory { total, available: pages.saturating_mul(page_size).min(total) })
    }
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
pub fn probe() -> Option<Memory> {
    None
}

#[cfg(all(test, target_os = "linux"))]
mod tests {
    use super::*;

    #[test]
    fn available_memory_is_not_free_memory_or_swap() {
        let m = meminfo("MemTotal: 8388608 kB\nMemFree: 1 kB\nMemAvailable: 2097152 kB\nSwapFree: 99999999 kB\n").unwrap();
        assert_eq!((m.total, m.available), (8 << 30, 2 << 30));
        assert!(meminfo("MemTotal: 1024 kB\n").is_none());
    }

    #[test]
    fn tighter_ancestor_and_high_limit_include_current_usage() {
        let root = std::env::temp_dir().join(format!("shader_memory_{}", std::process::id()));
        std::fs::create_dir_all(root.join("parent/child")).unwrap();
        std::fs::write(root.join("parent/memory.max"), "2147483648\n").unwrap();
        std::fs::write(root.join("parent/memory.high"), "1610612736\n").unwrap();
        std::fs::write(root.join("parent/memory.current"), "536870912\n").unwrap();
        std::fs::write(root.join("parent/child/memory.max"), "max\n").unwrap();
        let mut m = Memory { total: 64 << 30, available: 40 << 30 };
        cgroup_memory(&mut m, &root, "/parent/child").unwrap();
        assert_eq!((m.total, m.available), (1536 << 20, 1 << 30));
        std::fs::write(root.join("parent/memory.stat"), "inactive_file 268435456\n").unwrap();
        m.available = 40 << 30;
        cgroup_memory(&mut m, &root, "/parent/child").unwrap();
        assert_eq!(m.available, 1280 << 20);
        std::fs::write(root.join("parent/memory.current"), "2147483648\n").unwrap();
        cgroup_memory(&mut m, &root, "/parent/child").unwrap();
        assert_eq!(m.available, 0);
        assert!(cgroup_memory(&mut m, &root, "/../outside").is_none());
        std::fs::remove_dir_all(root).unwrap();
    }
}
