//! Health and readiness never turn corrupt records or missing devices green (were
//! tests/test_platform_contract.py).
// SPDX-License-Identifier: GPL-2.0-only
use super::*;
use crate::testing::Fixture;

/// The fixture's root with every command answered `fixture`, never from the process environment (as
/// in snapshot_tests.rs).
fn unanswered(fixture: &Fixture) -> Root {
    fixture.root.clone().with_answers(Vec::new())
}

fn contract() -> Fixture {
    let fixture = Fixture::new();
    fixture
        .put("/proc/sys/kernel/random/boot_id", "boot-a")
        .put("/proc/sys/kernel/osrelease", "fixture-kernel")
        .put_json(
            "/etc/y2linux/versions.json",
            &json!({"build_git_commit": "commit", "rootfs_version": "release"}),
        );
    fixture
}

fn audio(fixture: &Fixture) -> Value {
    json!(
        snapshot(&unanswered(fixture), &[], false, 0.0)
            .unwrap()
            .readiness
            .audio
            .state
    )
}

fn filesystem_errors(fixture: &Fixture) -> Value {
    let health = json!(check(&unanswered(fixture), false).unwrap());
    health["checks"]
        .as_array()
        .unwrap()
        .iter()
        .find(|c| c["name"] == "filesystem_errors")
        .unwrap()["state"]
        .clone()
}

#[test]
fn record_corruption_and_no_devices_are_not_green() {
    let fixture = contract();
    fixture
        .put("/etc/y2linux/versions.json", "[]")
        .put("/data/updates/state.json", "{}")
        .put("/proc/asound/cards", "--- no soundcards ---")
        .put("/etc/y2linux/audio-qualified.json", "{}");
    let result = snapshot(&unanswered(&fixture), &[], false, 0.0).unwrap();
    assert_eq!(json!(result.readiness.audio.state), "Unavailable");
    assert!(result.record.y2linux_commit.is_none());
    let health = json!(check(&unanswered(&fixture), false).unwrap());
    assert_ne!(health["state"], "OK");
    assert_eq!(filesystem_errors(&fixture), "UNAVAILABLE");
}

#[test]
fn unreadable_filesystem_counter_and_invalid_audio_profile_are_not_ready() {
    let fixture = contract();
    fixture
        .put(
            "/sys/fs/ext4/fixture/errors_count",
            "not a readable counter",
        )
        .put("/proc/asound/cards", " 0 [Y2Audio ]: fixture")
        .put(
            "/etc/y2linux/audio-qualified.json",
            r#"{"note":"a nonempty corrupt profile"}"#,
        );
    assert_eq!(audio(&fixture), "Unavailable");
    assert_eq!(filesystem_errors(&fixture), "UNAVAILABLE");
    fixture.put("/sys/fs/ext4/fixture/errors_count", "1");
    assert_eq!(filesystem_errors(&fixture), "FAILED");
    let profile = json!({"schema": "org.y2linux.audio-qualification/v1", "card": "Y2Audio",
                         "channels": 2, "qualified_formats": ["S16_LE"],
                         "qualified_rates": [44100]});
    fixture.put_json("/etc/y2linux/audio-qualified.json", &profile);
    assert_eq!(audio(&fixture), "Ready");
    fixture.put("/proc/asound/cards", " 0 [OtherCard ]: unrelated card");
    assert_eq!(audio(&fixture), "Unavailable");
}

/// The Innioasis Y1 (docs/planning/Y1-RECOVERY-PLAN.md, R4b): its read-only root and Y2DATA, each a
/// loop over its window of the eMMC, are the `root` and `data` an update's health acknowledgement
/// needs OK; the same mounts on the Y2 are not.
#[test]
fn the_y1_root_and_data_loops_are_ok() {
    use std::os::unix::fs::symlink;
    let fixture = contract();
    fixture
        .put(
            "/proc/device-tree/compatible",
            "innioasis,y1\0mediatek,mt6572\0",
        )
        .put(
            "/proc/self/mountinfo",
            "20 1 7:0 / / ro,noatime - ext4 /dev/loop0 ro\n\
             30 20 7:1 / /data rw,nosuid,nodev,noatime - ext4 /dev/loop1 rw\n",
        )
        .mkdir("/sys/dev/block")
        .mkdir("/sys/devices/platform/11120000.mmc/mmc_host/mmc0/mmc0:0001/block/mmcblk0")
        .mkdir("/sys/block")
        .mkdir("/data");
    for (device, offset, size) in [
        ("loop0", "72613888", "681574400"),
        ("loop1", "1291059200", "2147483648"),
    ] {
        let base = format!("/sys/devices/virtual/block/{device}/loop");
        fixture
            .put(&format!("{base}/offset"), offset)
            .put(&format!("{base}/sizelimit"), size)
            .put(&format!("{base}/backing_file"), "/dev/mmcblk0\n");
    }
    symlink(
        "../../devices/virtual/block/loop0",
        fixture.path("/sys/dev/block/7:0"),
    )
    .unwrap();
    symlink(
        "../../devices/virtual/block/loop1",
        fixture.path("/sys/dev/block/7:1"),
    )
    .unwrap();
    symlink(
        "../devices/platform/11120000.mmc/mmc_host/mmc0/mmc0:0001/block/mmcblk0",
        fixture.path("/sys/block/mmcblk0"),
    )
    .unwrap();
    let states = |fixture: &Fixture| {
        let blkid = |device: &str, uuid: &str| {
            crate::testing::ok(&["blkid", "-p", "-s", "UUID", "-o", "value", device], uuid)
        };
        let root = fixture.root.clone().with_answers(vec![
            blkid("/dev/loop0", y2p_core::volume::ROOT_UUID),
            blkid("/dev/loop1", y2p_core::volume::Y1_DATA_UUID),
        ]);
        let health = json!(check(&root, false).unwrap());
        let state = |name: &str| {
            health["checks"]
                .as_array()
                .unwrap()
                .iter()
                .find(|c| c["name"] == name)
                .unwrap()["state"]
                .clone()
        };
        (state("root"), state("data"))
    };
    assert_eq!(states(&fixture), (json!("OK"), json!("OK")));
    fixture.put(
        "/proc/device-tree/compatible",
        "innioasis,y2\0mediatek,mt6582\0",
    );
    assert_eq!(states(&fixture), (json!("FAILED"), json!("FAILED")));
}
