cmake_minimum_required(VERSION 3.16)

include(CheckTypeSize)
include(CheckSymbolExists)

# ============================================================
# 构建 ZenLib 静态库
# ============================================================
set(ZENLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ZenLib")
set(ZENLIB_SRC "${ZENLIB_ROOT}/Source/ZenLib")

# ZenLib 头文件
set(ZENLIB_HEADERS
    ${ZENLIB_SRC}/BitStream.h
    ${ZENLIB_SRC}/BitStream_Fast.h
    ${ZENLIB_SRC}/BitStream_LE.h
    ${ZENLIB_SRC}/Conf.h
    ${ZENLIB_SRC}/Conf_Internal.h
    ${ZENLIB_SRC}/CriticalSection.h
    ${ZENLIB_SRC}/Dir.h
    ${ZENLIB_SRC}/File.h
    ${ZENLIB_SRC}/FileName.h
    ${ZENLIB_SRC}/InfoMap.h
    ${ZENLIB_SRC}/MemoryDebug.h
    ${ZENLIB_SRC}/OS_Utils.h
    ${ZENLIB_SRC}/PreComp.h
    ${ZENLIB_SRC}/Thread.h
    ${ZENLIB_SRC}/Trace.h
    ${ZENLIB_SRC}/Translation.h
    ${ZENLIB_SRC}/Utils.h
    ${ZENLIB_SRC}/Ztring.h
    ${ZENLIB_SRC}/ZtringList.h
    ${ZENLIB_SRC}/ZtringListList.h
    ${ZENLIB_SRC}/ZtringListListF.h
    ${ZENLIB_SRC}/int128s.h
    ${ZENLIB_SRC}/int128u.h
    ${ZENLIB_SRC}/Format/Html/Html_Handler.h
    ${ZENLIB_SRC}/Format/Html/Html_Request.h
    ${ZENLIB_SRC}/Format/Http/Http_Cookies.h
    ${ZENLIB_SRC}/Format/Http/Http_Handler.h
    ${ZENLIB_SRC}/Format/Http/Http_Request.h
    ${ZENLIB_SRC}/Format/Http/Http_Utils.h
)

# ZenLib 源文件
set(ZENLIB_SOURCES
    ${ZENLIB_SRC}/Conf.cpp
    ${ZENLIB_SRC}/CriticalSection.cpp
    ${ZENLIB_SRC}/Dir.cpp
    ${ZENLIB_SRC}/File.cpp
    ${ZENLIB_SRC}/FileName.cpp
    ${ZENLIB_SRC}/InfoMap.cpp
    ${ZENLIB_SRC}/int128s.cpp
    ${ZENLIB_SRC}/int128u.cpp
    ${ZENLIB_SRC}/MemoryDebug.cpp
    ${ZENLIB_SRC}/OS_Utils.cpp
    ${ZENLIB_SRC}/Translation.cpp
    ${ZENLIB_SRC}/Thread.cpp
    ${ZENLIB_SRC}/Utils.cpp
    ${ZENLIB_SRC}/Ztring.cpp
    ${ZENLIB_SRC}/ZtringList.cpp
    ${ZENLIB_SRC}/ZtringListList.cpp
    ${ZENLIB_SRC}/ZtringListListF.cpp
    ${ZENLIB_SRC}/Format/Html/Html_Handler.cpp
    ${ZENLIB_SRC}/Format/Html/Html_Request.cpp
    ${ZENLIB_SRC}/Format/Http/Http_Cookies.cpp
    ${ZENLIB_SRC}/Format/Http/Http_Handler.cpp
    ${ZENLIB_SRC}/Format/Http/Http_Request.cpp
    ${ZENLIB_SRC}/Format/Http/Http_Utils.cpp
)

add_library(zen STATIC ${ZENLIB_SOURCES} ${ZENLIB_HEADERS})

target_include_directories(zen PUBLIC ${ZENLIB_ROOT}/Source)

target_compile_definitions(zen PUBLIC UNICODE _UNICODE)

target_compile_features(zen PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
if(CMAKE_THREAD_LIBS_INIT)
    target_link_libraries(zen PUBLIC ${CMAKE_THREAD_LIBS_INIT})
endif()

# Large file support
add_definitions(-D_FILE_OFFSET_BITS=64)

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
# 构建 MediaInfoLib 静态库
# ============================================================
set(MEDIAINFO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../MediaInfoLib")
set(MEDIAINFO_SRC "${MEDIAINFO_ROOT}/Source/MediaInfo")
set(THIRDPARTY_SRC "${MEDIAINFO_ROOT}/Source/ThirdParty")

set(MEDIAINFO_PUBLIC_HDRS
    ${MEDIAINFO_SRC}/MediaInfo.h
    ${MEDIAINFO_SRC}/MediaInfoList.h
    ${MEDIAINFO_SRC}/MediaInfo_Const.h
    ${MEDIAINFO_SRC}/MediaInfo_Events.h
    ${MEDIAINFO_ROOT}/Source/MediaInfoDLL/MediaInfoDLL.h
    ${MEDIAINFO_ROOT}/Source/MediaInfoDLL/MediaInfoDLL_Static.h
)

set(MEDIAINFO_SOURCES
    # ThirdParty
    ${THIRDPARTY_SRC}/md5/md5.c
    ${THIRDPARTY_SRC}/aes-gladman/aes_modes.c
    ${THIRDPARTY_SRC}/aes-gladman/aes_ni.c
    ${THIRDPARTY_SRC}/aes-gladman/aescrypt.c
    ${THIRDPARTY_SRC}/aes-gladman/aeskey.c
    ${THIRDPARTY_SRC}/aes-gladman/aestab.c
    ${THIRDPARTY_SRC}/sha1-gladman/sha1.c
    ${THIRDPARTY_SRC}/sha2-gladman/sha2.c
    ${THIRDPARTY_SRC}/hmac-gladman/hmac.c
    ${THIRDPARTY_SRC}/tfsxml/tfsxml.c
    ${THIRDPARTY_SRC}/fmt/format.cc
    ${THIRDPARTY_SRC}/tinyxml2/tinyxml2.cpp

    # Core
    ${MEDIAINFO_SRC}/File__Analyze.cpp
    ${MEDIAINFO_SRC}/File__Analyze_Buffer.cpp
    ${MEDIAINFO_SRC}/File__Analyze_Buffer_MinimizeSize.cpp
    ${MEDIAINFO_SRC}/File__Analyze_Element.cpp
    ${MEDIAINFO_SRC}/File__Analyze_Streams.cpp
    ${MEDIAINFO_SRC}/File__Analyze_Streams_Finish.cpp
    ${MEDIAINFO_SRC}/File__Base.cpp
    ${MEDIAINFO_SRC}/File__MultipleParsing.cpp
    ${MEDIAINFO_SRC}/File__Duplicate.cpp
    ${MEDIAINFO_SRC}/File__HasReferences.cpp
    ${MEDIAINFO_SRC}/File_Dummy.cpp
    ${MEDIAINFO_SRC}/File_Other.cpp
    ${MEDIAINFO_SRC}/File_Unknown.cpp
    ${MEDIAINFO_SRC}/HashWrapper.cpp
    ${MEDIAINFO_SRC}/MediaInfo.cpp
    ${MEDIAINFO_SRC}/MediaInfo_Config.cpp
    ${MEDIAINFO_SRC}/MediaInfo_Config_Automatic.cpp
    ${MEDIAINFO_SRC}/MediaInfo_Config_MediaInfo.cpp
    ${MEDIAINFO_SRC}/MediaInfo_Config_PerPackage.cpp
    ${MEDIAINFO_SRC}/MediaInfo_File.cpp
    ${MEDIAINFO_SRC}/MediaInfo_Inform.cpp
    ${MEDIAINFO_SRC}/MediaInfo_Internal.cpp
    ${MEDIAINFO_SRC}/MediaInfoList.cpp
    ${MEDIAINFO_SRC}/MediaInfoList_Internal.cpp
    ${MEDIAINFO_SRC}/TimeCode.cpp
    ${MEDIAINFO_SRC}/XmlUtils.cpp
    ${MEDIAINFO_SRC}/OutputHelpers.cpp
    ${MEDIAINFO_SRC}/ExternalCommandHelpers.cpp

    # Archive
    ${MEDIAINFO_SRC}/Archive/File_7z.cpp
    ${MEDIAINFO_SRC}/Archive/File_Ace.cpp
    ${MEDIAINFO_SRC}/Archive/File_Bzip2.cpp
    ${MEDIAINFO_SRC}/Archive/File_Elf.cpp
    ${MEDIAINFO_SRC}/Archive/File_Gzip.cpp
    ${MEDIAINFO_SRC}/Archive/File_Iso9660.cpp
    ${MEDIAINFO_SRC}/Archive/File_MachO.cpp
    ${MEDIAINFO_SRC}/Archive/File_Mz.cpp
    ${MEDIAINFO_SRC}/Archive/File_Rar.cpp
    ${MEDIAINFO_SRC}/Archive/File_Tar.cpp
    ${MEDIAINFO_SRC}/Archive/File_Zip.cpp

    # Audio
    ${MEDIAINFO_SRC}/Audio/File_Aac.cpp
    ${MEDIAINFO_SRC}/Audio/File_Aac_GeneralAudio.cpp
    ${MEDIAINFO_SRC}/Audio/File_Aac_GeneralAudio_Sbr.cpp
    ${MEDIAINFO_SRC}/Audio/File_Aac_GeneralAudio_Sbr_Ps.cpp
    ${MEDIAINFO_SRC}/Audio/File_Aac_Main.cpp
    ${MEDIAINFO_SRC}/Audio/File_Aac_Others.cpp
    ${MEDIAINFO_SRC}/Audio/File_Ac3.cpp
    ${MEDIAINFO_SRC}/Audio/File_Ac4.cpp
    ${MEDIAINFO_SRC}/Audio/File_Adm.cpp
    ${MEDIAINFO_SRC}/Audio/File_Adpcm.cpp
    ${MEDIAINFO_SRC}/Audio/File_Als.cpp
    ${MEDIAINFO_SRC}/Audio/File_Amr.cpp
    ${MEDIAINFO_SRC}/Audio/File_Amv.cpp
    ${MEDIAINFO_SRC}/Audio/File_Ape.cpp
    ${MEDIAINFO_SRC}/Audio/File_Aptx100.cpp
    ${MEDIAINFO_SRC}/Audio/File_Au.cpp
    ${MEDIAINFO_SRC}/Audio/File_Caf.cpp
    ${MEDIAINFO_SRC}/Audio/File_Celt.cpp
    ${MEDIAINFO_SRC}/Audio/File_ChannelGrouping.cpp
    ${MEDIAINFO_SRC}/Audio/File_ChannelSplitting.cpp
    ${MEDIAINFO_SRC}/Audio/File_Dat.cpp
    ${MEDIAINFO_SRC}/Audio/File_Dsdiff.cpp
    ${MEDIAINFO_SRC}/Audio/File_Dsf.cpp
    ${MEDIAINFO_SRC}/Audio/File_Dts.cpp
    ${MEDIAINFO_SRC}/Audio/File_DtsUhd.cpp
    ${MEDIAINFO_SRC}/Audio/File_DolbyAudioMetadata.cpp
    ${MEDIAINFO_SRC}/Audio/File_DolbyE.cpp
    ${MEDIAINFO_SRC}/Audio/File_ExtendedModule.cpp
    ${MEDIAINFO_SRC}/Audio/File_Flac.cpp
    ${MEDIAINFO_SRC}/Audio/File_Iamf.cpp
    ${MEDIAINFO_SRC}/Audio/File_Iab.cpp
    ${MEDIAINFO_SRC}/Audio/File_ImpulseTracker.cpp
    ${MEDIAINFO_SRC}/Audio/File_La.cpp
    ${MEDIAINFO_SRC}/Audio/File_Mga.cpp
    ${MEDIAINFO_SRC}/Audio/File_Midi.cpp
    ${MEDIAINFO_SRC}/Audio/File_Module.cpp
    ${MEDIAINFO_SRC}/Audio/File_Mpc.cpp
    ${MEDIAINFO_SRC}/Audio/File_MpcSv8.cpp
    ${MEDIAINFO_SRC}/Audio/File_Mpega.cpp
    ${MEDIAINFO_SRC}/Audio/File_Mpegh3da.cpp
    ${MEDIAINFO_SRC}/Audio/File_OpenMG.cpp
    ${MEDIAINFO_SRC}/Audio/File_Opus.cpp
    ${MEDIAINFO_SRC}/Audio/File_Pcm.cpp
    ${MEDIAINFO_SRC}/Audio/File_Pcm_M2ts.cpp
    ${MEDIAINFO_SRC}/Audio/File_Pcm_Vob.cpp
    ${MEDIAINFO_SRC}/Audio/File_Ps2Audio.cpp
    ${MEDIAINFO_SRC}/Audio/File_Rkau.cpp
    ${MEDIAINFO_SRC}/Audio/File_ScreamTracker3.cpp
    ${MEDIAINFO_SRC}/Audio/File_SmpteSt0302.cpp
    ${MEDIAINFO_SRC}/Audio/File_SmpteSt0337.cpp
    ${MEDIAINFO_SRC}/Audio/File_SmpteSt0331.cpp
    ${MEDIAINFO_SRC}/Audio/File_Speex.cpp
    ${MEDIAINFO_SRC}/Audio/File_Tak.cpp
    ${MEDIAINFO_SRC}/Audio/File_Tta.cpp
    ${MEDIAINFO_SRC}/Audio/File_TwinVQ.cpp
    ${MEDIAINFO_SRC}/Audio/File_Usac.cpp
    ${MEDIAINFO_SRC}/Audio/File_Vorbis.cpp
    ${MEDIAINFO_SRC}/Audio/File_Wvpk.cpp

    # Duplicate
    ${MEDIAINFO_SRC}/Duplicate/File__Duplicate__Base.cpp
    ${MEDIAINFO_SRC}/Duplicate/File__Duplicate__Writer.cpp
    ${MEDIAINFO_SRC}/Duplicate/File__Duplicate_MpegTs.cpp

    # Export
    ${MEDIAINFO_SRC}/Export/Export_EbuCore.cpp
    ${MEDIAINFO_SRC}/Export/Export_Niso.cpp
    ${MEDIAINFO_SRC}/Export/Export_Graph.cpp
    ${MEDIAINFO_SRC}/Export/Export_Fims.cpp
    ${MEDIAINFO_SRC}/Export/Export_Mpeg7.cpp
    ${MEDIAINFO_SRC}/Export/Export_PBCore.cpp
    ${MEDIAINFO_SRC}/Export/Export_PBCore2.cpp
    ${MEDIAINFO_SRC}/Export/Export_reVTMD.cpp

    # Image
    ${MEDIAINFO_SRC}/Image/File_AmigaIcon.cpp
    ${MEDIAINFO_SRC}/Image/File_ArriRaw.cpp
    ${MEDIAINFO_SRC}/Image/File_Bmp.cpp
    ${MEDIAINFO_SRC}/Image/File_Bpg.cpp
    ${MEDIAINFO_SRC}/Image/File_Dds.cpp
    ${MEDIAINFO_SRC}/Image/File_Dpx.cpp
    ${MEDIAINFO_SRC}/Image/File_Exr.cpp
    ${MEDIAINFO_SRC}/Image/File_Gif.cpp
    ${MEDIAINFO_SRC}/Image/File_Ico.cpp
    ${MEDIAINFO_SRC}/Image/File_GainMap.cpp
    ${MEDIAINFO_SRC}/Image/File_Jpeg.cpp
    ${MEDIAINFO_SRC}/Image/File_Pcx.cpp
    ${MEDIAINFO_SRC}/Image/File_Png.cpp
    ${MEDIAINFO_SRC}/Image/File_Psd.cpp
    ${MEDIAINFO_SRC}/Image/File_Rle.cpp
    ${MEDIAINFO_SRC}/Image/File_Tiff.cpp
    ${MEDIAINFO_SRC}/Image/File_Tga.cpp
    ${MEDIAINFO_SRC}/Image/File_WebP.cpp

    # Multiple
    ${MEDIAINFO_SRC}/Multiple/File__ReferenceFilesHelper.cpp
    ${MEDIAINFO_SRC}/Multiple/File__ReferenceFilesHelper_Resource.cpp
    ${MEDIAINFO_SRC}/Multiple/File__ReferenceFilesHelper_Sequence.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Aaf.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ancillary.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Bdmv.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Cdxa.cpp
    ${MEDIAINFO_SRC}/Multiple/File_DashMpd.cpp
    ${MEDIAINFO_SRC}/Multiple/File_DcpAm.cpp
    ${MEDIAINFO_SRC}/Multiple/File_DcpCpl.cpp
    ${MEDIAINFO_SRC}/Multiple/File_DcpPkl.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Dpg.cpp
    ${MEDIAINFO_SRC}/Multiple/File_DvDif.cpp
    ${MEDIAINFO_SRC}/Multiple/File_DvDif_Analysis.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Dvdv.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Dxw.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Flv.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Gxf.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Gxf_TimeCode.cpp
    ${MEDIAINFO_SRC}/Multiple/File_HdsF4m.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Hls.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ibi.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ibi_Creation.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ism.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ivf.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Lxf.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mk.cpp
    ${MEDIAINFO_SRC}/Multiple/File_MiXml.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mpeg4.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mpeg4_Descriptors.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mpeg4_Elements.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mpeg4_TimeCode.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mpeg_Descriptors.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mpeg_Psi.cpp
    ${MEDIAINFO_SRC}/Multiple/File_MpegPs.cpp
    ${MEDIAINFO_SRC}/Multiple/File_MpegTs.cpp
    ${MEDIAINFO_SRC}/Multiple/File_MpegTs_Duplicate.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Mxf.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Nsv.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Nut.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ogg.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ogg_SubElement.cpp
    ${MEDIAINFO_SRC}/Multiple/File_P2_Clip.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Pmp.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Ptx.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Riff.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Riff_Elements.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Rm.cpp
    ${MEDIAINFO_SRC}/Multiple/File_SequenceInfo.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Skm.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Swf.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Umf.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Vbi.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Wm.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Wm_Elements.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Wtv.cpp
    ${MEDIAINFO_SRC}/Multiple/File_Xdcam_Clip.cpp

    # Reader
    ${MEDIAINFO_SRC}/Reader/Reader_Directory.cpp
    ${MEDIAINFO_SRC}/Reader/Reader_File.cpp
    ${MEDIAINFO_SRC}/Reader/Reader_libcurl.cpp

    # Tag
    ${MEDIAINFO_SRC}/Tag/File__Tags.cpp
    ${MEDIAINFO_SRC}/Tag/File_ApeTag.cpp
    ${MEDIAINFO_SRC}/Tag/File_C2pa.cpp
    ${MEDIAINFO_SRC}/Tag/File_Exif.cpp
    ${MEDIAINFO_SRC}/Tag/File_Icc.cpp
    ${MEDIAINFO_SRC}/Tag/File_Id3.cpp
    ${MEDIAINFO_SRC}/Tag/File_Id3v2.cpp
    ${MEDIAINFO_SRC}/Tag/File_Iim.cpp
    ${MEDIAINFO_SRC}/Tag/File_Lyrics3.cpp
    ${MEDIAINFO_SRC}/Tag/File_Lyrics3v2.cpp
    ${MEDIAINFO_SRC}/Tag/File_PropertyList.cpp
    ${MEDIAINFO_SRC}/Tag/File_SphericalVideo.cpp
    ${MEDIAINFO_SRC}/Tag/File_VorbisCom.cpp
    ${MEDIAINFO_SRC}/Tag/File_Xmp.cpp

    # Text
    ${MEDIAINFO_SRC}/Text/File_AribStdB24B37.cpp
    ${MEDIAINFO_SRC}/Text/File_Cdp.cpp
    ${MEDIAINFO_SRC}/Text/File_Cmml.cpp
    ${MEDIAINFO_SRC}/Text/File_DvbSubtitle.cpp
    ${MEDIAINFO_SRC}/Text/File_DtvccTransport.cpp
    ${MEDIAINFO_SRC}/Text/File_Eia608.cpp
    ${MEDIAINFO_SRC}/Text/File_Eia708.cpp
    ${MEDIAINFO_SRC}/Text/File_Kate.cpp
    ${MEDIAINFO_SRC}/Text/File_N19.cpp
    ${MEDIAINFO_SRC}/Text/File_OtherText.cpp
    ${MEDIAINFO_SRC}/Text/File_Pac.cpp
    ${MEDIAINFO_SRC}/Text/File_Pdf.cpp
    ${MEDIAINFO_SRC}/Text/File_Pgs.cpp
    ${MEDIAINFO_SRC}/Text/File_Scc.cpp
    ${MEDIAINFO_SRC}/Text/File_Scte20.cpp
    ${MEDIAINFO_SRC}/Text/File_Sdp.cpp
    ${MEDIAINFO_SRC}/Text/File_SubRip.cpp
    ${MEDIAINFO_SRC}/Text/File_Teletext.cpp
    ${MEDIAINFO_SRC}/Text/File_TimedText.cpp
    ${MEDIAINFO_SRC}/Text/File_Ttml.cpp

    # Video
    ${MEDIAINFO_SRC}/Video/File_Aic.cpp
    ${MEDIAINFO_SRC}/Video/File_AfdBarData.cpp
    ${MEDIAINFO_SRC}/Video/File_Apv.cpp
    ${MEDIAINFO_SRC}/Video/File_Av1.cpp
    ${MEDIAINFO_SRC}/Video/File_Av2.cpp
    ${MEDIAINFO_SRC}/Video/File_Avc.cpp
    ${MEDIAINFO_SRC}/Video/File_Avc_Duplicate.cpp
    ${MEDIAINFO_SRC}/Video/File_AvsV.cpp
    ${MEDIAINFO_SRC}/Video/File_Avs3V.cpp
    ${MEDIAINFO_SRC}/Video/File_Canopus.cpp
    ${MEDIAINFO_SRC}/Video/File_CineForm.cpp
    ${MEDIAINFO_SRC}/Video/File_Dirac.cpp
    ${MEDIAINFO_SRC}/Video/File_DolbyVisionMetadata.cpp
    ${MEDIAINFO_SRC}/Video/File_Ffv1.cpp
    ${MEDIAINFO_SRC}/Video/File_Flic.cpp
    ${MEDIAINFO_SRC}/Video/File_Fraps.cpp
    ${MEDIAINFO_SRC}/Video/File_H263.cpp
    ${MEDIAINFO_SRC}/Video/File_HdrVividMetadata.cpp
    ${MEDIAINFO_SRC}/Video/File_Hevc.cpp
    ${MEDIAINFO_SRC}/Video/File_HuffYuv.cpp
    ${MEDIAINFO_SRC}/Video/File_Lagarith.cpp
    ${MEDIAINFO_SRC}/Video/File_Mpeg4v.cpp
    ${MEDIAINFO_SRC}/Video/File_Mpegv.cpp
    ${MEDIAINFO_SRC}/Video/File_ProRes.cpp
    ${MEDIAINFO_SRC}/Video/File_Theora.cpp
    ${MEDIAINFO_SRC}/Video/File_Vc1.cpp
    ${MEDIAINFO_SRC}/Video/File_Vc3.cpp
    ${MEDIAINFO_SRC}/Video/File_Vp8.cpp
    ${MEDIAINFO_SRC}/Video/File_Vp9.cpp
    ${MEDIAINFO_SRC}/Video/File_Vvc.cpp
    ${MEDIAINFO_SRC}/Video/File_Y4m.cpp
)

add_library(mediainfo STATIC ${MEDIAINFO_SOURCES} ${MEDIAINFO_PUBLIC_HDRS})

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

# 链接 zen 库
target_link_libraries(mediainfo PUBLIC zen)

# 链接 zlib (系统库)
find_package(ZLIB REQUIRED)
target_link_libraries(mediainfo PUBLIC ZLIB::ZLIB)
