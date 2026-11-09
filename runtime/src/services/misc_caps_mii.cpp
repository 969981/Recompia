// SPDX-License-Identifier: GPL-3.0-or-later
// caps:u (album reading) and mii:u (see misc.cpp; caps:su is in album.cpp).
#include "misc_internal.h"

namespace nxrt::services::miscsvc {

namespace {

// ---- caps:u (caps:su is in album.cpp) --------------------------------------------------------------------------------------------
// Album reading: lists are empty (Pokopia main imports no album list or load function, only nn::album::SaveScreenshot).
Handler album_list() {
    return [](Request& req, Response& rsp) -> uint32_t {
        zero_buffer(req.out_buf(0));
        rsp.push<uint64_t>(0);
        return 0;
    };
}

}  // namespace

std::shared_ptr<Service> make_album_app() {
    return make_iface("IAlbumApplicationService", [](Service& s) {
        s.add_cmd(32, "SetShimLibraryVersion", accept("caps", "SetShimLibraryVersion"));
        s.add_cmd(102, "GetAlbumFileList0AafeAruidDeprecated", album_list());
        s.add_cmd(103, "DeleteAlbumFileByAruid", accept("caps", "DeleteAlbumFileByAruid"));
        s.add_cmd(104, "GetAlbumFileSizeByAruid", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0);
            return 0;
        });
        s.add_cmd(105, "DeleteAlbumFileByAruidForDebug", accept("caps", "DeleteAlbumFileByAruidForDebug"));
        s.add_cmd(110, "LoadAlbumScreenShotImageByAruid", accept("caps", "LoadAlbumScreenShotImageByAruid"));
        s.add_cmd(120, "LoadAlbumScreenShotThumbnailImageByAruid", accept("caps", "LoadAlbumScreenShotThumbnailImageByAruid"));
        s.add_cmd(130, "PrecheckToCreateContentsByAruid", accept("caps", "PrecheckToCreateContentsByAruid"));
        s.add_cmd(140, "GetAlbumFileList1AafeAruidDeprecated", album_list());
        s.add_cmd(141, "GetAlbumFileList2AafeUidAruidDeprecated", album_list());
        s.add_cmd(142, "GetAlbumFileList3AaeAruidDeprecated", album_list());
    });
}

namespace {

// ---- mii:u ------------------------------------------------------------------------------------------------------------------------
// The shared Mii database is empty. Count and get answer zero; the default and random builders return an all-zero CharInfo (the
// real default Mii data is not reproduced here).
std::shared_ptr<Service> make_mii_database() {
    return make_iface("IDatabaseService", [](Service& s) {
        s.add_cmd(0, "IsUpdated", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(1, "IsFullDatabase", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        const Handler count = [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(0);
            return 0;
        };
        s.add_cmd(2, "GetCount", count);
        const Handler get = [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        };
        s.add_cmd(3, "Get", get);
        s.add_cmd(4, "Get1", get);
        s.add_cmd(8, "Get2", get);
        s.add_cmd(9, "Get3", get);
        // UpdateLatest / UpdateLatest1: nothing is stored, so the Mii that was given comes back unchanged.
        const Handler latest = [](Request& req, Response& rsp) -> uint32_t {
            if (req.in_size >= kCharInfoSize) {
                rsp.out.insert(rsp.out.end(), req.in, req.in + kCharInfoSize);
            } else {
                rsp.out.resize(rsp.out.size() + kCharInfoSize, 0);
            }
            return 0;
        };
        s.add_cmd(5, "UpdateLatest", latest);
        s.add_cmd(10, "UpdateLatest1", latest);
        s.add_cmd(6, "BuildRandom", [](Request&, Response& rsp) -> uint32_t {
            rsp.out.resize(rsp.out.size() + kCharInfoSize, 0);
            return 0;
        });
        s.add_cmd(7, "BuildDefault", [](Request& req, Response& rsp) -> uint32_t {
            if (req.arg<uint32_t>(0) >= 6) return kMiiInvalidArgument;   // six defaults
            rsp.out.resize(rsp.out.size() + kCharInfoSize, 0);
            return 0;
        });
        s.add_cmd(11, "FindIndex", refuse(kMiiNotFound, "mii", "FindIndex"));
        s.add_cmd(21, "GetIndex", refuse(kMiiNotFound, "mii", "GetIndex"));
        s.add_cmd(12, "Move", accept("mii", "Move"));
        s.add_cmd(13, "AddOrReplace", accept("mii", "AddOrReplace"));
        s.add_cmd(14, "Delete", accept("mii", "Delete"));
        s.add_cmd(15, "DestroyFile", accept("mii", "DestroyFile"));
        s.add_cmd(16, "DeleteFile", accept("mii", "DeleteFile"));
        s.add_cmd(17, "Format", accept("mii", "Format"));
        s.add_cmd(18, "Import", accept("mii", "Import"));
        s.add_cmd(19, "Export", accept("mii", "Export"));
        s.add_cmd(20, "IsBrokenDatabaseWithClearFlag", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(22, "SetInterfaceVersion", accept("mii", "SetInterfaceVersion"));
        s.add_cmd(23, "Convert", accept("mii", "Convert"));
        s.add_cmd(24, "ConvertCoreDataToCharInfo", accept("mii", "ConvertCoreDataToCharInfo"));
        s.add_cmd(25, "ConvertCharInfoToCoreData", accept("mii", "ConvertCharInfoToCoreData"));
        s.add_cmd(26, "Append", accept("mii", "Append"));
    });
}

}  // namespace

std::shared_ptr<Service> make_mii_static() {
    return make_iface("IStaticService", [](Service& s) {
        s.add_cmd(0, "GetDatabaseService", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_mii_database());
            return 0;
        });
    });
}

}  // namespace nxrt::services::miscsvc
