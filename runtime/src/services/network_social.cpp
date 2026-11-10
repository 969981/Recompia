// SPDX-License-Identifier: GPL-3.0-or-later
// friend, prepo, pctl and bcat: the account-free offline answers (no friends, no restrictions, no BCAT data). See network.cpp.
#include "network_internal.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "../net/vnet.h"
#include "../net/vsock.h"
#include "coop.h"
#include "ldn.h"
#include "services_internal.h"

namespace nxrt::services::netsvc {

namespace {

// =============================================================================================================================================
// friend: friend:u, friend:v, friend:m, friend:s, friend:a (IServiceCreator). Switchbrew "Friend services". No friends, no network: the
// list commands answer count 0, the network-sending commands are accepted and do nothing (see the report).
// =============================================================================================================================================

struct FriendEvents {
    ServiceEvent completion{true};   // nothing is pending, so the completion is at once
};

std::shared_ptr<ipc::Service> make_friend_service() {
    auto ev = std::make_shared<FriendEvents>();
    return make_iface("nn::friends::detail::ipc::IFriendService", [ev](ipc::Service& s) {
        cmd(s, "friend", 0, "GetCompletionEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->completion.new_handle());
            return 0;
        });
        rows(s, "friend", {
            {1, "Cancel", K::Ok},
            {10100, "GetFriendListIds", K::S32, 0},
            {10101, "GetFriendList", K::S32, 0},
            {10102, "UpdateFriendInfo", K::Ok},
            {10110, "GetFriendProfileImage", K::Ok},
            {10111, "GetFriendProfileImageWithImageSize", K::Ok},
            {10120, "CheckFriendListAvailability", K::U8, 1},
            {10121, "EnsureFriendListAvailable", K::Ok},
            {10200, "SendFriendRequestForApplication", K::Ok},
            {10211, "AddFacedFriendRequestForApplication", K::Ok},
            {10400, "GetBlockedUserListIds", K::S32, 0},
            {10420, "CheckBlockedUserListAvailability", K::U8, 1},
            {10421, "EnsureBlockedUserListAvailable", K::Ok},
            {10500, "GetProfileListLegacyV1", K::S32, 0},
            {10501, "GetProfileListLegacyV2", K::S32, 0},
            {10502, "GetProfileListV1", K::S32, 0},
            {10503, "GetProfileListV2", K::S32, 0},
            {10600, "DeclareOpenOnlinePlaySession", K::Ok},
            {10601, "DeclareCloseOnlinePlaySession", K::Ok},
            {10610, "UpdateUserPresence", K::Ok},
            {10700, "GetPlayHistoryRegistrationKey", K::Ok},
            {10701, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId", K::Ok},
            {10702, "AddPlayHistory", K::Ok},
            {11000, "GetProfileImageUrl", K::Ok},
            {11001, "GetProfileImageUrlV2", K::Ok},
            {20100, "GetFriendCount", K::S32, 0},
            {20101, "GetNewlyFriendCount", K::S32, 0},
            {20102, "GetFriendDetailedInfo", K::Ok},
            {20103, "SyncFriendList", K::Ok},
            {20104, "RequestSyncFriendList", K::Ok},
            {20105, "GetFriendListForViewerV1", K::S32, 0},
            {20106, "UpdateFriendInfoForViewerV1", K::Ok},
            {20107, "GetFriendDetailedInfoV2", K::Ok},
            {20108, "GetFriendListForViewerV2", K::S32, 0},
            {20109, "UpdateFriendInfoForViewerV2", K::Ok},
            {20110, "LoadFriendSettingV1", K::Ok},
            {20111, "LoadFriendSettingV2", K::Ok},
            {20200, "GetReceivedFriendRequestCount", K::S32, 0},
            {20201, "GetFriendRequestListV1", K::S32, 0},
            {20202, "GetFriendRequestListV2", K::S32, 0},
            {20203, "GetFriendRequestReceivedNotificationCount", K::S32, 0},
            {20300, "GetFriendCandidateList", K::S32, 0},
            {20301, "GetNintendoNetworkIdInfo", K::Ok},
            {20400, "GetBlockedUserListV1", K::S32, 0},
            {20401, "SyncBlockedUserList", K::Ok},
            {20402, "GetBlockedUserListV2", K::S32, 0},
            {20500, "GetProfileExtraListV1", K::S32, 0},
            {20501, "GetRelationship", K::Ok},
            {20502, "GetProfileExtraListV2", K::S32, 0},
            {20600, "GetUserPresenceViewV1", K::Ok},
            {20601, "GetUserPresenceViewV2", K::Ok},
            {20700, "GetPlayHistoryListV1", K::S32, 0},
            {20701, "GetPlayHistoryStatistics", K::Ok},
            {20702, "GetPlayHistoryListV2", K::S32, 0},
            {20800, "LoadUserSettingV1", K::Ok},
            {20801, "SyncUserSetting", K::Ok},
            {20802, "LoadUserSettingV2", K::Ok},
            {20900, "RequestListSummaryOverlayNotification", K::Ok},
            {21000, "GetExternalApplicationCatalog", K::Ok},
            {22000, "GetReceivedFriendInvitationListV1", K::S32, 0},
            {22001, "GetReceivedFriendInvitationDetailedInfoV1", K::Ok},
            {22002, "GetReceivedFriendInvitationListV2", K::S32, 0},
            {22003, "GetReceivedFriendInvitationDetailedInfoV2", K::Ok},
            {22010, "GetReceivedFriendInvitationCountCache", K::S32, 0},
            {30100, "DropFriendNewlyFlags", K::Ok},
            {30101, "DeleteFriend", K::Ok},
            {30110, "DropFriendNewlyFlag", K::Ok},
            {30120, "ChangeFriendFavoriteFlag", K::Ok},
            {30121, "ChangeFriendOnlineNotificationFlag", K::Ok},
            {30130, "SetFriendNote", K::Ok},
            {30131, "RequestUploadPendingNote", K::Ok},
            {30190, "RequestSyncLocalUpdates", K::Ok},
            {30200, "SendFriendRequest", K::Ok},
            {30201, "SendFriendRequestWithApplicationInfoV1", K::Ok},
            {30202, "CancelFriendRequest", K::Ok},
            {30203, "AcceptFriendRequest", K::Ok},
            {30204, "RejectFriendRequest", K::Ok},
            {30205, "ReadFriendRequest", K::Ok},
            {30210, "GetFacedFriendRequestRegistrationKey", K::Ok},
            {30211, "AddFacedFriendRequest", K::Ok},
            {30212, "CancelFacedFriendRequest", K::Ok},
            {30213, "GetFacedFriendRequestProfileImage", K::Ok},
            {30214, "GetFacedFriendRequestProfileImageFromPath", K::Ok},
            {30215, "SendFriendRequestWithExternalApplicationCatalogId", K::Ok},
            {30216, "ResendFacedFriendRequest", K::Ok},
            {30217, "SendFriendRequestWithNintendoNetworkIdInfo", K::Ok},
            {30218, "SendFriendRequestWithApplicationInfoV2", K::Ok},
            {30300, "GetSnsAccountLinkPageUrl", K::Ok},
            {30301, "UnlinkSnsAccount", K::Ok},
            {30400, "BlockUser", K::Ok},
            {30401, "BlockUserWithApplicationInfoV1", K::Ok},
            {30402, "UnblockUser", K::Ok},
            {30403, "BlockUserWithApplicationInfoV2", K::Ok},
            {30500, "GetProfileExtraFromFriendCodeV1", K::Ok},
            {30501, "GetProfileExtraFromFriendCodeV2", K::Ok},
            {30700, "DeletePlayHistory", K::Ok},
            {30701, "AddPlayHistoryWithApplication", K::Ok},
            {30810, "ChangePresencePermission", K::Ok},
            {30811, "ChangeFriendRequestReception", K::Ok},
            {30812, "ChangePlayLogPermission", K::Ok},
            {30820, "IssueFriendCode", K::Ok},
            {30830, "ClearPlayLog", K::Ok},
            {30900, "SendFriendInvitationV1", K::Ok},
            {30901, "SendFriendInvitationV2", K::Ok},
            {30910, "ReadFriendInvitation", K::Ok},
            {30911, "ReadAllFriendInvitations", K::Ok},
            {31000, "OpenUser", K::Ok},
            {31001, "OpenUserWithPlayLogPolicy", K::Ok},
            {40100, "DeleteFriendListCache", K::Ok},
            {40400, "DeleteBlockedUserListCache", K::Ok},
            {49900, "DeleteNetworkServiceAccountCache", K::Ok},
        });
    });
}

std::shared_ptr<ipc::Service> make_friend_notification() {
    auto ev = std::make_shared<ServiceEvent>();   // never signalled: no notifications
    return make_iface("nn::friends::detail::ipc::INotificationService", [ev](ipc::Service& s) {
        cmd(s, "friend", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        rows(s, "friend", {
            {1, "Clear", K::Ok},
            {2, "Pop", K::Zeros, 0, 16},          // SizedNotificationInfo, all zero: nothing is queued
        });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_friend_creator() {
    return make_iface("nn::friends::detail::ipc::IServiceCreator", [](ipc::Service& s) {
        cmd(s, "friend", 0, "CreateFriendService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_friend_service());
            return 0;
        });
        cmd(s, "friend", 1, "CreateNotificationService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_friend_notification());
            return 0;
        });
        cmd(s, "friend", 2, "CreateDaemonSuspendSessionService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("nn::friends::detail::ipc::IDaemonSuspendSessionService", [](ipc::Service&) {}));
            return 0;
        });
    });
}

namespace {

// =============================================================================================================================================
// prepo: prepo:u, prepo:a, prepo:a2 (IPrepoService). Play reports are accepted and dropped. ScarletNX misc.swipc lists 10105, 10200, 10400.
// =============================================================================================================================================

}  // namespace

std::shared_ptr<ipc::Service> make_prepo() {
    return make_iface("nn::prepo::detail::ipc::IPrepoService", [](ipc::Service& s) {
        rows(s, "prepo", {
            {10105, "SaveReportWithUser", K::Ok},
            {10200, "RequestImmediateTransmission", K::Ok},
            {10400, "GetSystemSessionId", K::U64, 0},
        });
    });
}

namespace {

// =============================================================================================================================================
// pctl: pctl, pctl:s, pctl:r, pctl:a (IParentalControlServiceFactory). No restriction is set: everything answers "allowed".
// =============================================================================================================================================

struct PctlEvents {
    ServiceEvent pin_changed;
    ServiceEvent sync;
    ServiceEvent play_timer;
    ServiceEvent unlinked;
};

std::shared_ptr<ipc::Service> make_pctl_service() {
    auto ev = std::make_shared<PctlEvents>();
    return make_iface("nn::pctl::detail::ipc::IParentalControlService", [ev](ipc::Service& s) {
        rows(s, "pctl", {
            {1, "Initialize", K::Ok},
            {1001, "CheckFreeCommunicationPermission", K::Ok},
            {1002, "ConfirmLaunchApplicationPermission", K::Ok},
            {1003, "ConfirmResumeApplicationPermission", K::Ok},
            {1004, "ConfirmSnsPostPermission", K::Ok},
            {1005, "ConfirmSystemSettingsPermission", K::Ok},
            {1006, "IsRestrictionTemporaryUnlocked", K::U8, 0},
            {1007, "RevertRestrictionTemporaryUnlocked", K::Ok},
            {1008, "EnterRestrictedSystemSettings", K::Ok},
            {1009, "LeaveRestrictedSystemSettings", K::Ok},
            {1010, "IsRestrictedSystemSettingsEntered", K::U8, 0},
            {1011, "RevertRestrictedSystemSettingsEntered", K::Ok},
            {1012, "GetRestrictedFeatures", K::U32, 0},
            {1013, "ConfirmStereoVisionPermission", K::Ok},
            {1014, "ConfirmPlayableApplicationVideoOld", K::Ok},
            {1015, "ConfirmPlayableApplicationVideo", K::Ok},
            {1016, "ConfirmShowNewsPermission", K::Ok},
            {1017, "EndFreeCommunication", K::Ok},
            {1018, "IsFreeCommunicationAvailable", K::U8, 1},
            {1019, "ConfirmLaunchApplicationPermission", K::Ok},
            {1020, "ConfirmLaunchSharedApplicationPermission", K::Ok},
            {1021, "TryBeginFreeCommunicationForStreamPlay", K::Ok},
            {1022, "EndFreeCommunicationForStreamPlay", K::Ok},
            {1023, "GetRestrictedFeatureFlagSet", K::U32, 0},
            {1031, "IsRestrictionEnabled", K::U8, 0},
            {1032, "GetSafetyLevel", K::S32, 0},
            {1033, "SetSafetyLevel", K::Ok},
            {1034, "GetSafetyLevelSettings", K::Ok},
            {1035, "GetCurrentSettings", K::Ok},
            {1036, "SetCustomSafetyLevelSettings", K::Ok},
            {1037, "GetDefaultRatingOrganization", K::U32, 0},
            {1038, "SetDefaultRatingOrganization", K::Ok},
            {1039, "GetFreeCommunicationApplicationListCount", K::S32, 0},
            {1042, "AddToFreeCommunicationApplicationList", K::Ok},
            {1043, "DeleteSettings", K::Ok},
            {1044, "GetFreeCommunicationApplicationList", K::S32, 0},
            {1045, "UpdateFreeCommunicationApplicationList", K::Ok},
            {1046, "DisableFeaturesForReset", K::Ok},
            {1047, "NotifyApplicationDownloadStartedOld", K::Ok},
            {1048, "NotifyNetworkProfileCreated", K::Ok},
            {1049, "ResetFreeCommunicationApplicationList", K::Ok},
            {1050, "AddToFreeCommunicationApplicationList", K::Ok},
            {1051, "NotifyApplicationDownloadStarted", K::Ok},
            {1061, "ConfirmStereoVisionRestrictionConfigurable", K::Ok},
            {1062, "GetStereoVisionRestriction", K::U8, 0},
            {1063, "SetStereoVisionRestriction", K::Ok},
            {1064, "ResetConfirmedStereoVisionPermission", K::Ok},
            {1065, "IsStereoVisionPermitted", K::U8, 1},
            {1201, "UnlockRestrictionTemporarily", K::Ok},
            {1202, "UnlockSystemSettingsRestriction", K::Ok},
            {1203, "SetPinCode", K::Ok},
            {1204, "GenerateInquiryCode", K::Ok},
            {1205, "CheckMasterKey", K::Ok},
            {1206, "GetPinCodeLength", K::Ok},
            {1403, "IsPairingActive", K::U8, 0},
            {1453, "IsPlayTimerEnabled", K::U8, 0},
        });
        cmd(s, "pctl", 1207, "GetPinCodeChangedEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->pin_changed.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1432, "GetSynchronizationEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->sync.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1457, "GetPlayTimerEventToRequestSuspension", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->play_timer.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1473, "GetUnlinkedEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->unlinked.new_handle());
            return 0;
        });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_pctl_factory() {
    return make_iface("nn::pctl::detail::ipc::IParentalControlServiceFactory", [](ipc::Service& s) {
        auto create = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_pctl_service());
            return 0;
        };
        cmd(s, "pctl", 0, "CreateService", create);
        cmd(s, "pctl", 1, "CreateServiceWithoutInitialize", create);
    });
}

namespace {

// =============================================================================================================================================
// bcat: bcat:u, bcat:a (IServiceCreator). No BCAT data for this title and no network: the sync requests finish at once with an empty cache.
// =============================================================================================================================================

struct ProgressEvents {
    ServiceEvent done{true};   // the sync is over
};

std::shared_ptr<ipc::Service> make_bcat_progress() {
    auto ev = std::make_shared<ProgressEvents>();
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheProgressService", [ev](ipc::Service& s) {
        cmd(s, "bcat", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->done.new_handle());
            return 0;
        });
        rows(s, "bcat", {
            {1, "GetImpl", K::BufZero, 0, 0x200},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_notifier() {
    auto ev = std::make_shared<ServiceEvent>();   // the storage is never updated
    return make_iface("nn::bcat::detail::ipc::INotifierService", [ev](ipc::Service& s) {
        cmd(s, "bcat", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_file() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheFileService", [](ipc::Service& s) {
        rows(s, "bcat", {
            {0, "Open", K::Fail, kFsPathNotFound},
            {1, "Read", K::U64, 0},
            {2, "GetSize", K::U64, 0},
            {3, "GetDigest", K::Zeros, 0, 0x10},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_directory() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheDirectoryService", [](ipc::Service& s) {
        rows(s, "bcat", {
            {0, "Open", K::Fail, kFsPathNotFound},
            {1, "Read", K::S32, 0},
            {2, "GetCount", K::S32, 0},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_storage() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheStorageService", [](ipc::Service& s) {
        cmd(s, "bcat", 0, "CreateFileService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_file());
            return 0;
        });
        cmd(s, "bcat", 1, "CreateDirectoryService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_directory());
            return 0;
        });
        rows(s, "bcat", {
            {10, "EnumerateDeliveryCacheDirectory", K::S32, 0},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_service() {
    auto progress = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_bcat_progress());
        return 0;
    };
    return make_iface("nn::bcat::detail::ipc::IBcatService", [progress](ipc::Service& s) {
        cmd(s, "bcat", 10100, "RequestSyncDeliveryCache", progress);
        cmd(s, "bcat", 10101, "RequestSyncDeliveryCacheWithDirectoryName", progress);
        cmd(s, "bcat", 20100, "RequestSyncDeliveryCacheWithApplicationId", progress);
        cmd(s, "bcat", 20101, "RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName", progress);
        cmd(s, "bcat", 20300, "GetDeliveryCacheStorageUpdateNotifier", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_notifier());
            return 0;
        });
        cmd(s, "bcat", 20301, "RequestSuspendDeliveryTask", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("nn::bcat::detail::ipc::IDeliveryTaskSuspensionService", [](ipc::Service&) {}));
            return 0;
        });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_bcat_creator() {
    return make_iface("nn::bcat::detail::ipc::IServiceCreator", [](ipc::Service& s) {
        auto bcat = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_service());
            return 0;
        };
        auto storage = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_storage());
            return 0;
        };
        cmd(s, "bcat", 0, "CreateBcatService", bcat);
        cmd(s, "bcat", 1, "CreateDeliveryCacheStorageService", storage);
        cmd(s, "bcat", 2, "CreateDeliveryCacheStorageServiceWithApplicationId", storage);
    });
}

}  // namespace nxrt::services::netsvc
