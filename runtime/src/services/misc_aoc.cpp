// SPDX-License-Identifier: GPL-3.0-or-later
// aoc:u: add-on content (see misc.cpp).
#include "misc_internal.h"

namespace nxrt::services::miscsvc {

namespace {

// ---- aoc:u ------------------------------------------------------------------------------------------------------------------------
// The add-on content (DLC) of [fs] aoc_dir is installed (add_on_contents(), services.h); without it, none. The base id of the title's
// add-on content is its program id + 0x1000, an add-on's index its id minus that. fsp-srv OpenDataStorageByDataId serves their RomFS.
// List*: ListAddOnContent (u32 offset, u32 count, pid placeholder), ListAddOnContentByApplicationId (u64 application id, u32
// offset, u32 count; [1.0.0-6.2.0]) -> u32 written, the indices in a u32 array (Switchbrew aoc:u, Ryujinx IAddOnContentManager).
// `at`: the byte offset of the u32 offset argument (0 or 8).
uint32_t aoc_list(Request& req, Response& rsp, size_t at, bool this_title, const char* what) {
    const ipc::Buffer b = req.out_buf(0);
    zero_buffer(b);
    const auto& list = add_on_contents();
    const uint32_t offset = req.arg<uint32_t>(at), count = req.arg<uint32_t>(at + 4);
    const uint64_t room = b.addr ? b.size / sizeof(uint32_t) : 0;
    const uint64_t want = std::min<uint64_t>(count, room);
    uint32_t n = 0;
    for (size_t i = offset; this_title && i < list.size() && n < want; ++i, ++n)
        std::memcpy(buf_mut(b) + n * sizeof(uint32_t), &list[i].index, sizeof(uint32_t));
    rsp.push<uint32_t>(n);
    NXLOG_I("aoc", "%s(offset %u, count %u): %u of %zu add-ons", what, offset, count, n, this_title ? list.size() : size_t{0});
    return 0;
}

std::shared_ptr<Service> make_purchase_manager() {
    return make_iface("IPurchaseEventManager", [](Service& s) {
        s.add_cmd(2, "GetPurchasedEventReadableHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().aoc_purchased.new_handle());
            return 0;
        });
        s.add_cmd(3, "PopPurchasedProductInfo", refuse(kAocNoPurchase, "aoc", "PopPurchasedProductInfo"));
        s.add_cmd(4, "PopPurchasedProductInfoWithUid", refuse(kAocNoPurchase, "aoc", "PopPurchasedProductInfoWithUid"));
    });
}
std::shared_ptr<Service> make_contents_manager() {
    return make_iface("IContentsServiceManager", [](Service& s) {
        // The authorization token comes from the network, which this console does not have.
        s.add_cmd(0, "RequestContentsAuthorizationToken", refuse(kAocNotConnected, "aoc", "RequestContentsAuthorizationToken"));
    });
}

}  // namespace

std::shared_ptr<Service> make_aoc() {
    return make_iface("IAddOnContentManager", [](Service& s) {
        // Count and list answer the add-ons of [fs] aoc_dir (the ByApplicationId forms only for this title).
        s.add_cmd(0, "CountAddOnContentByApplicationId", [](Request& req, Response& rsp) -> uint32_t {
            const bool ours = req.arg<uint64_t>(0) == profile().program_id;
            const auto n = static_cast<int32_t>(ours ? add_on_contents().size() : 0);
            rsp.push<int32_t>(n);
            NXLOG_I("aoc", "CountAddOnContentByApplicationId(0x%016llX): %d", static_cast<unsigned long long>(req.arg<uint64_t>(0)), n);
            return 0;
        });
        s.add_cmd(1, "ListAddOnContentByApplicationId", [](Request& req, Response& rsp) -> uint32_t {
            return aoc_list(req, rsp, 8, req.arg<uint64_t>(0) == profile().program_id, "ListAddOnContentByApplicationId");
        });
        s.add_cmd(2, "CountAddOnContent", [](Request&, Response& rsp) -> uint32_t {
            const auto n = static_cast<int32_t>(add_on_contents().size());
            rsp.push<int32_t>(n);
            NXLOG_I("aoc", "CountAddOnContent: %d", n);
            return 0;
        });
        s.add_cmd(3, "ListAddOnContent", [](Request& req, Response& rsp) -> uint32_t {
            return aoc_list(req, rsp, 0, true, "ListAddOnContent");
        });
        s.add_cmd(4, "GetAddOnContentBaseIdByApplicationId", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(req.arg<uint64_t>(0) + kAocBaseIdOffset);
            return 0;
        });
        s.add_cmd(5, "GetAddOnContentBaseId", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(profile().program_id + kAocBaseIdOffset);
            NXLOG_I("aoc", "GetAddOnContentBaseId: 0x%016llX", static_cast<unsigned long long>(profile().program_id + kAocBaseIdOffset));
            return 0;
        });
        s.add_cmd(6, "PrepareAddOnContentByApplicationId", accept("aoc", "PrepareAddOnContentByApplicationId"));
        s.add_cmd(7, "PrepareAddOnContent", accept("aoc", "PrepareAddOnContent"));
        const Handler list_event = [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().aoc_list_changed.new_handle());
            return 0;
        };
        s.add_cmd(8, "GetAddOnContentListChangedEvent", list_event);
        s.add_cmd(9, "GetAddOnContentLostErrorCode", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0);   // no add-on content was lost
            return 0;
        });
        s.add_cmd(10, "GetAddOnContentListChangedEventWithProcessId", list_event);
        s.add_cmd(11, "NotifyMountAddOnContent", accept("aoc", "NotifyMountAddOnContent"));
        s.add_cmd(12, "NotifyUnmountAddOnContent", accept("aoc", "NotifyUnmountAddOnContent"));
        s.add_cmd(13, "IsAddOnContentMountedForDebug", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(50, "CheckAddOnContentMountStatus", accept("aoc", "CheckAddOnContentMountStatus"));
        const Handler purchase = [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_purchase_manager());
            return 0;
        };
        s.add_cmd(100, "CreateEcPurchasedEventManager", purchase);
        s.add_cmd(101, "CreatePermanentEcPurchasedEventManager", purchase);
        s.add_cmd(110, "CreateContentsServiceManager", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_contents_manager());
            return 0;
        });
        s.add_cmd(200, "SetRequiredAddOnContentsOnContentsAvailabilityTransition",
                  accept("aoc", "SetRequiredAddOnContentsOnContentsAvailabilityTransition"));
    });
}

}  // namespace nxrt::services::miscsvc
