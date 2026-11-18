// SPDX-License-Identifier: GPL-3.0-or-later
//! The error a command returns when the page has to tell failures apart: a code to match on, and the sentence the player reads.
//! Serialised as { code, message }; the page shows `message` like any other error and branches on `code` (never on the words).
//! Only the commands whose callers branch use it; the rest keep returning a plain String.

use serde::Serialize;

/// The player stopped an update or an install of a build: nothing to report.
pub const UPDATE_CANCELLED: &str = "update_cancelled";
/// The player stopped the unpack of a game package.
pub const UNPACK_CANCELLED: &str = "unpack_cancelled";
/// No co-op invite is kept under the short code typed (coop_code::NOT_FOUND).
pub const INVITE_NOT_FOUND: &str = "invite_not_found";

#[derive(Debug, Serialize)]
pub struct CmdError {
    pub code: Option<&'static str>,
    pub message: String,
}

impl CmdError {
    /// `message` with `code` when it is the sentence `cancelled` (its job's cancel, or another the page branches on), plain otherwise.
    pub fn coded_if(message: String, cancelled: &str, code: &'static str) -> Self {
        CmdError { code: (message == cancelled).then_some(code), message }
    }
}

impl From<String> for CmdError {
    fn from(message: String) -> Self {
        CmdError { code: None, message }
    }
}

impl From<&str> for CmdError {
    fn from(message: &str) -> Self {
        message.to_string().into()
    }
}
