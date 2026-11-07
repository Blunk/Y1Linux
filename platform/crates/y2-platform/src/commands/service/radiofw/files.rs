//! The radio files (the Y2's seven, or an image's radio-firmware.json set): read from a stock
//! system.img or a directory, checked against the reviewed sizes and hashes (with the board defaults
//! derived from a stock library), installed on Y2DATA in one rename.
// SPDX-License-Identifier: GPL-2.0-only
use super::{
    fs, hex, os_path, py, sha256_hex, Error, Ext4, Files, Image, OpenOptions, Path, Result,
    Reviewed, Root, Slice,
};
use std::io::Write;
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::fs::OpenOptionsExt;

/// The generic Wi-Fi/BT board records of the stock reader library, with the
/// default addresses cleared.
pub(super) fn defaults(custom: &[u8]) -> Result<Files> {
    let slice = |a: usize, b: usize| custom.get(a..b.min(custom.len())).unwrap_or(&[]).to_vec();
    let mut wifi = slice(0x5a8e, 0x5c8e);
    let mut bt = slice(0x3a18, 0x3a58);
    if wifi.len() < 4 {
        return Err(Error::Value(
            "unpack_from requires a buffer of at least 4 bytes".into(),
        ));
    }
    let header = (
        u16::from_le_bytes([wifi[0], wifi[1]]),
        u16::from_le_bytes([wifi[2], wifi[3]]),
    );
    if !custom.starts_with(b"\x7fELF\x01\x01\x01")
        || header != (0x104, 0)
        || wifi.get(196..198) != Some(&b"\x01\x00"[..])
    {
        return Err(Error::Value("stock_reader_library_layout".into()));
    }
    for byte in wifi.iter_mut().take(10).skip(4) {
        *byte = 0;
    }
    for byte in bt.iter_mut().take(6) {
        *byte = 0;
    }
    Ok(vec![
        ("WIFI.defaults".into(), wifi),
        ("BT.defaults".into(), bt),
    ])
}

/// The configured default records (radio-firmware.json): each slice of its library (`libraries`:
/// path and bytes, the set's own first), which must start with its expected bytes, with its leading
/// bytes cleared.
pub(super) fn sliced(slices: &[Slice], libraries: &[(&str, &[u8])]) -> Result<Files> {
    let layout = || Error::Value("stock_reader_library_layout".into());
    let mut out = Vec::new();
    for slice in slices {
        let wanted = slice.library.as_deref();
        let library = libraries
            .iter()
            .find(|(name, _)| wanted.is_none_or(|w| w == *name))
            .map(|(_, data)| *data)
            .ok_or_else(layout)?;
        if !library.starts_with(b"\x7fELF") {
            return Err(layout());
        }
        let end = slice
            .offset
            .checked_add(slice.size as usize)
            .ok_or_else(layout)?;
        let mut data = library.get(slice.offset..end).ok_or_else(layout)?.to_vec();
        if !data.starts_with(&hex(&slice.expect).ok_or_else(layout)?) {
            return Err(layout());
        }
        data[..slice.clear].fill(0);
        out.push((slice.name.clone(), data));
    }
    Ok(out)
}

pub(super) fn get<'a>(files: &'a Files, name: &str) -> Option<&'a Vec<u8>> {
    files.iter().find(|(n, _)| n == name).map(|(_, d)| d)
}

/// Every reviewed file present, with the reviewed size and SHA-256.
pub(super) fn checked(reviewed: &Reviewed, files: Files) -> Result<Files> {
    for (name, size, sha) in reviewed.all() {
        match get(&files, name) {
            Some(data) if data.len() as u64 == *size && sha256_hex(data) == *sha => {}
            _ => return Err(Error::Value(format!("firmware_mismatch:{name}"))),
        }
    }
    // The two Bluetooth patches in their download order (provision.py).
    let mut order: Vec<u8> = Vec::new();
    for (name, _, _) in &reviewed.files {
        if name.ends_with("_hdr.bin") {
            let byte = get(&files, name)
                .and_then(|data| data.get(24))
                .ok_or_else(|| Error::Value("index out of range".into()))?;
            order.push(byte & 15);
        }
    }
    order.sort();
    if order != reviewed.patches {
        return Err(Error::Value("firmware_patch_order".into()));
    }
    Ok(files)
}

/// A stock library of the image with its reviewed size and SHA-256.
fn library(fs: &mut Ext4, (path, size, sha): &(String, u64, String)) -> Result<Vec<u8>> {
    let data = fs.read(path, *size)?;
    if data.len() as u64 != *size || sha256_hex(&data) != *sha {
        let name = path.rsplit('/').next().unwrap_or_default();
        return Err(Error::Value(format!("firmware_mismatch:{name}")));
    }
    Ok(data)
}

/// The radio files from the stock system.img (sparse or raw ext4).
pub(super) fn from_system_image(reviewed: &Reviewed, path: &Path) -> Result<Files> {
    let mut fs = Ext4::new(Image::open(path)?)?;
    let mut files: Files = Vec::new();
    for (name, size, _) in &reviewed.files {
        files.push((
            name.clone(),
            fs.read(&format!("/etc/firmware/{name}"), *size)?,
        ));
    }
    let custom = library(&mut fs, &reviewed.custom)?;
    files.extend(match &reviewed.slices {
        None => defaults(&custom)?,
        Some(slices) => {
            let mut libraries = vec![(reviewed.custom.0.clone(), custom)];
            for entry in &reviewed.libraries {
                libraries.push((entry.0.clone(), library(&mut fs, entry)?));
            }
            let named: Vec<(&str, &[u8])> = libraries
                .iter()
                .map(|(name, data)| (name.as_str(), data.as_slice()))
                .collect();
            sliced(slices, &named)?
        }
    });
    checked(reviewed, files)
}

/// The supplement from another package's system.img: each file of its /etc/firmware with the
/// reviewed size and SHA-256.
pub(super) fn from_supplement_image(reviewed: &Reviewed, path: &Path) -> Result<Files> {
    if reviewed.supplement.is_empty() {
        return Err(Error::Value("no_supplement".into()));
    }
    let mut fs = Ext4::new(Image::open(path)?)?;
    let mut files: Files = Vec::new();
    for (name, size, sha) in &reviewed.supplement {
        let data = fs.read(&format!("/etc/firmware/{name}"), *size)?;
        if data.len() as u64 != *size || sha256_hex(&data) != *sha {
            return Err(Error::Value(format!("firmware_mismatch:{name}")));
        }
        files.push((name.clone(), data));
    }
    Ok(files)
}

/// The radio files from a directory (a root's /lib/firmware/mediatek/mt6582).
pub(super) fn from_directory(reviewed: &Reviewed, path: &Path) -> Result<Files> {
    let mut files = Vec::new();
    for (name, size, _) in reviewed.all() {
        let file = path.join(name);
        let info = file.symlink_metadata().map_err(|e| os_path(e, &file))?;
        if !info.file_type().is_file() || info.len() != *size {
            return Err(Error::Value(format!("firmware_mismatch:{name}")));
        }
        files.push((
            name.clone(),
            fs::read(&file).map_err(|e| os_path(e, &file))?,
        ));
    }
    checked(reviewed, files)
}

/// Cheap check for status polling: every file there with its size.
///
/// The service and the USB views ask every pass: the reviewed tables as they are, nothing copied.
pub fn present(root: &Root) -> bool {
    let Ok(reviewed) = Reviewed::of(root) else {
        return false;
    };
    let target = root.path(&reviewed.target);
    let every = reviewed.all().all(|(name, size, _)| {
        target
            .join(name)
            .symlink_metadata()
            .is_ok_and(|m| m.len() == *size && m.file_type().is_file())
    });
    every
}

/// Every supplement file is in place with its size (true when there is none).
pub(super) fn supplemented(root: &Root, reviewed: &Reviewed) -> bool {
    let target = root.path(&reviewed.target);
    reviewed.supplement.iter().all(|(name, size, _)| {
        target
            .join(name)
            .symlink_metadata()
            .is_ok_and(|m| m.len() == *size && m.file_type().is_file())
    })
}

/// The installed supplement files that still have their reviewed hash (the set's install keeps them).
fn kept_supplement(target: &Path, reviewed: &Reviewed) -> Files {
    reviewed
        .supplement
        .iter()
        .filter_map(|(name, size, sha)| {
            let file = target.join(name);
            let info = file.symlink_metadata().ok()?;
            let data = (info.file_type().is_file() && info.len() == *size)
                .then(|| fs::read(&file).ok())
                .flatten()?;
            (sha256_hex(&data) == *sha).then(|| (name.clone(), data))
        })
        .collect()
}

/// Every reviewed file is in place with the reviewed hash.
pub(super) fn installed(root: &Root, reviewed: &Reviewed) -> bool {
    from_directory(reviewed, &root.path(&reviewed.target)).is_ok()
}

/// `shutil.rmtree(path, ignore_errors=True)`.
fn rmtree(path: &Path) {
    if path
        .symlink_metadata()
        .is_ok_and(|m| m.file_type().is_dir())
    {
        let _ = fs::remove_dir_all(path);
    }
}

/// Install the checked set on Y2DATA, keeping an installed supplement.
pub(super) fn install(root: &Root, reviewed: &Reviewed, files: Files) -> Result<()> {
    let mut files = checked(reviewed, files)?;
    let target = root.path(&reviewed.target);
    files.extend(kept_supplement(&target, reviewed));
    write_set(root, reviewed, files)
}

/// Add the checked supplement to the installed files (or to a new folder): the folder's other files
/// stay, so the same one-rename write as the set's.
pub(super) fn install_supplement(root: &Root, reviewed: &Reviewed, files: Files) -> Result<()> {
    let target = root.path(&reviewed.target);
    let mut all = files;
    if target.is_dir() && !target.is_symlink() {
        let names: Vec<String> = all.iter().map(|(n, _)| n.clone()).collect();
        for entry in fs::read_dir(&target).map_err(|e| os_path(e, &target))? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().into_owned();
            if names.contains(&name) || !entry.file_type()?.is_file() {
                continue;
            }
            all.push((name, fs::read(entry.path())?));
        }
    }
    write_set(root, reviewed, all)
}

/// Write the files to Y2DATA: a fresh directory, then one rename.
fn write_set(root: &Root, reviewed: &Reviewed, files: Files) -> Result<()> {
    let target = root.path(&reviewed.target);
    let (Some(parent), Some(name)) = (
        target.parent().map(Path::to_path_buf),
        target.file_name().map(|n| n.to_string_lossy().into_owned()),
    ) else {
        return Err(Error::Value("firmware_target_unsafe".into()));
    };
    crate::reply::mkdir_parents(&parent, 0o700)?;
    let pid = std::process::id();
    let staging = parent.join(format!(".{name}.{pid}"));
    rmtree(&staging);
    fs::DirBuilder::new()
        .mode(0o700)
        .create(&staging)
        .map_err(|e| os_path(e, &staging))?;
    for (name, data) in &files {
        let path = staging.join(name);
        let mut out = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(&path)
            .map_err(|e| os_path(e, &path))?;
        out.write_all(data).and_then(|()| out.sync_all())?;
    }
    let meta = target.symlink_metadata();
    if meta.as_ref().is_ok_and(|m| m.file_type().is_symlink())
        || (target.exists() && !target.is_dir())
    {
        return Err(Error::Value("firmware_target_unsafe".into()));
    }
    let old = parent.join(format!(".{name}.old.{pid}"));
    if target.exists() {
        fs::rename(&target, &old).map_err(|e| rename_error(e, &target, &old))?;
    }
    fs::rename(&staging, &target).map_err(|e| rename_error(e, &staging, &target))?;
    rmtree(&old);
    let directory = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_DIRECTORY)
        .open(&parent)
        .map_err(|e| os_path(e, &parent))?;
    directory.sync_all().map_err(Error::from)
}

fn rename_error(error: std::io::Error, from: &Path, to: &Path) -> Error {
    Error::Os(format!(
        "{} -> {}",
        py::os_error(&error, Some(&from.to_string_lossy())).0,
        py::repr(&to.to_string_lossy())
    ))
}
