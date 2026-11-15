// SPDX-License-Identifier: GPL-3.0-or-later
//! A bounded native translator configuration, selected afresh for each process.

use super::Mode;
use crate::host::memory::Memory;

const MIB: u64 = 1 << 20;
const GIB: u64 = 1 << 30;

#[derive(Clone, Copy, Debug)]
pub struct Settings {
    pub jobs: usize,
    pub mode: Mode,
    pub heap: u64,
    pub available: Option<u64>,
}

impl Settings {
    pub fn detect(gentle: bool) -> Result<Self, String> {
        let cpus = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
        let memory = crate::host::memory::probe();
        #[cfg(target_os = "macos")]
        if crate::host::memory_pressure_level().is_some_and(|level| level >= 4) {
            return Err("Memory pressure is critical. Close other programs and try the shader translation again.".into());
        }
        Self::choose(cpus, memory, gentle)
    }

    fn choose(cpus: usize, memory: Option<Memory>, gentle: bool) -> Result<Self, String> {
        let available = memory.map(|m| m.available.min(m.total));
        // Keep at least half the currently available memory, and at least 1 GiB, outside the translator. The process
        // budget includes 256 MiB beyond the GC heap (JIT, stacks, native allocations); the GC limit is not an RSS limit.
        // A 2 GiB process budget caps the heap at 1.5 GiB. Each worker gets 256 MiB of that heap: extra workers increased
        // peak RSS without improving throughput in the native 2/6/12/16-worker measurements.
        let budget = available.map(|a| (a / 2).min(a.saturating_sub(GIB)).min(2 * GIB)).unwrap_or(GIB);
        if budget < 512 * MIB {
            return Err("Not enough available memory to translate shaders. Close other programs and try again (at least 1.5 GiB available is needed).".into());
        }
        let heap = (budget - 256 * MIB).min(1536 * MIB);
        let mode = if available.is_some() && !gentle && cpus > 1 && heap >= 512 * MIB { Mode::Fast } else { Mode::Safe };
        let jobs = cpus.saturating_sub(1).max(1).min((heap / (256 * MIB)) as usize);
        Ok(Self { jobs: if mode == Mode::Safe { jobs.min(2) } else { jobs }, mode, heap, available })
    }

    pub fn safer(self) -> Self {
        Self { jobs: 1, mode: Mode::Safe, heap: self.heap.min(512 * MIB), ..self }
    }

    pub fn describe(self) -> String {
        format!("Shader translator: {} jobs, {} GC, {} MiB heap limit; available memory {}.",
            self.jobs, if self.mode == Mode::Fast { "server" } else { "workstation" }, self.heap / MIB,
            self.available.map(|n| format!("{} MiB", n / MIB)).unwrap_or_else(|| "unknown (conservative settings)".into()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn memory(available: u64) -> Option<Memory> { Some(Memory { total: 64 * GIB, available }) }

    #[test]
    fn scales_with_available_memory_and_cpu_not_installed_ram() {
        let fast = Settings::choose(16, memory(40 * GIB), false).unwrap();
        assert_eq!((fast.jobs, fast.heap, fast.mode), (6, 1536 * MIB, Mode::Fast));
        let smaller = Settings::choose(16, memory(2 * GIB), false).unwrap();
        assert_eq!((smaller.jobs, smaller.heap), (3, 768 * MIB));
        assert_eq!(Settings::choose(2, memory(40 * GIB), false).unwrap().jobs, 1);
        assert_eq!(Settings::choose(1, memory(40 * GIB), false).unwrap().mode, Mode::Safe);
    }

    #[test]
    fn unknown_gentle_and_pressure_use_conservative_settings() {
        assert_eq!(Settings::choose(64, None, false).unwrap().mode, Mode::Safe);
        assert!(Settings::choose(16, memory(GIB), false).is_err());
        let tight = Settings::choose(16, memory(1536 * MIB), false).unwrap();
        assert_eq!((tight.jobs, tight.heap, tight.mode), (1, 256 * MIB, Mode::Safe));
        let gentle = Settings::choose(16, memory(40 * GIB), true).unwrap();
        assert_eq!((gentle.jobs, gentle.mode), (2, Mode::Safe));
        let safer = tight.safer();
        assert!(safer.heap <= tight.heap && safer.jobs <= tight.jobs);
    }
}
