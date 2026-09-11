# Defines zlib_static as a static library via FetchContent. Idempotent.
if(TARGET zlib_static)
    return()
endif()

include(FetchCache)
include(FetchContent)

FetchContent_Declare(
    zlib
    URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TLS_VERIFY OFF
)
FetchContent_MakeAvailable(zlib)

set(ZLIB_SRCS
    "${zlib_SOURCE_DIR}/adler32.c"
    "${zlib_SOURCE_DIR}/crc32.c"
    "${zlib_SOURCE_DIR}/deflate.c"
    "${zlib_SOURCE_DIR}/infback.c"
    "${zlib_SOURCE_DIR}/inffast.c"
    "${zlib_SOURCE_DIR}/inflate.c"
    "${zlib_SOURCE_DIR}/inftrees.c"
    "${zlib_SOURCE_DIR}/trees.c"
    "${zlib_SOURCE_DIR}/uncompr.c"
    "${zlib_SOURCE_DIR}/zutil.c"
)

add_library(zlib_static STATIC ${ZLIB_SRCS})
target_include_directories(zlib_static PUBLIC "${zlib_SOURCE_DIR}")
set_target_properties(zlib_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(zlib_static PRIVATE /w)
    target_compile_definitions(zlib_static PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
