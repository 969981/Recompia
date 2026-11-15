// SPDX-License-Identifier: GPL-3.0-or-later
//! The icon inside a Windows exe: what Explorer, shortcuts and the taskbar show, and what SDL gives the game's window (it takes the
//! exe's first icon group when the game sets none). `stamp` replaces every icon group of an exe with one .ico; `swap_running` does that
//! to the launcher's own exe, which Windows lets us rename but not write while it runs.
//!
//! An .ico file is ICONDIR + one 16-byte ICONDIRENTRY per image + the images. In an exe each image is an RT_ICON resource and the group
//! is an RT_GROUP_ICON resource: the same entries, 14 bytes each, with the RT_ICON id in place of the file offset.
// Only Windows exes carry these; elsewhere only the tests use the .ico reader.
#![cfg_attr(not(windows), allow(dead_code))]

/// One image of an .ico: its directory fields and its bytes (a PNG or a DIB, stored unchanged in the exe).
#[derive(Debug, PartialEq)]
pub struct IcoImage<'a> {
    pub width: u8,
    pub height: u8,
    pub colors: u8,
    pub planes: u16,
    pub bit_count: u16,
    pub data: &'a [u8],
}

fn u16_at(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(at..at + 2)?.try_into().ok()?))
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

/// The images of an .ico file, in file order. None: not an icon file, or an entry points outside it.
pub fn parse_ico(b: &[u8]) -> Option<Vec<IcoImage<'_>>> {
    if u16_at(b, 0)? != 0 || u16_at(b, 2)? != 1 {
        return None;
    }
    let count = u16_at(b, 4)? as usize;
    if count == 0 {
        return None;
    }
    (0..count)
        .map(|i| {
            let e = 6 + 16 * i;
            let (size, offset) = (u32_at(b, e + 8)? as usize, u32_at(b, e + 12)? as usize);
            Some(IcoImage {
                width: *b.get(e)?,
                height: *b.get(e + 1)?,
                colors: *b.get(e + 2)?,
                planes: u16_at(b, e + 4)?,
                bit_count: u16_at(b, e + 6)?,
                data: b.get(offset..offset.checked_add(size)?)?,
            })
        })
        .collect()
}

/// The RT_GROUP_ICON resource for `images` stored as RT_ICON ids 1..=n.
pub fn group_resource(images: &[IcoImage]) -> Vec<u8> {
    let mut g = Vec::with_capacity(6 + 14 * images.len());
    g.extend_from_slice(&0u16.to_le_bytes());
    g.extend_from_slice(&1u16.to_le_bytes());
    g.extend_from_slice(&(images.len() as u16).to_le_bytes());
    for (i, im) in images.iter().enumerate() {
        g.extend_from_slice(&[im.width, im.height, im.colors, 0]);
        g.extend_from_slice(&im.planes.to_le_bytes());
        g.extend_from_slice(&im.bit_count.to_le_bytes());
        g.extend_from_slice(&(im.data.len() as u32).to_le_bytes());
        g.extend_from_slice(&(i as u16 + 1).to_le_bytes());
    }
    g
}

/// The RT_ICON ids an RT_GROUP_ICON resource lists, in its order.
pub fn group_ids(g: &[u8]) -> Option<Vec<u16>> {
    let count = u16_at(g, 4)? as usize;
    (0..count).map(|i| u16_at(g, 6 + 14 * i + 12)).collect()
}

#[cfg(windows)]
pub use win::{has_icon, notify_shell, stamp, swap_running};

#[cfg(windows)]
mod win {
    use super::{group_ids, group_resource, parse_ico};
    use std::fs;
    use std::os::windows::ffi::OsStrExt;
    use std::path::{Path, PathBuf};
    use windows::core::{BOOL, PCWSTR};
    use windows::Win32::Foundation::{FreeLibrary, HMODULE};
    use windows::Win32::System::LibraryLoader::{
        BeginUpdateResourceW, EndUpdateResourceW, EnumResourceLanguagesW, EnumResourceNamesW, FindResourceW, LoadLibraryExW, LoadResource,
        LockResource, SizeofResource, UpdateResourceW, LOAD_LIBRARY_AS_DATAFILE, LOAD_LIBRARY_AS_IMAGE_RESOURCE,
    };
    use windows::Win32::UI::Shell::{SHChangeNotify, SHCNE_UPDATEITEM, SHCNF_PATHW};

    const RT_ICON: PCWSTR = PCWSTR(3 as _);
    const RT_GROUP_ICON: PCWSTR = PCWSTR(14 as _);
    const LANG_NEUTRAL: u16 = 0;

    fn wide(p: &Path) -> Vec<u16> {
        p.as_os_str().encode_wide().chain(Some(0)).collect()
    }

    /// A resource name as the update API takes it back: a number (MAKEINTRESOURCE) or a NUL-terminated string.
    #[derive(Clone)]
    enum Name {
        Id(u16),
        Text(Vec<u16>),
    }

    impl Name {
        unsafe fn from(p: PCWSTR) -> Name {
            if (p.0 as usize) >> 16 == 0 {
                Name::Id(p.0 as usize as u16)
            } else {
                Name::Text(p.as_wide().iter().copied().chain(Some(0)).collect())
            }
        }
        fn pcwstr(&self) -> PCWSTR {
            match self {
                Name::Id(id) => PCWSTR(*id as usize as _),
                Name::Text(w) => PCWSTR(w.as_ptr()),
            }
        }
    }

    /// The exe mapped as data, freed on drop: it must be gone before the file can be updated or renamed.
    struct Module(HMODULE);

    impl Module {
        fn open(exe: &Path) -> Option<Module> {
            let w = wide(exe);
            unsafe { LoadLibraryExW(PCWSTR(w.as_ptr()), None, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE) }.ok().map(Module)
        }
        fn names(&self, kind: PCWSTR) -> Vec<Name> {
            unsafe extern "system" fn each(_: HMODULE, _: PCWSTR, name: PCWSTR, out: isize) -> BOOL {
                unsafe { (*(out as *mut Vec<Name>)).push(Name::from(name)) };
                true.into()
            }
            let mut out: Vec<Name> = Vec::new();
            let _ = unsafe { EnumResourceNamesW(Some(self.0), kind, Some(each), &mut out as *mut _ as isize) };
            out
        }
        fn languages(&self, kind: PCWSTR, name: &Name) -> Vec<u16> {
            unsafe extern "system" fn each(_: HMODULE, _: PCWSTR, _: PCWSTR, lang: u16, out: isize) -> BOOL {
                unsafe { (*(out as *mut Vec<u16>)).push(lang) };
                true.into()
            }
            let mut out: Vec<u16> = Vec::new();
            let _ = unsafe { EnumResourceLanguagesW(Some(self.0), kind, name.pcwstr(), Some(each), &mut out as *mut _ as isize) };
            out
        }
        fn bytes(&self, kind: PCWSTR, name: &Name) -> Option<Vec<u8>> {
            unsafe {
                let r = FindResourceW(Some(self.0), name.pcwstr(), kind);
                if r.is_invalid() {
                    return None;
                }
                let size = SizeofResource(Some(self.0), r) as usize;
                let p = LockResource(LoadResource(Some(self.0), r).ok()?) as *const u8;
                (!p.is_null()).then(|| std::slice::from_raw_parts(p, size).to_vec())
            }
        }
    }

    impl Drop for Module {
        fn drop(&mut self) {
            let _ = unsafe { FreeLibrary(self.0) };
        }
    }

    /// The images of the exe's first icon group (the one Explorer and SDL use), in the group's order.
    fn first_group_images(exe: &Path) -> Option<Vec<Vec<u8>>> {
        let m = Module::open(exe)?;
        let first = m.names(RT_GROUP_ICON).into_iter().next()?;
        let ids = group_ids(&m.bytes(RT_GROUP_ICON, &first)?)?;
        ids.into_iter().map(|id| m.bytes(RT_ICON, &Name::Id(id))).collect()
    }

    /// The exe already shows this .ico: its first icon group holds exactly the .ico's images, in order.
    pub fn has_icon(exe: &Path, ico: &[u8]) -> bool {
        let (Some(have), Some(want)) = (first_group_images(exe), parse_ico(ico)) else { return false };
        have.len() == want.len() && have.iter().zip(&want).all(|(h, w)| h.as_slice() == w.data)
    }

    /// Every (kind, name, language) of the exe's icons and icon groups: what `stamp` deletes before it adds its own.
    fn icon_resources(exe: &Path) -> Vec<(PCWSTR, Name, u16)> {
        let Some(m) = Module::open(exe) else { return Vec::new() };
        let mut out = Vec::new();
        for kind in [RT_GROUP_ICON, RT_ICON] {
            for name in m.names(kind) {
                for lang in m.languages(kind, &name) {
                    out.push((kind, name.clone(), lang));
                }
            }
        }
        out
    }

    /// Replaces every icon of `exe` with the images of `ico` (one group, id 1). The file must not be running or open elsewhere.
    pub fn stamp(exe: &Path, ico: &[u8]) -> Result<(), String> {
        let images = parse_ico(ico).ok_or("the icon file is not a valid .ico")?;
        let old = icon_resources(exe);
        let w = wide(exe);
        let fail = |e: windows::core::Error| format!("could not change the icon of {}: {}", exe.display(), e.message());
        unsafe {
            let h = BeginUpdateResourceW(PCWSTR(w.as_ptr()), false).map_err(fail)?;
            let run = || -> windows::core::Result<()> {
                for (kind, name, lang) in &old {
                    UpdateResourceW(h, *kind, name.pcwstr(), *lang, None, 0)?;
                }
                for (i, im) in images.iter().enumerate() {
                    UpdateResourceW(h, RT_ICON, PCWSTR((i + 1) as _), LANG_NEUTRAL, Some(im.data.as_ptr().cast()), im.data.len() as u32)?;
                }
                let group = group_resource(&images);
                UpdateResourceW(h, RT_GROUP_ICON, PCWSTR(1 as _), LANG_NEUTRAL, Some(group.as_ptr().cast()), group.len() as u32)
            };
            match run() {
                Ok(()) => EndUpdateResourceW(h, false).map_err(fail),
                Err(e) => {
                    let _ = EndUpdateResourceW(h, true);
                    Err(fail(e))
                }
            }
        }
    }

    /// Stamps the running exe: a stamped copy takes its name, and the running file becomes `old` (deleted at the next start by
    /// update::clean_start, as after an update). Nothing changes when any step fails.
    pub fn swap_running(exe: &Path, old: &Path, ico: &[u8]) -> Result<(), String> {
        let mut next = exe.as_os_str().to_owned();
        next.push(".icon");
        let next = PathBuf::from(next);
        let result = (|| {
            fs::copy(exe, &next).map_err(|e| format!("could not copy {}: {e}", exe.display()))?;
            stamp(&next, ico)?;
            if old.exists() {
                fs::remove_file(old).map_err(|e| format!("{} is still in use: {e}", old.display()))?;
            }
            fs::rename(exe, old).map_err(|e| format!("could not move {} aside: {e}", exe.display()))?;
            if let Err(e) = fs::rename(&next, exe) {
                let _ = fs::rename(old, exe);
                return Err(format!("could not put the new {} in place: {e}", exe.display()));
            }
            Ok(())
        })();
        let _ = fs::remove_file(&next);
        result
    }

    /// Tells Explorer the file changed, so its icon (and the icon of every shortcut to it) is drawn again.
    pub fn notify_shell(path: &Path) {
        let w = wide(path);
        unsafe { SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, Some(w.as_ptr().cast()), None) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ico(images: &[(u8, &[u8])]) -> Vec<u8> {
        let mut b = vec![0, 0, 1, 0];
        b.extend_from_slice(&(images.len() as u16).to_le_bytes());
        let mut offset = 6 + 16 * images.len();
        for (w, data) in images {
            b.extend_from_slice(&[*w, *w, 0, 0, 1, 0, 32, 0]);
            b.extend_from_slice(&(data.len() as u32).to_le_bytes());
            b.extend_from_slice(&(offset as u32).to_le_bytes());
            offset += data.len();
        }
        for (_, data) in images {
            b.extend_from_slice(data);
        }
        b
    }

    #[test]
    fn an_ico_becomes_one_group_with_ids_from_one() {
        let file = ico(&[(16, b"small"), (0, b"large png")]);
        let images = parse_ico(&file).unwrap();
        assert_eq!(images.len(), 2);
        assert_eq!(images[1].data, b"large png");
        assert_eq!(images[1].width, 0, "256 px is stored as 0");
        let group = group_resource(&images);
        assert_eq!(group.len(), 6 + 14 * 2);
        assert_eq!(group_ids(&group).unwrap(), vec![1, 2]);
        assert_eq!(u32::from_le_bytes(group[6 + 14 + 8..6 + 14 + 12].try_into().unwrap()), 9, "the group names each image's size");
    }

    #[test]
    fn a_broken_ico_is_refused() {
        assert!(parse_ico(b"\x89PNG").is_none());
        let mut file = ico(&[(16, b"small")]);
        file.truncate(file.len() - 1);
        assert!(parse_ico(&file).is_none(), "an image that runs past the end");
    }

    #[test]
    fn the_shipped_icons_parse() {
        for b in [crate::branding::ico(crate::branding::Look::Recompia, false), crate::branding::ico(crate::branding::Look::Classic, true)] {
            assert!(parse_ico(b).is_some_and(|v| !v.is_empty()));
        }
    }
}
