# ============================================================
# ZenLib + MediaInfoLib 源码集成
# 使用 GLOB 自动收集源文件，便于版本升级
# ============================================================

include(CheckTypeSize)
include(CheckSymbolExists)

# Unity Build 加速 (由根 CMakeLists 的 VIDEOEYE_UNITY_BUILD 控制, 默认 OFF)
# 注意: MediaInfoLib 的 Multiple/*.cpp 存在同名文件级全局变量、ZenLib 的
#       std::numeric_limits::max() 会被 Windows min/max 宏展开 —— 均与 Unity Build
#       不兼容, 故默认关闭; 如需尝试, cmake -DVIDEOEYE_UNITY_BUILD=ON
if(VIDEOEYE_UNITY_BUILD)
    set(_UNITY_BATCH 32)
else()
    set(_UNITY_BATCH 0)
endif()

# ============================================================
# ZenLib 静态库
# ============================================================
set(ZENLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ZenLib")
set(ZENLIB_SRC "${ZENLIB_ROOT}/Source/ZenLib")

file(GLOB_RECURSE ZENLIB_SOURCES CONFIGURE_DEPENDS
    "${ZENLIB_SRC}/*.cpp"
    "${ZENLIB_SRC}/*.c"
)
file(GLOB_RECURSE ZENLIB_HEADERS CONFIGURE_DEPENDS
    "${ZENLIB_SRC}/*.h"
)

# 排除不需要的文件 (工具、示例、测试)
file(GLOB ZENLIB_HTTP_CLIENT_SOURCES CONFIGURE_DEPENDS "${ZENLIB_SRC}/HTTP_Client/*.c" "${ZENLIB_SRC}/HTTP_Client/*.cpp")
file(GLOB ZENLIB_HTTP_CLIENT_HEADERS CONFIGURE_DEPENDS "${ZENLIB_SRC}/HTTP_Client/*.h")
list(REMOVE_ITEM ZENLIB_SOURCES ${ZENLIB_HTTP_CLIENT_SOURCES})
list(REMOVE_ITEM ZENLIB_HEADERS ${ZENLIB_HTTP_CLIENT_HEADERS})

add_library(zen STATIC ${ZENLIB_SOURCES} ${ZENLIB_HEADERS})
if(_UNITY_BATCH GREATER 0)
    set_property(TARGET zen PROPERTY UNITY_BUILD ON)
    set_property(TARGET zen PROPERTY UNITY_BUILD_BATCH_SIZE ${_UNITY_BATCH})
endif()

target_include_directories(zen PUBLIC ${ZENLIB_ROOT}/Source)

target_compile_definitions(zen PUBLIC UNICODE _UNICODE)

target_compile_features(zen PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
if(CMAKE_THREAD_LIBS_INIT)
    target_link_libraries(zen PUBLIC ${CMAKE_THREAD_LIBS_INIT})
endif()

# Large file support (target 级别, 避免全局 add_definitions 泄漏到后续 target)
target_compile_definitions(zen PUBLIC _FILE_OFFSET_BITS=64)

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

# ============================================================
# MediaInfoLib 静态库
# ============================================================
set(MEDIAINFO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/MediaInfoLib")
set(MEDIAINFO_SRC "${MEDIAINFO_ROOT}/Source/MediaInfo")
set(THIRDPARTY_SRC "${MEDIAINFO_ROOT}/Source/ThirdParty")

# 自动收集 MediaInfoLib 源文件
file(GLOB_RECURSE MEDIAINFO_SOURCES CONFIGURE_DEPENDS
    "${MEDIAINFO_SRC}/*.cpp"
    "${MEDIAINFO_SRC}/*.c"
)

# 自动收集 ThirdParty 源文件，排除不需要的工具/示例代码
file(GLOB THIRDPARTY_C_SOURCES CONFIGURE_DEPENDS "${THIRDPARTY_SRC}/*/*.c")
file(GLOB THIRDPARTY_CXX_SOURCES CONFIGURE_DEPENDS "${THIRDPARTY_SRC}/*/*.cpp" "${THIRDPARTY_SRC}/*/*.cc")

# 排除不需要的文件 (工具、示例、测试)
list(FILTER THIRDPARTY_C_SOURCES EXCLUDE REGEX ".*/(aesxam|tablegen|shasum|pwd2key)\\.c$")

set(MEDIAINFO_ALL_SOURCES ${MEDIAINFO_SOURCES} ${THIRDPARTY_C_SOURCES} ${THIRDPARTY_CXX_SOURCES})

# MediaInfoLib 公共头文件
file(GLOB MEDIAINFO_PUBLIC_HDRS CONFIGURE_DEPENDS
    "${MEDIAINFO_SRC}/MediaInfo.h"
    "${MEDIAINFO_SRC}/MediaInfoList.h"
    "${MEDIAINFO_SRC}/MediaInfo_Const.h"
    "${MEDIAINFO_SRC}/MediaInfo_Events.h"
    "${MEDIAINFO_ROOT}/Source/MediaInfoDLL/MediaInfoDLL.h"
    "${MEDIAINFO_ROOT}/Source/MediaInfoDLL/MediaInfoDLL_Static.h"
)

add_library(mediainfo STATIC ${MEDIAINFO_ALL_SOURCES} ${MEDIAINFO_PUBLIC_HDRS})
# 注意: MediaInfoLib 的 Multiple/*.cpp 存在大量同名文件级全局变量
# (如 MaxCountSameElementInTrace 同时在 File_Mk/File_Mxf 定义), Unity Build 会冲突,
# 故不启用 unity; zen/ap4 启用。
# if(_UNITY_BATCH GREATER 0) ... endif()

target_include_directories(mediainfo PUBLIC
    ${MEDIAINFO_ROOT}/Source
    ${MEDIAINFO_ROOT}/Source/MediaInfo
    ${MEDIAINFO_ROOT}/Source/MediaInfoDLL
)

target_include_directories(mediainfo PRIVATE
    ${THIRDPARTY_SRC}
    ${THIRDPARTY_SRC}/md5
    ${THIRDPARTY_SRC}/base64
    ${THIRDPARTY_SRC}/aes-gladman
    ${THIRDPARTY_SRC}/sha1-gladman
    ${THIRDPARTY_SRC}/sha2-gladman
    ${THIRDPARTY_SRC}/hmac-gladman
    ${THIRDPARTY_SRC}/tinyxml2
)

target_compile_features(mediainfo PUBLIC cxx_std_17)

# 禁用不需要的特性
target_compile_definitions(mediainfo PRIVATE
    MEDIAINFO_LIBCURL_NO
    MEDIAINFO_LIBMMS_NO
    MEDIAINFO_GRAPHVIZ_DLL_RUNTIME
    FMT_UNICODE=0
)

# 链接依赖
target_link_libraries(mediainfo PUBLIC zen)

find_package(ZLIB REQUIRED)
target_link_libraries(mediainfo PUBLIC ZLIB::ZLIB)
