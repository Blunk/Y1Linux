//! Which player the shared root runs on. Y2ROOT starts on the Innioasis Y2 and on the Innioasis Y1
//! (docs/planning/Y1-RECOVERY-PLAN.md, R4b); the device tree's first compatible string tells them
//! apart. Anything else, an unreadable tree and a fixture root without one are the Y2.
// SPDX-License-Identifier: GPL-2.0-only
use crate::Root;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Player {
    Y2,
    Y1,
}

impl Player {
    pub fn of(root: &Root) -> Player {
        let compatible = root
            .read("/proc/device-tree/compatible")
            .unwrap_or_default();
        if compatible.split('\0').next() == Some("innioasis,y1") {
            Player::Y1
        } else {
            Player::Y2
        }
    }

    /// The native update core built for this player's eMMC (`core.c`, `-DY2_UPDATE_Y1` for the Y1).
    pub fn update_core(self) -> &'static str {
        match self {
            Player::Y2 => "/usr/sbin/y2-update-core",
            Player::Y1 => "/usr/sbin/y1-update-core",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_first_compatible_string_names_the_player() {
        let dir = std::env::temp_dir().join(format!("y2p-player-{}", std::process::id()));
        std::fs::create_dir_all(dir.join("proc/device-tree")).unwrap();
        let root = Root::new(&dir);
        assert_eq!(Player::of(&root), Player::Y2);
        for (compatible, player) in [
            ("innioasis,y1\0mediatek,mt6572\0", Player::Y1),
            ("innioasis,y2\0mediatek,mt6582\0", Player::Y2),
            ("mediatek,mt6572\0innioasis,y1\0", Player::Y2),
            ("innioasis,y1x\0", Player::Y2),
        ] {
            std::fs::write(dir.join("proc/device-tree/compatible"), compatible).unwrap();
            assert_eq!(Player::of(&root), player, "{compatible:?}");
        }
        assert_eq!(Player::Y1.update_core(), "/usr/sbin/y1-update-core");
        assert_eq!(Player::Y2.update_core(), "/usr/sbin/y2-update-core");
        std::fs::remove_dir_all(dir).unwrap();
    }
}
