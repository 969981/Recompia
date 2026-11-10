// SPDX-License-Identifier: GPL-3.0-or-later
// irs, nfc:user, nfc:mf:u and nfp:user: no IR camera, NFC switched off (see misc.cpp).
#include "misc_internal.h"

namespace nxrt::services::miscsvc {

// ---- irs --------------------------------------------------------------------------------------------------------------------------
// No IR camera is connected: sessions are accepted, processors accept and do nothing, and the camera handle is 0.
std::shared_ptr<Service> make_irs() {
    return make_iface("IIrSensorServer", [](Service& s) {
        s.add_cmd(302, "ActivateIrsensor", accept("irs", "ActivateIrsensor"));
        s.add_cmd(303, "DeactivateIrsensor", accept("irs", "DeactivateIrsensor"));
        s.add_cmd(304, "GetIrsensorSharedMemoryHandle", [](Request&, Response& rsp) -> uint32_t {
            services::ServiceSharedMemory sm(kIrsSharedMemorySize, "irs");
            if (!sm.ok()) return kNotSupported;
            rsp.out_copy.push_back(sm.new_handle());
            return 0;
        });
        s.add_cmd(305, "StopImageProcessor", accept("irs", "StopImageProcessor"));
        s.add_cmd(306, "RunMomentProcessor", accept("irs", "RunMomentProcessor"));
        s.add_cmd(307, "RunClusteringProcessor", accept("irs", "RunClusteringProcessor"));
        s.add_cmd(308, "RunImageTransferProcessor", accept("irs", "RunImageTransferProcessor"));
        s.add_cmd(309, "GetImageTransferProcessorState", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        s.add_cmd(310, "RunTeraPluginProcessor", accept("irs", "RunTeraPluginProcessor"));
        s.add_cmd(311, "GetNpadIrCameraHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);   // no camera: handle 0
            return 0;
        });
        s.add_cmd(314, "CheckFirmwareVersion", accept("irs", "CheckFirmwareVersion"));
        s.add_cmd(318, "StopImageProcessorAsync", accept("irs", "StopImageProcessorAsync"));
        s.add_cmd(319, "ActivateIrsensorWithFunctionLevel", accept("irs", "ActivateIrsensorWithFunctionLevel"));
    });
}

namespace {

// ---- nfc:user, nfc:mf:u, nfp:user -------------------------------------------------------------------------------------------------
// NFC is switched off: the state is NonInitialized, nothing is enabled, and there are no devices. Commands that need a device answer
// the no-device result.
Handler nfc_no_device(const char* tag, const char* what) { return refuse(kNfcNoDevice, tag, what); }
Handler nfp_no_device(const char* tag, const char* what) { return refuse(kNfpNoDevice, tag, what); }

}  // namespace

std::shared_ptr<Service> make_nfc_user() {
    return make_iface("nn::nfc::detail::IUser", [](Service& s) {
        s.add_cmd(0, "InitializeOld", accept("nfc", "InitializeOld"));
        s.add_cmd(1, "FinalizeOld", accept("nfc", "FinalizeOld"));
        s.add_cmd(2, "GetStateOld", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);   // NonInitialized
            return 0;
        });
        s.add_cmd(3, "IsNfcEnabledOld", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(400, "Initialize", accept("nfc", "Initialize"));
        s.add_cmd(401, "Finalize", accept("nfc", "Finalize"));
        s.add_cmd(402, "GetState", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        s.add_cmd(403, "IsNfcEnabled", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(404, "ListDevices", [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(405, "GetDeviceState", nfc_no_device("nfc", "GetDeviceState"));
        s.add_cmd(406, "GetNpadId", nfc_no_device("nfc", "GetNpadId"));
        s.add_cmd(407, "GetAvailabilityChangeEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfc_available.new_handle());
            return 0;
        });
        s.add_cmd(408, "StartDetection", nfc_no_device("nfc", "StartDetection"));
        s.add_cmd(409, "StopDetection", nfc_no_device("nfc", "StopDetection"));
        s.add_cmd(410, "GetTagInfo", nfc_no_device("nfc", "GetTagInfo"));
        s.add_cmd(411, "GetActivateEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfc_activate.new_handle());
            return 0;
        });
        s.add_cmd(412, "GetDeactivateEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfc_deactivate.new_handle());
            return 0;
        });
        s.add_cmd(1000, "ReadMifare", nfc_no_device("nfc", "ReadMifare"));
        s.add_cmd(1001, "WriteMifare", nfc_no_device("nfc", "WriteMifare"));
        s.add_cmd(1300, "SendCommandByPassThrough", nfc_no_device("nfc", "SendCommandByPassThrough"));
        s.add_cmd(1301, "KeepPassThroughSession", nfc_no_device("nfc", "KeepPassThroughSession"));
        s.add_cmd(1302, "ReleasePassThroughSession", nfc_no_device("nfc", "ReleasePassThroughSession"));
    });
}

std::shared_ptr<Service> make_nfp_user() {
    return make_iface("nn::nfp::detail::IUser", [](Service& s) {
        // The object's State (Ryujinx IUser: NonInitialized 0, Initialized 1): GetState reports it, so a caller that checks it
        // before nn::nfp::Initialize does not initialise twice (the SDK aborts on a second Initialize).
        auto state = std::make_shared<std::atomic<uint32_t>>(0);
        s.add_cmd(0, "Initialize", [state](Request&, Response&) -> uint32_t {
            state->store(1);
            NXLOG_D("nfp", "Initialize: state Initialized");
            return 0;
        });
        s.add_cmd(1, "Finalize", [state](Request&, Response&) -> uint32_t {
            state->store(0);
            NXLOG_D("nfp", "Finalize: state NonInitialized");
            return 0;
        });
        s.add_cmd(2, "ListDevices", [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(3, "StartDetection", nfp_no_device("nfp", "StartDetection"));
        s.add_cmd(4, "StopDetection", nfp_no_device("nfp", "StopDetection"));
        s.add_cmd(5, "Mount", nfp_no_device("nfp", "Mount"));
        s.add_cmd(6, "Unmount", nfp_no_device("nfp", "Unmount"));
        s.add_cmd(7, "OpenApplicationArea", nfp_no_device("nfp", "OpenApplicationArea"));
        s.add_cmd(8, "GetApplicationArea", nfp_no_device("nfp", "GetApplicationArea"));
        s.add_cmd(9, "SetApplicationArea", nfp_no_device("nfp", "SetApplicationArea"));
        s.add_cmd(10, "Flush", nfp_no_device("nfp", "Flush"));
        s.add_cmd(11, "Restore", nfp_no_device("nfp", "Restore"));
        s.add_cmd(12, "CreateApplicationArea", nfp_no_device("nfp", "CreateApplicationArea"));
        s.add_cmd(13, "GetTagInfo", nfp_no_device("nfp", "GetTagInfo"));
        s.add_cmd(14, "GetRegisterInfo", nfp_no_device("nfp", "GetRegisterInfo"));
        s.add_cmd(15, "GetCommonInfo", nfp_no_device("nfp", "GetCommonInfo"));
        s.add_cmd(16, "GetModelInfo", nfp_no_device("nfp", "GetModelInfo"));
        s.add_cmd(17, "AttachActivateEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfp_activate.new_handle());
            return 0;
        });
        s.add_cmd(18, "AttachDeactivateEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfp_deactivate.new_handle());
            return 0;
        });
        s.add_cmd(19, "GetState", [state](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(state->load());
            return 0;
        });
        s.add_cmd(20, "GetDeviceState", nfp_no_device("nfp", "GetDeviceState"));
        s.add_cmd(21, "GetNpadId", nfp_no_device("nfp", "GetNpadId"));
        s.add_cmd(22, "GetApplicationAreaSize", nfp_no_device("nfp", "GetApplicationAreaSize"));
        s.add_cmd(23, "GetAvailabilityChangeEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfp_available.new_handle());
            return 0;
        });
        s.add_cmd(24, "RecreateApplicationArea", nfp_no_device("nfp", "RecreateApplicationArea"));
        s.add_cmd(25, "StartDetectionWithFilter", nfp_no_device("nfp", "StartDetectionWithFilter"));
    });
}

// IUserManager of nfc:user, nfc:mf:u and nfp:user: CreateUserInterface returns the user object for that service. The nfc:mf:u
// IUser has no command table here (its ids are not in the sources read), so its commands answer kStubService.
std::shared_ptr<Service> make_user_manager(const char* name, std::shared_ptr<Service> (*make_user)()) {
    return make_iface(name, [make_user](Service& s) {
        s.add_cmd(0, "CreateUserInterface", [make_user](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_user());
            return 0;
        });
    });
}
std::shared_ptr<Service> make_mifare_user() { return make_iface("nn::nfc::mifare::detail::IUser", [](Service&) {}); }

}  // namespace nxrt::services::miscsvc
