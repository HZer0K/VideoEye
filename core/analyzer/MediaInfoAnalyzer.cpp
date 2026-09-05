#include "MediaInfoAnalyzer.h"

#ifdef HAVE_MEDIAINFO
#include "MediaInfo/MediaInfo.h"
#include <QDebug>

namespace videoeye {
namespace analyzer {

// MediaInfoLib 使用 UNICODE 编译时，MediaInfoLib::String = std::wstring
// 以下辅助函数处理字符串转换
static inline MediaInfoLib::String ToMediaInfoString(const QString& qs) {
    std::wstring ws = qs.toStdWString();
    return MediaInfoLib::String(ws.begin(), ws.end());
}

static inline QString FromMediaInfoString(const MediaInfoLib::String& mis) {
    return QString::fromStdWString(std::wstring(mis.begin(), mis.end()));
}

MediaInfoAnalyzer::MediaInfoAnalyzer()
    : mi_(new MediaInfoLib::MediaInfo())
    , opened_(false) {
}

MediaInfoAnalyzer::~MediaInfoAnalyzer() {
    if (mi_) {
        mi_->Close();
        delete mi_;
        mi_ = nullptr;
    }
}

bool MediaInfoAnalyzer::Open(const QString& filePath) {
    if (!mi_ || filePath.isEmpty()) {
        return false;
    }

    Close();

    auto path = ToMediaInfoString(filePath);
    size_t result = mi_->Open(path);

    if (result == 0) {
        qWarning() << "MediaInfoAnalyzer: 无法打开文件" << filePath;
        return false;
    }

    opened_ = true;
    return true;
}

QString MediaInfoAnalyzer::GetCompleteInfo() const {
    if (!opened_ || !mi_) {
        return QString();
    }

    MediaInfoLib::String info = mi_->Inform();
    return FromMediaInfoString(info);
}

QString MediaInfoAnalyzer::GetParameter(int streamKind, int streamNumber, const QString& parameter) const {
    if (!opened_ || !mi_) {
        return QString();
    }

    auto result = mi_->Get(
        static_cast<MediaInfoLib::stream_t>(streamKind),
        static_cast<size_t>(streamNumber),
        ToMediaInfoString(parameter),
        MediaInfoLib::Info_Text,
        MediaInfoLib::Info_Name);

    if (result.empty()) {
        return QString();
    }

    return FromMediaInfoString(result);
}

int MediaInfoAnalyzer::GetStreamCount(int streamKind) const {
    if (!opened_ || !mi_) {
        return 0;
    }

    return static_cast<int>(
        mi_->Count_Get(static_cast<MediaInfoLib::stream_t>(streamKind)));
}

void MediaInfoAnalyzer::Close() {
    if (mi_) {
        mi_->Close();
    }
    opened_ = false;
}

bool MediaInfoAnalyzer::IsReady() const {
    return opened_ && mi_ != nullptr;
}

} // namespace analyzer
} // namespace videoeye

#else

namespace videoeye {
namespace analyzer {

MediaInfoAnalyzer::MediaInfoAnalyzer()
    : mi_(nullptr), opened_(false) {
}

MediaInfoAnalyzer::~MediaInfoAnalyzer() = default;

bool MediaInfoAnalyzer::Open(const QString& filePath) {
    Q_UNUSED(filePath);
    opened_ = false;
    return false;
}

QString MediaInfoAnalyzer::GetCompleteInfo() const {
    return QString();
}

QString MediaInfoAnalyzer::GetParameter(int streamKind, int streamNumber, const QString& parameter) const {
    Q_UNUSED(streamKind);
    Q_UNUSED(streamNumber);
    Q_UNUSED(parameter);
    return QString();
}

int MediaInfoAnalyzer::GetStreamCount(int streamKind) const {
    Q_UNUSED(streamKind);
    return 0;
}

void MediaInfoAnalyzer::Close() {
    opened_ = false;
}

bool MediaInfoAnalyzer::IsReady() const {
    return false;
}

} // namespace analyzer
} // namespace videoeye

#endif // HAVE_MEDIAINFO
