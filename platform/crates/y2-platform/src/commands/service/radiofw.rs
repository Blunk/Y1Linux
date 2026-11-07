//! `y2-platform firmware`: radio firmware from the owner's own copy (RELEASE-49).
//!
//! Y2Linux images carry no MediaTek radio firmware: it may not be redistributed. Each player keeps the
//! reviewed files on Y2DATA (`/data/firmware/mt6582`, which `/lib/firmware/mediatek/mt6582` links
//! to). They come from the owner's copy: on a player that ran an older image, the recovery copies them
//! from the old root before it writes the new one (recovery.sh); otherwise the owner copies
//! `system.img` from the Innioasis Y2 firmware package into the `Firmware` folder over USB (on Internal
//! Storage, or the Firmware drive on Y2DATA while there is no Internal Storage); this module reads the
//! Android sparse ext4 image itself, checks the five files against the reviewed SHA-256 values,
//! derives the two board defaults like tools/connectivity/provision.py, installs them, deletes the copy
//! and starts the radios. Only names, sizes and hashes are in this file; never firmware bytes.
//!
//! Another player's image describes its own set in `/etc/y2linux/radio-firmware.json` (`Config`; the
//! Innioasis Y1: docs/planning/Y1-BLUETOOTH-PLAN.md §16); without that file the Y2's tables below apply.
// SPDX-License-Identifier: GPL-2.0-only
use super::image::{self, Ext4, Image};
use crate::commands::storage::media;
use crate::reply::{Failure, Outcome, Reply};
use serde::{Deserialize, Serialize};
use std::{
    fs::{self, OpenOptions},
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};
use y2p_core::{
    command,
    digest::sha256_hex,
    json::{Object, ObjectExt},
    py, record, Root,
};

mod copy;
mod files;
mod fm;

pub(crate) use self::copy::inbox;
use self::{copy::*, files::*};
pub use self::{
    copy::{candidates, internal, leftover_waiting, state as status, Status},
    files::present,
};

/// docs/knowledge/evidence/m5-firmware-inventory.json (stock v3.2.0 FM, /etc/firmware).
pub const FILES: [(&str, u64, &str); 5] = [
    (
        "WIFI_RAM_CODE_MT6582",
        160368,
        "4def37522e2bfd2d4b78e01bf9f567a37e3f67ab3d7b6646ffb317f5b2b49b82",
    ),
    (
        "WMT_SOC.cfg",
        80,
        "cea0a94b8a88e9c24e6835446ebc3106d7b100c3709ea98e8bb7368689c8a2b5",
    ),
    (
        "mt6572_82_patch_e1_0_hdr.bin",
        33120,
        "fa3be4a92540296de512d64f56272746be10bdfa2db571c8ceada2d40cd616f0",
    ),
    (
        "mt6572_82_patch_e1_1_hdr.bin",
        17476,
        "96eb25489305bd472805db1da33bcc0acd37b5bb21cceda98d7e88ba1179ca37",
    ),
    (
        "modem_1_2g_n.img",
        2424376,
        "5059775975cbf6ab74c43978ca8f65d9a274b83585f456134e39b09f6dc7a4f1",
    ),
];
/// provision.py: generic board defaults derived from the stock reader library.
pub const DEFAULTS: [(&str, u64, &str); 2] = [
    (
        "WIFI.defaults",
        512,
        "eb5e87224e3a493e825179f547fc13924f564a42730ea821b4359b5b36f4defe",
    ),
    (
        "BT.defaults",
        64,
        "f224ff4abfb45a612ef03249e87e4ecfa44b7c1f9f19ee3cc78ae354764d9472",
    ),
];
pub const CUSTOM: (&str, u64, &str) = (
    "/lib/libcustom_nvram.so",
    51224,
    "c6460e9649f539b56449dc19f8d20771402f539be9e73340a74a9ccf45287397",
);

/// The image's own radio firmware set; the root is read-only, so the image vouches for it.
pub const CONFIG: &str = "/etc/y2linux/radio-firmware.json";

/// `radio-firmware.json`: where the set goes and where the owner's copy goes, the files of the stock
/// image's /etc/firmware, and the board defaults cut from one stock library.
#[derive(Deserialize, Debug)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub schema: u32,
    pub target: String,
    #[serde(default)]
    pub inbox: Option<String>,
    /// The note put into the inbox folder (README.txt).
    #[serde(default)]
    pub note: Option<String>,
    pub files: Vec<Entry>,
    pub library: Entry,
    pub defaults: Vec<Slice>,
    /// The patches' download sequence numbers (header byte 24, low nibble), sorted.
    pub patches: Vec<u8>,
}

/// A file with its reviewed size and SHA-256 (`name` is a path for the library).
#[derive(Deserialize, Debug)]
#[serde(deny_unknown_fields)]
pub struct Entry {
    pub name: String,
    pub size: u64,
    pub sha256: String,
}

/// A default record cut from the library: `size` bytes at `offset`, which must start with `expect`
/// (hex), the first `clear` bytes zeroed (the generic address), then checked against `sha256`.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Slice {
    pub name: String,
    pub offset: usize,
    pub size: u64,
    pub expect: String,
    pub clear: usize,
    pub sha256: String,
}

/// The reviewed tables (a test swaps them for its fixture files', as tests/test_radio_firmware.py).
#[derive(Clone)]
pub struct Reviewed {
    pub files: Vec<(String, u64, String)>,
    pub defaults: Vec<(String, u64, String)>,
    pub custom: (String, u64, String),
    /// How the defaults come out of the library: the Y2's reader (None) or the configured slices.
    pub slices: Option<Vec<Slice>>,
    pub patches: Vec<u8>,
    pub target: String,
    pub inbox: Option<String>,
    pub note: Option<String>,
}

impl Reviewed {
    pub fn stock() -> Self {
        let own = |t: &[(&str, u64, &str)]| {
            t.iter()
                .map(|(n, s, h)| (n.to_string(), *s, h.to_string()))
                .collect()
        };
        Self {
            files: own(&FILES),
            defaults: own(&DEFAULTS),
            custom: (CUSTOM.0.into(), CUSTOM.1, CUSTOM.2.into()),
            slices: None,
            patches: vec![1, 2],
            target: TARGET.into(),
            inbox: None,
            note: None,
        }
    }

    /// This image's set: its `radio-firmware.json`, else the Y2's. A file that is there but not a
    /// valid set is an error, never the Y2's tables.
    pub fn of(root: &Root) -> Result<Self> {
        let path = root.path(CONFIG);
        if path.symlink_metadata().is_err() {
            return Ok(Self::stock());
        }
        let invalid = || Error::Value("radio_firmware_config".into());
        let text = fs::read_to_string(&path).map_err(|_| invalid())?;
        let config: Config = serde_json::from_str(&text).map_err(|_| invalid())?;
        Self::from_config(config).ok_or_else(invalid)
    }

    fn from_config(c: Config) -> Option<Self> {
        let plain = |name: &str| !name.is_empty() && !name.contains('/') && !name.starts_with('.');
        let sha = |hex: &str| hex.len() == 64 && hex.bytes().all(|b| b.is_ascii_hexdigit());
        let absolute = |p: &str| p.starts_with('/') && !p.split('/').any(|part| part == "..");
        let valid = c.schema == 1
            && absolute(&c.target)
            && c.target.starts_with("/data/firmware/")
            && c.inbox.as_deref().is_none_or(absolute)
            && absolute(&c.library.name)
            && sha(&c.library.sha256)
            && !c.files.is_empty()
            && c.files.iter().all(|f| plain(&f.name) && sha(&f.sha256))
            && c.defaults.iter().all(|d| {
                plain(&d.name)
                    && sha(&d.sha256)
                    && d.clear as u64 <= d.size
                    && hex(&d.expect).is_some_and(|e| e.len() as u64 <= d.size)
            });
        if !valid {
            return None;
        }
        let table = |entries: &[Entry]| {
            entries
                .iter()
                .map(|e| (e.name.clone(), e.size, e.sha256.to_ascii_lowercase()))
                .collect()
        };
        Some(Self {
            files: table(&c.files),
            defaults: c
                .defaults
                .iter()
                .map(|d| (d.name.clone(), d.size, d.sha256.to_ascii_lowercase()))
                .collect(),
            custom: (
                c.library.name,
                c.library.size,
                c.library.sha256.to_ascii_lowercase(),
            ),
            slices: Some(c.defaults),
            patches: c.patches,
            target: c.target,
            inbox: c.inbox,
            note: c.note,
        })
    }

    fn all(&self) -> impl Iterator<Item = &(String, u64, String)> {
        self.files.iter().chain(self.defaults.iter())
    }
}

const TARGET: &str = "/data/firmware/mt6582";

/// `bytes.fromhex(text)`.
fn hex(text: &str) -> Option<Vec<u8>> {
    if !text.len().is_multiple_of(2) {
        return None;
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(text.get(i..i + 2)?, 16).ok())
        .collect()
}
/// While the firmware is missing the computer sees this folder as the MTP storage "Firmware"
/// (usb.media_views); nothing else of Y2DATA is exported.
pub(crate) const INBOX: &str = "/data/firmware/inbox";
pub const STATE: &str = "/run/y2/radio-firmware.json";
const FOLDER: &str = "Firmware";
const SOURCE: &str = "system.img";
/// With Internal Storage the copy goes there (#39: Y2DATA has room for the zip's 518 MB image, never
/// for an unpacked 860 MB one).
const INTERNAL_INBOX: &str = "/media/internal/music/Firmware";
const SD_INBOX: &str = "/media/sd/Firmware";
const ENOSPC: i64 = 28;
const README: &str =
    "Wi-Fi and Bluetooth need the radio firmware from the Innioasis Y2 firmware package.\n\
Copy (do not move) the file system.img from that package into this folder (about\n\
520 MB). The player checks it, keeps the five small radio files and deletes the copy.\n\
A larger system.img (about 860 MB) is an unpacked copy: if there is not enough space\n\
here, put it into a folder named Firmware on the SD card instead.\n";

/// What radiofw raises (its callers catch OSError and ValueError alike, by text).
#[derive(Debug)]
pub enum Error {
    Value(String),
    Os(String),
}

impl Error {
    pub fn text(&self) -> String {
        match self {
            Error::Value(t) | Error::Os(t) => t.clone(),
        }
    }
}

/// An `OSError` naming no file.
impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Error {
        Error::Os(y2p_core::py::os_error(&error, None).0)
    }
}

impl From<image::Error> for Error {
    fn from(error: image::Error) -> Error {
        match error {
            image::Error::Value(t) => Error::Value(t),
            image::Error::Os(t) => Error::Os(t),
            // FileNotFoundError(path): only from a lookup, which every caller turns into a ValueError.
            image::Error::NotFound => Error::Value("not_the_y2_firmware_package".into()),
        }
    }
}

impl From<Failure> for Error {
    fn from(failure: Failure) -> Error {
        match failure {
            Failure::Value(t) => Error::Value(t),
            other => Error::Os(other.message()),
        }
    }
}

fn os_path(error: std::io::Error, path: &Path) -> Error {
    Error::Os(py::os_error(&error, Some(&path.to_string_lossy())).0)
}

type Result<T> = std::result::Result<T, Error>;
type Files = Vec<(String, Vec<u8>)>;

/// (re)start the radios with the installed firmware. Stopping running radios can take
/// longer than a short command limit, which on 49 cut the combined restart off after its stop half
/// (archive 154).
fn start(root: &Root) {
    command::run(root, &["/etc/init.d/S41y2-connectivity", "stop"], 90.0);
    command::run(root, &["/etc/init.d/S41y2-connectivity", "start"], 30.0);
}

/// `y2-platform firmware <action> [path]`.
pub fn run(root: &Root, action: &str, path: Option<&str>) -> Outcome {
    let result = (|| -> Result<Status> {
        let reviewed = Reviewed::of(root)?;
        match action {
            "status" => Ok(state(root)),
            "scan" => scan(root, &reviewed),
            _ => {
                let path = path.filter(|p| !p.is_empty()).ok_or_else(|| {
                    Error::Value("install needs a system.img or a directory".into())
                })?;
                let source = PathBuf::from(path);
                let files = if source.is_dir() {
                    from_directory(&reviewed, &source)?
                } else {
                    from_system_image(&reviewed, &source)?
                };
                install(root, &reviewed, files)?;
                start(root);
                Ok(Status::of("Ready", None))
            }
        }
    })();
    match result {
        // A scan whose copy could not be installed exits 1 as an install does (it exited 0 with
        // state Failed); the service ignores the code and reads the state record.
        Ok(value) => Reply::sorted(&value, u8::from(value.state == "Failed")),
        Err(error) => Reply::unsorted(
            &Object::new()
                .with("state", "Failed")
                .with("failure", error.text()),
            1,
        ),
    }
}

#[cfg(test)]
#[path = "radiofw_tests.rs"]
mod radiofw_tests;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testing::Fixture;
    use serde_json::json;
    use std::time::Duration;

    /// One scan as JSON.
    fn scan(root: &Root, reviewed: &Reviewed) -> Result<serde_json::Value> {
        super::copy::scan(root, reviewed).map(|status| json!(status))
    }

    #[test]
    fn a_scan_whose_copy_cannot_be_installed_exits_1() {
        let fixture = Fixture::new();
        let image = format!("{INBOX}/{SOURCE}");
        fixture.put(&image, "not a system image");
        // A finished copy: older than the arrival window, and seen unchanged on the pass before.
        let file = fs::File::options()
            .write(true)
            .open(fixture.path(&image))
            .unwrap();
        file.set_modified(SystemTime::now() - Duration::from_secs(60))
            .unwrap();
        let (seen, arriving) = seen(&file.metadata().unwrap());
        assert!(!arriving);
        fixture.put_json(
            STATE,
            &json!({"schema": 1, "state": "Missing", "seen": seen}),
        );
        let reply = run(&fixture.root, "scan", None).unwrap();
        assert_eq!(reply.code, 1, "{}", reply.stdout);
        assert_eq!(fixture.json(STATE)["state"], json!("Failed"));
        // The next pass reports the same failure, still as one.
        assert_eq!(run(&fixture.root, "scan", None).unwrap().code, 1);
    }

    fn patch(sequence: u8) -> Vec<u8> {
        let mut data = vec![7u8; 64];
        data[18..20].copy_from_slice(b"PS");
        data[24] = 0x20 | sequence;
        data
    }

    fn custom_library() -> Vec<u8> {
        let mut data: Vec<u8> = (0..51224u32).map(|i| (i * 13 % 256) as u8).collect();
        data[..7].copy_from_slice(b"\x7fELF\x01\x01\x01");
        data[0x5a8e..0x5a92].copy_from_slice(&[0x04, 0x01, 0, 0]);
        data[0x5a8e + 196..0x5a8e + 198].copy_from_slice(b"\x01\x00");
        data
    }

    /// tests/test_radio_firmware.py: fixture files with the reviewed tables swapped for theirs.
    fn fixture() -> (Reviewed, Files, Vec<u8>) {
        let files: Files = vec![
            ("WIFI_RAM_CODE_MT6582".into(), vec![1u8; 5000]),
            ("WMT_SOC.cfg".into(), b"coex_wmt_ant_mode=1\n".to_vec()),
            ("mt6572_82_patch_e1_0_hdr.bin".into(), patch(2)),
            ("mt6572_82_patch_e1_1_hdr.bin".into(), patch(1)),
            ("modem_1_2g_n.img".into(), vec![9u8; 7000]),
        ];
        let custom = custom_library();
        let derived = defaults(&custom).unwrap();
        let table = |f: &Files| {
            f.iter()
                .map(|(n, d)| (n.clone(), d.len() as u64, sha256_hex(d)))
                .collect()
        };
        let reviewed = Reviewed {
            files: table(&files),
            defaults: table(&derived),
            custom: (CUSTOM.0.into(), custom.len() as u64, sha256_hex(&custom)),
            ..Reviewed::stock()
        };
        (reviewed, files, custom)
    }

    #[test]
    fn a_system_image_installs_and_its_copy_goes() {
        let (reviewed, files, custom) = fixture();
        let dir = std::env::temp_dir().join(format!("y2-radiofw-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let tree = dir.join("tree");
        fs::create_dir_all(tree.join("etc/firmware")).unwrap();
        fs::create_dir_all(tree.join("lib")).unwrap();
        for (name, data) in &files {
            fs::write(tree.join("etc/firmware").join(name), data).unwrap();
        }
        fs::write(tree.join("lib/libcustom_nvram.so"), &custom).unwrap();
        let root = Root::new(dir.join("root"));
        fs::create_dir_all(root.path("/run/y2")).unwrap();
        let image = root.path(INBOX).join(SOURCE);
        fs::create_dir_all(image.parent().unwrap()).unwrap();
        let made = std::process::Command::new("mke2fs")
            .args(["-q", "-F", "-b", "4096", "-d"])
            .arg(&tree)
            .arg(&image)
            .arg("8M")
            .output()
            .is_ok_and(|o| o.status.success());
        if !made {
            let _ = fs::remove_dir_all(&dir);
            return; // no host mke2fs
        }
        let old = SystemTime::now() - Duration::from_secs(60);
        fs::File::options()
            .write(true)
            .open(&image)
            .unwrap()
            .set_modified(old)
            .unwrap();
        // The first pass only notes the copy; the second takes it.
        assert_eq!(scan(&root, &reviewed).unwrap(), json!({"state": "Missing"}));
        assert_eq!(scan(&root, &reviewed).unwrap(), json!({"state": "Ready"}));
        assert!(!image.exists());
        assert!(installed(&root, &reviewed));
        let wifi = fs::read(root.path(TARGET).join("WIFI.defaults")).unwrap();
        assert_eq!(wifi.len(), 512);
        assert_eq!(&wifi[4..10], &[0; 6]);
        assert_eq!(
            root.json(STATE).unwrap(),
            json!({"schema": 1, "state": "Ready", "installed_from": "system.img"})
        );
        // A second copy is judged and deleted by leftover() (the stock tables reject it: kept).
        assert_eq!(scan(&root, &reviewed).unwrap(), json!({"state": "Ready"}));
        // from_directory reads the installed set back; checked() refuses a wrong patch order.
        let back = from_directory(&reviewed, &root.path(TARGET)).unwrap();
        let mut swapped = back.clone();
        for item in &mut swapped {
            if item.0.ends_with("_hdr.bin") {
                item.1[24] = 0x21;
            }
        }
        let mut reordered = reviewed.clone();
        for (name, size, sha) in &mut reordered.files {
            if let Some(data) = get(&swapped, name) {
                *size = data.len() as u64;
                *sha = sha256_hex(data);
            }
        }
        assert_eq!(
            checked(&reordered, swapped).unwrap_err().text(),
            "firmware_patch_order"
        );
        let stock = Reviewed::stock();
        assert_eq!(
            checked(&stock, back).unwrap_err().text(),
            "firmware_mismatch:WIFI_RAM_CODE_MT6582"
        );
        fs::remove_dir_all(&dir).unwrap();
    }

    /// A synthetic library with a generic record at +0x3004 (never stock bytes).
    fn library(record: [u8; 6]) -> Vec<u8> {
        let mut data: Vec<u8> = (0..13452u32).map(|i| (i * 7 % 256) as u8).collect();
        data[..7].copy_from_slice(b"\x7fELF\x01\x01\x01");
        data[0x3004..0x300a].copy_from_slice(&record);
        data
    }

    const GENERIC: [u8; 6] = [0x00, 0x00, 0x46, 0x65, 0x72, 0x01];

    /// A radio-firmware.json for `files` and `library`, as the Y1's image carries one.
    fn config(files: &Files, library: &[u8]) -> serde_json::Value {
        let slice = Slice {
            name: "BT.defaults".into(),
            offset: 0x3004,
            size: 32,
            expect: "000046657201".into(),
            clear: 6,
            sha256: String::new(),
        };
        let derived = files::sliced(std::slice::from_ref(&slice), library).unwrap();
        let entry = |name: &str, data: &[u8]| {
            let sha256 = sha256_hex(data);
            json!({"name": name, "size": data.len(), "sha256": sha256})
        };
        let mut defaults = json!(slice);
        defaults["sha256"] = json!(sha256_hex(&derived[0].1));
        json!({
            "schema": 1, "target": "/data/firmware/mt6572", "inbox": "/data/music/Firmware",
            "note": "Copy system.img from the Y1 package here.\n",
            "files": files.iter().map(|(n, d)| entry(n, d)).collect::<Vec<_>>(),
            "library": entry("/lib/libbluetooth_mtk.so", library),
            "defaults": [defaults],
            "patches": [1],
        })
    }

    fn y1_files() -> Files {
        let mut patch = vec![5u8; 39868];
        patch[24] = 0x11;
        vec![
            ("WMT_SOC.cfg".into(), vec![b'a'; 82]),
            ("mt6572_82_patch_e1_0_hdr.bin".into(), patch),
        ]
    }

    #[test]
    fn an_image_without_a_config_keeps_the_y2_tables() {
        let fixture = Fixture::new();
        let reviewed = Reviewed::of(&fixture.root).unwrap();
        assert_eq!(
            (reviewed.target.as_str(), reviewed.patches.clone()),
            (TARGET, vec![1, 2])
        );
        assert!(reviewed.slices.is_none() && reviewed.inbox.is_none());
        assert_eq!(reviewed.all().count(), 7);
    }

    #[test]
    fn a_configured_set_is_read_and_a_bad_one_is_refused() {
        let fixture = Fixture::new();
        let good = config(&y1_files(), &library(GENERIC));
        fixture.put_json(CONFIG, &good);
        let reviewed = Reviewed::of(&fixture.root).unwrap();
        assert_eq!(reviewed.target, "/data/firmware/mt6572");
        assert_eq!(
            (reviewed.patches.clone(), reviewed.all().count()),
            (vec![1], 3)
        );
        assert_eq!(reviewed.custom.0, "/lib/libbluetooth_mtk.so");
        let mut bad = Vec::new();
        for (key, value) in [
            ("schema", json!(2)),
            ("target", json!("/data/../etc")),
            ("target", json!("/etc/firmware")),
            ("inbox", json!("music/Firmware")),
            ("files", json!([])),
            (
                "files",
                json!([{"name": "../x", "size": 1, "sha256": "0".repeat(64)}]),
            ),
            ("patches", json!("1")),
            ("surprise", json!(true)),
        ] {
            let mut changed = good.clone();
            changed[key] = value;
            bad.push(changed);
        }
        let mut slice = good.clone();
        slice["defaults"][0]["expect"] = json!("0");
        bad.push(slice);
        for value in bad {
            fixture.put_json(CONFIG, &value);
            assert_eq!(
                Reviewed::of(&fixture.root).err().map(|e| e.text()),
                Some("radio_firmware_config".into()),
                "{value}"
            );
            // Never the Y2's tables instead: the status says why, and nothing counts as present.
            assert_eq!(
                json!(state(&fixture.root)),
                json!({"state": "Failed", "reason": "radio_firmware_config"})
            );
            assert!(!present(&fixture.root));
        }
        fixture.put(CONFIG, "not json");
        assert!(Reviewed::of(&fixture.root).is_err());
    }

    #[test]
    fn configured_defaults_need_the_stock_layout() {
        let slice = Slice {
            name: "BT.defaults".into(),
            offset: 0x3004,
            size: 32,
            expect: "000046657201".into(),
            clear: 6,
            sha256: String::new(),
        };
        let stock = library(GENERIC);
        let cut = files::sliced(std::slice::from_ref(&slice), &stock).unwrap();
        assert_eq!(&cut[0].1[..6], &[0; 6]);
        assert_eq!(&cut[0].1[6..], &stock[0x300a..0x3024]);
        let moved = library([2, 0x11, 0x22, 0x33, 0x44, 0x55]);
        assert_eq!(
            files::sliced(std::slice::from_ref(&slice), &moved)
                .unwrap_err()
                .text(),
            "stock_reader_library_layout"
        );
        let short = &stock[..0x3010];
        assert!(files::sliced(std::slice::from_ref(&slice), short).is_err());
    }

    #[test]
    fn a_configured_set_installs_from_its_system_image_and_its_copy_goes() {
        let dir = std::env::temp_dir().join(format!("y2-radiofw-config-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let tree = dir.join("tree");
        fs::create_dir_all(tree.join("etc/firmware")).unwrap();
        fs::create_dir_all(tree.join("lib")).unwrap();
        let files = y1_files();
        for (name, data) in &files {
            fs::write(tree.join("etc/firmware").join(name), data).unwrap();
        }
        let library = library(GENERIC);
        fs::write(tree.join("lib/libbluetooth_mtk.so"), &library).unwrap();
        let root = Root::new(dir.join("root"));
        for folder in ["/run/y2", "/etc/y2linux", "/data/music"] {
            fs::create_dir_all(root.path(folder)).unwrap();
        }
        let text = serde_json::to_string(&config(&files, &library)).unwrap();
        fs::write(root.path(CONFIG), text).unwrap();
        let reviewed = Reviewed::of(&root).unwrap();
        // The first pass makes the configured folder with the configured note.
        assert_eq!(scan(&root, &reviewed).unwrap(), json!({"state": "Missing"}));
        let inbox = root.path("/data/music/Firmware");
        let note = fs::read_to_string(inbox.join("README.txt")).unwrap();
        assert_eq!(note, "Copy system.img from the Y1 package here.\n");
        let image = inbox.join(SOURCE);
        let made = std::process::Command::new("mke2fs")
            .args(["-q", "-F", "-b", "4096", "-d"])
            .arg(&tree)
            .arg(&image)
            .arg("8M")
            .output()
            .is_ok_and(|o| o.status.success());
        if !made {
            let _ = fs::remove_dir_all(&dir);
            return; // no host mke2fs
        }
        fs::File::options()
            .write(true)
            .open(&image)
            .unwrap()
            .set_modified(SystemTime::now() - Duration::from_secs(60))
            .unwrap();
        assert_eq!(scan(&root, &reviewed).unwrap(), json!({"state": "Missing"}));
        assert_eq!(scan(&root, &reviewed).unwrap(), json!({"state": "Ready"}));
        assert!(!image.exists() && !inbox.exists());
        assert!(installed(&root, &reviewed) && present(&root));
        let bt = fs::read(root.path("/data/firmware/mt6572/BT.defaults")).unwrap();
        assert_eq!((bt.len(), &bt[..6]), (32, &[0u8; 6][..]));
        assert!(
            !root.path(TARGET).exists(),
            "the Y2's folder stays untouched"
        );
        assert_eq!(json!(state(&root)), json!({"state": "Ready"}));
        // The Y2's tables refuse this set.
        let back = from_directory(&reviewed, &root.path("/data/firmware/mt6572")).unwrap();
        assert_eq!(
            checked(&Reviewed::stock(), back).unwrap_err().text(),
            "firmware_mismatch:WIFI_RAM_CODE_MT6582"
        );
        fs::remove_dir_all(&dir).unwrap();
    }
}
