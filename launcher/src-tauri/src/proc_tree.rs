//! Ties the processes Set up starts (the game build, the shader tools) and the processes they start in turn to the
//! launcher's life, so they end when the launcher closes or crashes. A build left running kept writing into build/ under
//! the next Set up: two lifts of main share build/lift/main/.lean-flows, each clears it, and the newer one failed with
//! "No such file or directory: ...\.lean-flows\<n>.pkl".
//!
//! Windows: one job object for the launcher, KILL_ON_JOB_CLOSE. Its handle is never closed by us; the system closes it when
//! the launcher exits, however it exits, and that ends every process in the job. Processes a child starts join its job.
//! Linux: PR_SET_PDEATHSIG sends the child SIGTERM when the launcher thread that started it goes away (each caller waits for
//! its child on that thread); pkp_build.py answers SIGTERM by ending its whole process group. macOS has no such signal.
//!
//! Never used for the game itself: a player may close the launcher while playing.

use std::process::{Child, Command};

/// `cmd.spawn()`, with the child (and what it starts) ending when the launcher does.
pub fn spawn(cmd: &mut Command) -> std::io::Result<Child> {
    #[cfg(target_os = "linux")]
    end_with_launcher(cmd);
    let child = cmd.spawn()?;
    #[cfg(windows)]
    job::adopt(&child);
    Ok(child)
}

#[cfg(target_os = "linux")]
fn end_with_launcher(cmd: &mut Command) {
    use std::os::unix::process::CommandExt;
    let launcher = std::process::id() as libc::pid_t;
    // SAFETY: prctl, getppid and raise are async-signal-safe, which is all pre_exec asks of the closure
    unsafe {
        cmd.pre_exec(move || {
            if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM as libc::c_ulong) != 0 {
                return Err(std::io::Error::last_os_error());
            }
            // the launcher went away between fork and prctl: the signal will never come
            if libc::getppid() != launcher {
                libc::raise(libc::SIGTERM);
            }
            Ok(())
        });
    }
}

#[cfg(windows)]
mod job {
    use std::os::windows::io::AsRawHandle;
    use std::process::Child;
    use std::sync::OnceLock;
    use windows::Win32::Foundation::{CloseHandle, HANDLE};
    use windows::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    };

    struct Job(HANDLE);
    // SAFETY: a job object handle may be used from any thread
    unsafe impl Send for Job {}
    unsafe impl Sync for Job {}

    fn job() -> Option<&'static Job> {
        static JOB: OnceLock<Option<Job>> = OnceLock::new();
        JOB.get_or_init(|| unsafe {
            let h = CreateJobObjectW(None, windows::core::PCWSTR::null()).ok()?;
            let mut info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            let size = std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32;
            if SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info as *const _ as *const _, size).is_err() {
                let _ = CloseHandle(h);
                return None;
            }
            Some(Job(h))
        })
        .as_ref()
    }

    /// Puts the child in the launcher's job. The child is assigned just after it starts; the scripts take far longer than
    /// that to start their own processes, which then join the job. Without a job (the call failed) the child runs as before.
    pub fn adopt(child: &Child) {
        if let Some(j) = job() {
            // SAFETY: both handles are open for the duration of the call
            let _ = unsafe { AssignProcessToJobObject(j.0, HANDLE(child.as_raw_handle())) };
        }
    }
}
