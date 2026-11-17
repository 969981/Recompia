// SPDX-License-Identifier: GPL-3.0-or-later
//! Whose mod switches and settings a Mods page call reads or changes: the global defaults, a singleplayer profile, or a co-op group.
//!
//! The Modtopia core keeps a layer of overrides per save account (`<game>/config/accounts/<account key>/`) over the global defaults, and
//! the game picks the layer of the account it plays (runtime be_save_identity -> mt_loader_set_config_account). So a scope is an account
//! key: a profile's [account] uid, or a co-op group's shared `account_uid`, which a co-op start hands the game as NXRT_ACCOUNT_UID
//! (coop_launch.rs). Every member's copy of a group has the same uid, so the group's layer has the same name on every PC.

use std::path::Path;

use crate::saveimport::account_key_of;

#[derive(Clone, Debug, PartialEq)]
pub enum Scope {
    /// the global defaults (Recompia's own list: what every account inherits)
    Global,
    /// a singleplayer profile of profiles.json, by id
    Profile(String),
    /// a co-op group (coop.rs), by id
    Coop(String),
}

impl Scope {
    /// The scope a command names: `profile` or `coop` (a group id), or neither for the global defaults.
    pub fn of(profile: Option<String>, coop: Option<String>) -> Result<Scope, String> {
        match (profile, coop) {
            (None, None) => Ok(Scope::Global),
            (Some(p), None) => Ok(Scope::Profile(p)),
            (None, Some(g)) => Ok(Scope::Coop(g)),
            (Some(_), Some(_)) => Err("Choose a profile or a co-op group, not both.".into()),
        }
    }

    pub fn is_global(&self) -> bool {
        *self == Scope::Global
    }

    /// The account key of the scope's layer; None for the global defaults.
    pub fn account(&self, game_dir: &Path) -> Result<Option<String>, String> {
        match self {
            Scope::Global => Ok(None),
            Scope::Profile(id) => {
                let profiles = crate::profiles::load(game_dir);
                let p = profiles.get(id).ok_or("That profile does not exist.")?;
                Ok(Some(account_key_of(&p.uid)))
            }
            Scope::Coop(id) => {
                let g = crate::coop::get(&crate::coop::root_of(game_dir), id)?;
                Ok(Some(account_key_of(&g.account_uid)))
            }
        }
    }

    /// Every layer of this install: each singleplayer profile and each co-op group (a lock put back clears them all, mods_lock.rs).
    pub fn every_layer(game_dir: &Path) -> Vec<Scope> {
        let profiles = crate::profiles::load(game_dir).profiles.into_iter().map(|p| Scope::Profile(p.id));
        let groups = crate::coop::list(&crate::coop::root_of(game_dir)).into_iter().map(|g| Scope::Coop(g.id));
        profiles.chain(groups).collect()
    }

    pub fn profile_id(&self) -> Option<String> {
        if let Scope::Profile(id) = self { Some(id.clone()) } else { None }
    }

    pub fn coop_id(&self) -> Option<String> {
        if let Scope::Coop(id) = self { Some(id.clone()) } else { None }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_command_names_one_scope() {
        assert_eq!(Scope::of(None, None).unwrap(), Scope::Global);
        assert_eq!(Scope::of(Some("p".into()), None).unwrap(), Scope::Profile("p".into()));
        assert_eq!(Scope::of(None, Some("g".into())).unwrap(), Scope::Coop("g".into()));
        assert!(Scope::of(Some("p".into()), Some("g".into())).is_err());
    }

    #[test]
    fn a_co_op_group_is_the_layer_of_its_shared_account() {
        let dir = crate::mods::testkit::temp_game_dir("scope-coop");
        let root = crate::coop::root_of(&dir);
        let g = crate::coop::create(&root, "Island", "Ash", "192.0.2.10").unwrap();
        let profile = crate::profiles::load(&dir).active_profile().id.clone();
        let group_key = Scope::Coop(g.id.clone()).account(&dir).unwrap().unwrap();
        assert_eq!(group_key, account_key_of(&g.account_uid), "the key the game gets through NXRT_ACCOUNT_UID");
        assert_ne!(Some(group_key), Scope::Profile(profile.clone()).account(&dir).unwrap(), "apart from the singleplayer layer");
        assert_eq!(Scope::Global.account(&dir).unwrap(), None);
        assert!(Scope::Coop("ffff".into()).account(&dir).is_err());
        assert!(Scope::Coop("../x".into()).account(&dir).is_err());
        assert_eq!(Scope::every_layer(&dir), vec![Scope::Profile(profile), Scope::Coop(g.id)]);
        let _ = std::fs::remove_dir_all(dir);
    }
}
