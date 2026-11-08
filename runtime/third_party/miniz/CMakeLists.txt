# miniz 3.0.2 (MIT, https://github.com/richgel999/miniz, release amalgamation miniz.c/miniz.h, unchanged; THIRD_PARTY_NOTICES.md): zlib
# streams for the Pokopia save repair (games/pkp/pkp_save_uuid.cpp). Archive, stdio-free time and zip APIs are compiled out.
add_library(nxrt_miniz STATIC miniz.c)
target_include_directories(nxrt_miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nxrt_miniz PUBLIC MINIZ_NO_ARCHIVE_APIS MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_definitions(nxrt_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
