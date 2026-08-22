# ZenLib + MediaInfoLib 源码集成
include(CheckTypeSize)
include(CheckSymbolExists)

# ===== ZenLib 静态库 =====
set(ZENLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ZenLib")
set(ZENLIB_SRC "${ZENLIB_ROOT}/Source/ZenLib")

file(GLOB_RECURSE ZENLIB_SOURCES CONFIGURE_DEPENDS "${ZENLIB_SRC}/*.cpp" "${ZENLIB_SRC}/*.c")
file(GLOB_RECURSE ZENLIB_HEADERS CONFIGURE_DEPENDS "${ZENLIB_SRC}/*.h")

# 排除 HTTP client (项目不需要)
file(GLOB ZENLIB_HTTP_CLIENT_SOURCES CONFIGURE_DEPENDS "${ZENLIB_SRC}/HTTP_Client/*")
list(REMOVE_ITEM ZENLIB_SOURCES ${ZENLIB_HTTP_CLIENT_SOURCES})

add_library(zen STATIC ${ZENLIB_SOURCES} ${ZENLIB_HEADERS})
target_include_directories(zen PUBLIC ${ZENLIB_ROOT}/Source)
target_compile_definitions(zen PUBLIC UNICODE _UNICODE _FILE_OFFSET_BITS=64)
target_compile_features(zen PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
if(CMAKE_THREAD_LIBS_INIT)
    target_link_libraries(zen PUBLIC ${CMAKE_THREAD_LIBS_INIT})
endif()

check_type_size(size_t SIZE_T_SIZE)
check_type_size(long LONG_SIZE)
if((LONG_SIZE GREATER 4) AND (SIZE_T_SIZE EQUAL LONG_SIZE))
    target_compile_definitions(zen PUBLIC SIZE_T_IS_LONG)
endif()

check_symbol_exists(gmtime_r time.h HAVE_GMTIME_R)
if(HAVE_GMTIME_R)
    target_compile_definitions(zen PUBLIC HAVE_GMTIME_R)
endif()
check_symbol_exists(localtime_r time.h HAVE_LOCALTIME_R)
if(HAVE_LOCALTIME_R)
    target_compile_definitions(zen PUBLIC HAVE_LOCALTIME_R)
endif()

# ===== MediaInfoLib 静态库 =====
set(MEDIAINFO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/MediaInfoLib")
set(MEDIAINFO_SRC "${MEDIAINFO_ROOT}/Source/MediaInfo")
set(THIRDPARTY_SRC "${MEDIAINFO_ROOT}/Source/ThirdParty")

file(GLOB_RECURSE MEDIAINFO_SOURCES CONFIGURE_DEPENDS "${MEDIAINFO_SRC}/*.cpp" "${MEDIAINFO_SRC}/*.c")
file(GLOB THIRDPARTY_C_SOURCES CONFIGURE_DEPENDS "${THIRDPARTY_SRC}/*/*.c")
file(GLOB THIRDPARTY_CXX_SOURCES CONFIGURE_DEPENDS "${THIRDPARTY_SRC}/*/*.cpp" "${THIRDPARTY_SRC}/*/*.cc")
list(FILTER THIRDPARTY_C_SOURCES EXCLUDE REGEX ".*/(aesxam|tablegen|shasum|pwd2key)\\.c$")

set(MEDIAINFO_ALL_SOURCES ${MEDIAINFO_SOURCES} ${THIRDPARTY_C_SOURCES} ${THIRDPARTY_CXX_SOURCES})

file(GLOB MEDIAINFO_PUBLIC_HDRS CONFIGURE_DEPENDS
    "${MEDIAINFO_SRC}/MediaInfo.h" "${MEDIAINFO_SRC}/MediaInfoList.h"
    "${MEDIAINFO_SRC}/MediaInfo_Const.h" "${MEDIAINFO_SRC}/MediaInfo_Events.h"
    "${MEDIAINFO_ROOT}/Source/MediaInfoDLL/MediaInfoDLL.h"
    "${MEDIAINFO_ROOT}/Source/MediaInfoDLL/MediaInfoDLL_Static.h"
)

add_library(mediainfo STATIC ${MEDIAINFO_ALL_SOURCES} ${MEDIAINFO_PUBLIC_HDRS})

target_include_directories(mediainfo PUBLIC
    ${MEDIAINFO_ROOT}/Source ${MEDIAINFO_ROOT}/Source/MediaInfo
    ${MEDIAINFO_ROOT}/Source/MediaInfoDLL
)
target_include_directories(mediainfo PRIVATE
    ${THIRDPARTY_SRC} ${THIRDPARTY_SRC}/md5 ${THIRDPARTY_SRC}/base64
    ${THIRDPARTY_SRC}/aes-gladman ${THIRDPARTY_SRC}/sha1-gladman
    ${THIRDPARTY_SRC}/sha2-gladman ${THIRDPARTY_SRC}/hmac-gladman
    ${THIRDPARTY_SRC}/tinyxml2
)

target_compile_features(mediainfo PUBLIC cxx_std_17)
target_compile_definitions(mediainfo PRIVATE
    MEDIAINFO_LIBCURL_NO MEDIAINFO_LIBMMS_NO
    MEDIAINFO_GRAPHVIZ_DLL_RUNTIME FMT_UNICODE=0
)

target_link_libraries(mediainfo PUBLIC zen)
find_package(ZLIB REQUIRED)
target_link_libraries(mediainfo PUBLIC ZLIB::ZLIB)
