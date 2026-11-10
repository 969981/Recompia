// SPDX-License-Identifier: GPL-3.0-or-later
// olsc:u (online save data backup, off) and ectx:aw (error context writer) (see misc.cpp).
#include "misc_internal.h"

namespace nxrt::services::miscsvc {

// ---- olsc:u -----------------------------------------------------------------------------------------------------------------------
// IOlscServiceForApplication (10.0.0+), online save data backup, which is off: Ryubing Olsc/IOlscServiceForApplication.cs (MIT)
// for the commands, results and the setting values (1 enabled, 2 not; Ryubing marks the values as not determined).
std::shared_ptr<Service> make_olsc() {
    struct Olsc {
        std::mutex mu;
        bool initialized = false;
        std::map<std::pair<uint64_t, uint64_t>, bool> backup;
    };
    auto o = std::make_shared<Olsc>();
    return make_iface("olsc:u", [o](Service& s) {
        s.add_cmd(0, "Initialize", [o](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> g(o->mu);
            o->initialized = true;
            NXLOG_D("olsc", "Initialize: online save data backup is not available");
            return 0;
        });
        s.add_cmd(13, "GetSaveDataBackupSetting", [o](Request& req, Response& rsp) -> uint32_t {
            const auto uid = std::make_pair(req.arg<uint64_t>(0), req.arg<uint64_t>(8));
            std::lock_guard<std::mutex> g(o->mu);
            if (!o->initialized) return kOlscNotInitialized;
            if (uid.first == 0 && uid.second == 0) return kOlscNullArgument;
            const auto it = o->backup.find(uid);
            rsp.push<uint8_t>(it != o->backup.end() && it->second ? 1 : 2);
            return 0;
        });
        s.add_cmd(14, "SetSaveDataBackupSettingEnabled", [o](Request& req, Response&) -> uint32_t {
            const bool enabled = req.arg<uint64_t>(0) != 0;
            const auto uid = std::make_pair(req.arg<uint64_t>(8), req.arg<uint64_t>(16));
            std::lock_guard<std::mutex> g(o->mu);
            if (!o->initialized) return kOlscNotInitialized;
            if (uid.first == 0 && uid.second == 0) return kOlscNullArgument;
            o->backup[uid] = enabled;
            return 0;
        });
    });
}

namespace {

// ---- ectx:aw ----------------------------------------------------------------------------------------------------------------------
// Error-context writer: the registrar takes the context and drops it.
std::shared_ptr<Service> make_ectx_registrar() {
    return make_iface("nn::err::context::IContextRegistrar", [](Service& s) {
        s.add_cmd(0, "Complete", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(0);   // context_descriptor
            return 0;
        });
    });
}

}  // namespace

std::shared_ptr<Service> make_ectx() {
    return make_iface("nn::err::context::IWriterForApplication", [](Service& s) {
        s.add_cmd(0, "CreateContextRegistrar", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_ectx_registrar());
            return 0;
        });
    });
}

}  // namespace nxrt::services::miscsvc
