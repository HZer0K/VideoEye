#include "core/qc/QcReportFormat.h"

#include <algorithm>
#include <cctype>

namespace videoeye {
namespace qc {

const char* ToString(QcReportFormat format) {
    switch (format) {
        case QcReportFormat::Json: return "json";
        case QcReportFormat::Csv:  return "csv";
        case QcReportFormat::Html: return "html";
        case QcReportFormat::Text: return "txt";
        case QcReportFormat::Pdf:  return "pdf";
    }
    return "json";
}

std::string QcReportExtension(QcReportFormat format) {
    return std::string(".") + ToString(format);
}

bool ParseQcReportFormat(const std::string& text, QcReportFormat& out) {
    std::string key = text;
    if (!key.empty() && key.front() == '.') key.erase(key.begin());
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (key == "json") { out = QcReportFormat::Json; return true; }
    if (key == "csv")  { out = QcReportFormat::Csv;  return true; }
    if (key == "html" || key == "htm") { out = QcReportFormat::Html; return true; }
    if (key == "txt" || key == "text") { out = QcReportFormat::Text; return true; }
    if (key == "pdf")  { out = QcReportFormat::Pdf;  return true; }
    return false;
}

bool QcReportFormatFromPath(const std::string& path, QcReportFormat& out) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == "json") { out = QcReportFormat::Json; return true; }
    if (ext == "csv")  { out = QcReportFormat::Csv;  return true; }
    if (ext == "html" || ext == "htm") { out = QcReportFormat::Html; return true; }
    if (ext == "txt")  { out = QcReportFormat::Text; return true; }
    if (ext == "pdf")  { out = QcReportFormat::Pdf;  return true; }
    return false;
}

}  // namespace qc
}  // namespace videoeye
