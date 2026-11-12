//! The volumes of `observe.storage(ctx)['volumes']` as transfer, maintenance, diagnostics and update
//! read them (`transfer.data_volume`): root, Y2DATA, the SD card and Y2Media, in that consumer order,
//! each checked for its controller and filesystem UUID with the same `blkid` request. On the Innioasis
//! Y1 the root and Y2DATA are loop devices over its eMMC instead (docs/planning/Y1-RECOVERY-PLAN.md,
//! R4b). The SD card's
//! own claim and protected read-only mounts (`media-mount.json`) belong to the storage group's
//! observation; nothing here reads them.
// SPDX-License-Identifier: GPL-2.0-only
use crate::{command, mounts::mounts, player::Player, sys::statvfs, Root};
use serde::Serialize;
use std::path::{Path, PathBuf};

/// The volume paths in their consumer order (an index order consumers rely on).
pub const TARGETS: [&str; 4] = ["/", "/data", "/media/sd", "/media/internal"];

/// The eMMC's controller (MSDC0) and the SD card's (MSDC1), as their sysfs device paths name them.
pub const EMMC_CONTROLLER: &str = "11230000.mmc";
pub const SD_CONTROLLER: &str = "11240000.mmc";
/// The filesystem UUIDs the image's own volumes are made with.
pub const ROOT_UUID: &str = "79324c69-6e75-4801-8000-000000000101";
pub const DATA_UUID: &str = "79324c69-6e75-4801-8000-000000000102";
pub const Y2MEDIA_UUID: &str = "79324c69-6e75-4801-8000-000000000103";
/// Y2Media's partition (eMMC p8): its start and size in sectors, as sysfs `start`/`size` read.
pub const Y2MEDIA_START: &str = "3742720";
pub const Y2MEDIA_SIZE: &str = "11460608";
/// The Y1's eMMC controller (MSDC0 of the MT6572).
pub const Y1_EMMC_CONTROLLER: &str = "11120000.mmc";
/// The Y1's Y2DATA (tools/build/y1_data.py); its root is the same Y2ROOT image as the Y2's.
pub const Y1_DATA_UUID: &str = "79314c69-6e75-4801-8000-000000000102";
/// Linux sees the Y1's eMMC without partitions: PID1 puts ANDROID (read-only, the root) and USRDATA
/// (Y2DATA) on loop devices (initramfs/y1-root.h). Each target's window in bytes, as the loop's sysfs
/// `offset` and `sizelimit` read.
const Y1_LOOPS: [(&str, &str, &str); 2] = [
    ("/", "72613888", "681574400"),
    ("/data", "1291059200", "2147483648"),
];

/// The filesystem UUID a volume target must have (none for the SD card).
pub fn expected_uuid(player: Player, target: &str) -> Option<&'static str> {
    match (player, target) {
        (_, "/") => Some(ROOT_UUID),
        (Player::Y2, "/data") => Some(DATA_UUID),
        (Player::Y1, "/data") => Some(Y1_DATA_UUID),
        (_, "/media/internal") => Some(Y2MEDIA_UUID),
        _ => None,
    }
}

/// The controller a volume target's device sits on: the SD card on MSDC1, everything else on the
/// eMMC.
pub fn controller(target: &str) -> &'static str {
    if target == "/media/sd" {
        SD_CONTROLLER
    } else {
        EMMC_CONTROLLER
    }
}

/// Whether the block device `device_id` (`major:minor`) is the one `target` belongs on: below the
/// target's controller; on the Y1 the root and Y2DATA each their loop over exactly their window of
/// the whole eMMC.
pub fn source_valid(root: &Root, player: Player, target: &str, device_id: &str) -> bool {
    let device = root.path(&format!("/sys/dev/block/{device_id}"));
    if !device.exists() {
        return false;
    }
    match Y1_LOOPS.iter().find(|(path, ..)| *path == target) {
        Some((_, offset, size)) if player == Player::Y1 => {
            let read = |name: &str| crate::root::read(&device.join("loop").join(name), 64);
            read("offset").as_deref() == Some(offset)
                && read("sizelimit").as_deref() == Some(size)
                && read("backing_file").as_deref() == Some("/dev/mmcblk0")
                && resolve(&root.path("/sys/block/mmcblk0"))
                    .to_string_lossy()
                    .contains(Y1_EMMC_CONTROLLER)
        }
        _ => resolve(&device)
            .to_string_lossy()
            .contains(controller(target)),
    }
}

/// The Y1's root is read-only by design (a read-only loop, and the normal system's eMMC guard never
/// writes ANDROID): mounted so, it is Ready, not Degraded.
pub fn read_only_by_design(player: Player, target: &str) -> bool {
    player == Player::Y1 && target == "/"
}

/// A volume's state: `Ready`, `Degraded` (mounted read-only), `Failed` (wrong controller or
/// filesystem, or no statvfs), `Unavailable` (not mounted exactly once).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub enum VolumeState {
    Ready,
    Degraded,
    Failed,
    Unavailable,
}

/// A volume's capacity budget (`space_state`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub enum SpaceState {
    Normal,
    LowSpace,
    CriticalSpace,
    /// Mounted read-only.
    ReadOnlyRisk,
    Failed,
    Unavailable,
}

impl VolumeState {
    /// The text records and replies carry.
    pub fn as_str(self) -> &'static str {
        match self {
            VolumeState::Ready => "Ready",
            VolumeState::Degraded => "Degraded",
            VolumeState::Failed => "Failed",
            VolumeState::Unavailable => "Unavailable",
        }
    }
}

impl SpaceState {
    /// The text records and replies carry.
    pub fn as_str(self) -> &'static str {
        match self {
            SpaceState::Normal => "Normal",
            SpaceState::LowSpace => "LowSpace",
            SpaceState::CriticalSpace => "CriticalSpace",
            SpaceState::ReadOnlyRisk => "ReadOnlyRisk",
            SpaceState::Failed => "Failed",
            SpaceState::Unavailable => "Unavailable",
        }
    }
}

/// One entry of `storage(ctx)['volumes']`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Volume {
    pub path: &'static str,
    pub state: VolumeState,
    /// `<boot id>:<mount id>:<device id>`: the same value means the same mount of the same device
    /// this boot.
    pub generation: Option<String>,
    pub uuid: Option<String>,
    pub space_state: SpaceState,
    pub available_bytes: Option<u64>,
}

/// `observe.space_state`: product capacity budget, not electrical thresholds. Never delete user data.
pub fn space_state(available: u64, total: u64, readonly: bool, error: bool) -> SpaceState {
    const MIB: u64 = 1024 * 1024;
    if error {
        SpaceState::Failed
    } else if readonly {
        SpaceState::ReadOnlyRisk
    } else if available < (32 * MIB).max(total / 20) {
        SpaceState::CriticalSpace
    } else if available < (96 * MIB).max(total / 10) {
        SpaceState::LowSpace
    } else {
        SpaceState::Normal
    }
}

/// `Path.resolve()` (strict=False): symlinks resolved as far as the path exists.
fn resolve(path: &Path) -> PathBuf {
    if let Ok(real) = crate::sys::real_path(path) {
        return real;
    }
    let mut missing = Vec::new();
    let mut existing = path.to_path_buf();
    while !existing.as_os_str().is_empty() {
        if let Ok(mut real) = crate::sys::real_path(&existing) {
            for part in missing.iter().rev() {
                real.push(part);
            }
            return real;
        }
        match (existing.file_name(), existing.parent()) {
            (Some(name), Some(parent)) => {
                missing.push(name.to_os_string());
                existing = parent.to_path_buf();
            }
            _ => break,
        }
    }
    path.to_path_buf()
}

/// `observe.storage(ctx)['volumes']`. Stacked mounts are ambiguous to consumers: a target that is not
/// mounted exactly once is Unavailable.
pub fn volumes(root: &Root) -> Vec<Volume> {
    let mounts = mounts(root);
    let boot_id = root.boot_id();
    let player = Player::of(root);
    TARGETS
        .into_iter()
        .map(|target| probe(root, player, &mounts, boot_id.as_deref(), target))
        .collect()
}

/// One target's entry: its single mount's controller, filesystem UUID (`blkid`) and space.
fn probe(
    root: &Root,
    player: Player,
    mounts: &[crate::mounts::Mount],
    boot_id: Option<&str>,
    target: &'static str,
) -> Volume {
    {
        let found: Vec<_> = mounts.iter().filter(|m| m.path == target).collect();
        let [item] = found[..] else {
            return Volume {
                path: target,
                state: VolumeState::Unavailable,
                generation: None,
                uuid: None,
                space_state: SpaceState::Unavailable,
                available_bytes: None,
            };
        };
        let generation = boot_id
            .filter(|b| !b.is_empty())
            .map(|b| format!("{b}:{}:{}", item.mount_id, item.device_id));
        let controller_valid = source_valid(root, player, target, &item.device_id);
        let mut state = if controller_valid {
            VolumeState::Ready
        } else {
            VolumeState::Failed
        };
        let mut uuid = None;
        if controller_valid {
            let answer = command::run(
                root,
                &["blkid", "-p", "-s", "UUID", "-o", "value", &item.source],
                command::TIMEOUT,
            );
            uuid = if answer.ok {
                answer.output.filter(|o| !o.is_empty())
            } else {
                None
            };
            let expected_uuid = expected_uuid(player, target);
            if expected_uuid.is_some() && uuid.as_deref() != expected_uuid {
                state = VolumeState::Failed;
            }
        }
        let readonly = item.read_only() && !read_only_by_design(player, target);
        let (space_state, available_bytes) = match statvfs(&root.path(target)) {
            Ok(space) => {
                let mut space_state = space_state(
                    space.available(),
                    space.total(),
                    readonly,
                    state == VolumeState::Failed,
                );
                if space.files > 0 && space.favail == 0 {
                    space_state = SpaceState::CriticalSpace;
                }
                (space_state, Some(space.available()))
            }
            Err(_) => {
                state = VolumeState::Failed;
                (SpaceState::Failed, None)
            }
        };
        if state == VolumeState::Ready && readonly {
            state = VolumeState::Degraded;
        }
        Volume {
            path: target,
            state,
            generation,
            uuid,
            space_state,
            available_bytes,
        }
    }
}

/// The volume at `path` (one of [`TARGETS`]), probed alone: the other volumes (the SD card's
/// `blkid` and `statvfs` among them) are not touched.
pub fn volume(root: &Root, path: &str) -> Volume {
    let target = TARGETS
        .into_iter()
        .find(|t| *t == path)
        .expect("one of the four volume targets");
    probe(
        root,
        Player::of(root),
        &mounts(root),
        root.boot_id().as_deref(),
        target,
    )
}

/// The volume at `path` when it is verified: Ready, with a generation this boot.
pub fn verified(root: &Root, path: &str) -> Option<Volume> {
    Some(volume(root, path)).filter(|v| v.state == VolumeState::Ready && v.generation.is_some())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn space_states_follow_the_budget() {
        assert_eq!(
            space_state(10, 100, false, false),
            SpaceState::CriticalSpace
        );
        assert_eq!(
            space_state(1 << 40, 1 << 41, false, false),
            SpaceState::Normal
        );
        assert_eq!(
            space_state(80 << 20, 100 << 20, false, false),
            SpaceState::LowSpace
        );
        assert_eq!(
            space_state(1 << 40, 1 << 41, true, false),
            SpaceState::ReadOnlyRisk
        );
        assert_eq!(
            space_state(1 << 40, 1 << 41, true, true),
            SpaceState::Failed
        );
    }

    /// The Y1 (docs/planning/Y1-RECOVERY-PLAN.md, R4b): the root's read-only loop over ANDROID
    /// and Y2DATA's loop over USRDATA, as PID1 sets them up, are Ready; any other window, backing
    /// device, UUID or player is not.
    #[test]
    fn the_y1_volumes_are_loops_over_their_windows() {
        use crate::command::Outcome;
        use std::os::unix::fs::symlink;
        let dir = std::env::temp_dir().join(format!("y2p-volume-y1-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let put = |name: &str, text: &str| {
            let path = dir.join(name);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, text).unwrap();
        };
        put(
            "proc/device-tree/compatible",
            "innioasis,y1\0mediatek,mt6572\0",
        );
        put("proc/sys/kernel/random/boot_id", "boot-a\n");
        put(
            "proc/self/mountinfo",
            "20 1 7:0 / / ro,noatime - ext4 /dev/loop0 ro\n\
             30 20 7:1 / /data rw,nosuid,nodev,noatime - ext4 /dev/loop1 rw\n",
        );
        for (loop_, offset, size) in [
            ("loop0", "72613888", "681574400"),
            ("loop1", "1291059200", "2147483648"),
        ] {
            put(
                &format!("sys/devices/virtual/block/{loop_}/loop/offset"),
                &format!("{offset}\n"),
            );
            put(
                &format!("sys/devices/virtual/block/{loop_}/loop/sizelimit"),
                &format!("{size}\n"),
            );
            put(
                &format!("sys/devices/virtual/block/{loop_}/loop/backing_file"),
                "/dev/mmcblk0\n",
            );
        }
        std::fs::create_dir_all(dir.join("sys/dev/block")).unwrap();
        symlink(
            "../../devices/virtual/block/loop0",
            dir.join("sys/dev/block/7:0"),
        )
        .unwrap();
        symlink(
            "../../devices/virtual/block/loop1",
            dir.join("sys/dev/block/7:1"),
        )
        .unwrap();
        std::fs::create_dir_all(
            dir.join("sys/devices/platform/11120000.mmc/mmc_host/mmc0/mmc0:0001/block/mmcblk0"),
        )
        .unwrap();
        std::fs::create_dir_all(dir.join("sys/block")).unwrap();
        symlink(
            "../devices/platform/11120000.mmc/mmc_host/mmc0/mmc0:0001/block/mmcblk0",
            dir.join("sys/block/mmcblk0"),
        )
        .unwrap();
        std::fs::create_dir_all(dir.join("data")).unwrap();
        let blkid = |device: &str, uuid: &str| {
            (
                ["blkid", "-p", "-s", "UUID", "-o", "value", device]
                    .map(String::from)
                    .to_vec(),
                Outcome {
                    ok: true,
                    reason: None,
                    output: Some(uuid.into()),
                },
            )
        };
        let probe = |data_uuid: &str| {
            let root = Root::fixture(&dir).with_answers(vec![
                blkid("/dev/loop0", ROOT_UUID),
                blkid("/dev/loop1", data_uuid),
            ]);
            (volume(&root, "/").state, volume(&root, "/data").state)
        };
        assert_eq!(
            probe(Y1_DATA_UUID),
            (VolumeState::Ready, VolumeState::Ready)
        );
        // The Y2's Y2DATA is not the Y1's.
        assert_eq!(probe(DATA_UUID).1, VolumeState::Failed);
        // Another window, or a loop over anything but the eMMC.
        put(
            "sys/devices/virtual/block/loop1/loop/offset",
            "1291059712\n",
        );
        assert_eq!(probe(Y1_DATA_UUID).1, VolumeState::Failed);
        put(
            "sys/devices/virtual/block/loop1/loop/offset",
            "1291059200\n",
        );
        put(
            "sys/devices/virtual/block/loop1/loop/backing_file",
            "/data/reborn/data.img\n",
        );
        assert_eq!(probe(Y1_DATA_UUID).1, VolumeState::Failed);
        // On the Y2 a loop is never a volume, and a read-only root is Degraded.
        put(
            "sys/devices/virtual/block/loop1/loop/backing_file",
            "/dev/mmcblk0\n",
        );
        put(
            "proc/device-tree/compatible",
            "innioasis,y2\0mediatek,mt6582\0",
        );
        assert_eq!(probe(DATA_UUID), (VolumeState::Failed, VolumeState::Failed));
        assert!(!read_only_by_design(Player::Y2, "/"));
        assert!(!read_only_by_design(Player::Y1, "/data"));
        std::fs::remove_dir_all(dir).unwrap();
    }
}
