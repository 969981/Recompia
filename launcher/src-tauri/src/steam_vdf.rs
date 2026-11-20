// SPDX-License-Identifier: GPL-3.0-or-later
//! Steam's binary key-value format, the one shortcuts.vdf is written in (steam.rs edits our entries in it).
//!
//! A map is a run of entries ended by 0x08. An entry is a type byte (0 map, 1 string, 2 u32 little endian), the key and a 0 byte, then the
//! value (a map's value is the map itself; a string ends with a 0 byte). The file is the root map, which holds one entry: "shortcuts".
#[derive(Clone, Debug, PartialEq)]
pub enum Val {
    Map(Vec<(String, Val)>),
    Str(String),
    Int(u32),
}

fn cstr(b: &[u8], p: &mut usize) -> Result<String, String> {
    let start = *p;
    while *p < b.len() && b[*p] != 0 {
        *p += 1;
    }
    if *p >= b.len() {
        return Err("a text in the file is not finished".into());
    }
    let s = String::from_utf8_lossy(&b[start..*p]).into_owned();
    *p += 1;
    Ok(s)
}

fn parse_map(b: &[u8], p: &mut usize, depth: u32) -> Result<Vec<(String, Val)>, String> {
    if depth > 8 {
        return Err("the file nests too deep".into());
    }
    let mut out = Vec::new();
    loop {
        let t = *b.get(*p).ok_or("the file ends inside a list")?;
        *p += 1;
        if t == 8 {
            return Ok(out);
        }
        let key = cstr(b, p)?;
        let v = match t {
            0 => Val::Map(parse_map(b, p, depth + 1)?),
            1 => Val::Str(cstr(b, p)?),
            2 => {
                let w = b.get(*p..*p + 4).ok_or("the file ends inside a number")?;
                *p += 4;
                Val::Int(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
            }
            other => return Err(format!("unknown entry type {other} in the file")),
        };
        out.push((key, v));
    }
}

/// The root map of a shortcuts.vdf. An empty or missing file is an empty root.
pub fn parse(b: &[u8]) -> Result<Vec<(String, Val)>, String> {
    if b.is_empty() {
        return Ok(Vec::new());
    }
    let mut p = 0;
    let root = parse_map(b, &mut p, 0)?;
    Ok(root)
}

pub(crate) fn write_map(out: &mut Vec<u8>, m: &[(String, Val)]) {
    for (k, v) in m {
        match v {
            Val::Map(inner) => {
                out.push(0);
                out.extend_from_slice(k.as_bytes());
                out.push(0);
                write_map(out, inner);
            }
            Val::Str(s) => {
                out.push(1);
                out.extend_from_slice(k.as_bytes());
                out.push(0);
                out.extend_from_slice(s.as_bytes());
                out.push(0);
            }
            Val::Int(n) => {
                out.push(2);
                out.extend_from_slice(k.as_bytes());
                out.push(0);
                out.extend_from_slice(&n.to_le_bytes());
            }
        }
    }
    out.push(8);
}

pub fn write(root: &[(String, Val)]) -> Vec<u8> {
    let mut out = Vec::new();
    write_map(&mut out, root);
    out
}

pub fn key_is(k: &str, name: &str) -> bool {
    k.eq_ignore_ascii_case(name)
}

pub fn get_str<'a>(entry: &'a [(String, Val)], name: &str) -> Option<&'a str> {
    entry.iter().find_map(|(k, v)| match v {
        Val::Str(s) if key_is(k, name) => Some(s.as_str()),
        _ => None,
    })
}

pub fn get_int(entry: &[(String, Val)], name: &str) -> Option<u32> {
    entry.iter().find_map(|(k, v)| match v {
        Val::Int(n) if key_is(k, name) => Some(*n),
        _ => None,
    })
}

/// Sets a string field, under the key's spelling the entry already uses (Steam writes some in lower case), or adds it.
pub fn set_str(entry: &mut Vec<(String, Val)>, name: &str, value: &str) {
    match entry.iter_mut().find(|(k, v)| key_is(k, name) && matches!(v, Val::Str(_))) {
        Some((_, v)) => *v = Val::Str(value.to_string()),
        None => entry.push((name.to_string(), Val::Str(value.to_string()))),
    }
}

/// The list of shortcuts inside the root map; empty when the file has none.
pub fn shortcuts(root: &[(String, Val)]) -> &[(String, Val)] {
    root.iter()
        .find_map(|(k, v)| match v {
            Val::Map(list) if key_is(k, "shortcuts") => Some(list.as_slice()),
            _ => None,
        })
        .unwrap_or(&[])
}

/// The list of shortcuts inside the root map, created when the file has none.
pub fn shortcuts_mut(root: &mut Vec<(String, Val)>) -> &mut Vec<(String, Val)> {
    let i = match root.iter().position(|(k, v)| key_is(k, "shortcuts") && matches!(v, Val::Map(_))) {
        Some(i) => i,
        None => {
            root.push(("shortcuts".into(), Val::Map(Vec::new())));
            root.len() - 1
        }
    };
    match &mut root[i].1 {
        Val::Map(m) => m,
        _ => unreachable!(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_refuses_garbage_and_reads_empty() {
        assert!(parse(&[]).unwrap().is_empty());
        assert!(parse(&[9, b'x', 0]).is_err());
        assert!(parse(&[1, b'k', 0, b'v']).is_err());      // string not finished
        assert!(parse(&[2, b'k', 0, 1, 2]).is_err());      // number cut short
        assert!(parse(&[0, b'a', 0]).is_err());            // map not closed
    }

    #[test]
    fn set_str_keeps_the_spelling_steam_used() {
        let mut e = vec![("exe".to_string(), Val::Str("a".into())), ("appid".to_string(), Val::Int(7))];
        set_str(&mut e, "Exe", "b");
        set_str(&mut e, "StartDir", "c");
        assert_eq!(e[0], ("exe".to_string(), Val::Str("b".into())));
        assert_eq!(get_str(&e, "startdir"), Some("c"));
        assert_eq!(get_int(&e, "AppId"), Some(7));
        assert!(shortcuts(&[]).is_empty());
    }
}
