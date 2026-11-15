//! A child process's output (the game build, the shader tools, the DLSS download) read line by line.
//!
//! `BufRead::lines()` ends at the first line that is not UTF-8, and `map_while(Result::ok)` then drops the rest of the stream.
//! A Windows tool writes in the ANSI code page, so a line naming a folder such as "OneDrive\Área de Trabalho" ended the
//! reader: the game build's `@error` line never arrived and Set up failed with no reason in setup.log (#630). Here such a line
//! is read lossily and the stream goes on.

use std::io::{BufRead, BufReader, Read};

/// The lines `r` gives until it ends or fails, without their line ending ("\n" or "\r\n"). Bytes that are not UTF-8 read as
/// U+FFFD.
pub fn lines<R: Read>(r: R) -> impl Iterator<Item = String> {
    let mut r = BufReader::new(r);
    std::iter::from_fn(move || {
        let mut buf = Vec::new();
        match r.read_until(b'\n', &mut buf) {
            Ok(0) | Err(_) => None,
            Ok(_) => {
                if buf.last() == Some(&b'\n') {
                    buf.pop();
                    if buf.last() == Some(&b'\r') {
                        buf.pop();
                    }
                }
                Some(String::from_utf8_lossy(&buf).into_owned())
            }
        }
    })
}

#[cfg(test)]
mod tests {
    #[test]
    fn a_line_that_is_not_utf8_does_not_end_the_stream() {
        // "@error C:\...\Área\r\n" as cp1252 writes it (0xC1), then a line after it
        let out: &[u8] = b"@progress lift 0 4 x\r\n@error C:\\\xC1rea\r\nlast";
        let got: Vec<String> = super::lines(out).collect();
        assert_eq!(got, ["@progress lift 0 4 x", "@error C:\\\u{FFFD}rea", "last"]);
    }
}
