#include "FrameData.h"
#include <cstring>
#include <iomanip>
#include <sstream>

namespace videoeye {
namespace model {

std::string StreamInfo::ToString() const {
    std::ostringstream oss;

    const int kKeyWidth = 32;
    auto Print = [&](const char* key, const std::string& value) {
        if (value.empty()) {
            return;
        }
        oss << "  " << std::left << std::setw(kKeyWidth) << key << ": " << value << "\n";
    };

    oss << "ExtractorFormat\n";
    Print("Complete name", extractor.complete_name);
    Print("Format", extractor.format);
    Print("Format profile", extractor.format_profile);
    Print("Codec ID", extractor.codec_id);
    Print("File size", extractor.file_size);
    Print("Duration", extractor.duration);
    Print("Overall bit rate mode", extractor.overall_bit_rate_mode);
    Print("Overall bit rate", extractor.overall_bit_rate);
    Print("Frame rate", extractor.frame_rate);
    Print("Description", extractor.description);
    Print("Writing application", extractor.writing_application);

    oss << "\nAudioCodecFormat\n";
    Print("ID", audio.id);
    Print("Format", audio.format);
    Print("Format/Info", audio.format_info);
    Print("Codec ID", audio.codec_id);
    Print("Duration", audio.duration);
    Print("Bit rate mode", audio.bit_rate_mode);
    Print("Bit rate", audio.bit_rate);
    Print("Channel(s)", audio.channels);
    Print("Channel layout", audio.channel_layout);
    Print("Sampling rate", audio.sampling_rate);
    Print("Frame rate", audio.frame_rate);
    Print("Compression mode", audio.compression_mode);
    Print("Stream size", audio.stream_size);
    Print("Default", audio.is_default);
    Print("Alternate group", audio.alternate_group);

    oss << "\nVideoCodecFormat\n";
    Print("ID", video.id);
    Print("Format", video.format);
    Print("Format/Info", video.format_info);
    Print("Format profile", video.format_profile);
    Print("Format settings", video.format_settings);
    Print("Format settings, CABAC", video.format_settings_cabac);
    Print("Format settings, Reference fra", video.format_settings_reference_frames);
    Print("HDR format", video.hdr_format);
    Print("HDR format compatibility", video.hdr_format_compatibility);
    Print("Codec ID", video.codec_id);
    Print("Codec ID/Info", video.codec_id_info);
    Print("Duration", video.duration);
    Print("Bit rate", video.bit_rate);
    Print("Width", video.width);
    Print("Height", video.height);
    Print("Display aspect ratio", video.display_aspect_ratio);
    Print("Frame rate mode", video.frame_rate_mode);
    Print("Frame rate", video.frame_rate);
    Print("Color space", video.color_space);
    Print("Chroma subsampling", video.chroma_subsampling);
    Print("Bit depth", video.bit_depth);
    Print("Scan type", video.scan_type);
    Print("Bits/(Pixel*Frame)", video.bits_pixel_frame);
    Print("Stream size", video.stream_size);
    Print("Color range", video.color_range);
    Print("Color primaries", video.color_primaries);
    Print("Transfer characteristics", video.transfer_characteristics);
    Print("Matrix coefficients", video.matrix_coefficients);
    Print("Mastering display color primaries", video.mastering_display_color_primaries);
    Print("Mastering display luminance", video.mastering_display_luminance);
    Print("Maximum Content Light Level", video.maximum_content_light_level);
    Print("Maximum Frame-Average Light Level", video.maximum_frame_average_light_level);
    Print("Codec configuration box", video.codec_configuration_box);

    return oss.str();
}

FrameData::~FrameData() {
    for (int i = 0; i < 8; ++i) {
        if (data[i]) {
            delete[] data[i];
            data[i] = nullptr;
        }
    }
}

void FrameData::CopyFrom(const FrameData& other) {
    width = other.width;
    height = other.height;
    format = other.format;
    pts = other.pts;
    timestamp = other.timestamp;
    
    for (int i = 0; i < 8; ++i) {
        linesize[i] = other.linesize[i];
        if (other.data[i] && other.linesize[i] > 0) {
            if (!data[i]) {
                data[i] = new uint8_t[linesize[i] * height];
            }
            std::memcpy(data[i], other.data[i], linesize[i] * height);
        }
    }
}

} // namespace model
} // namespace videoeye
