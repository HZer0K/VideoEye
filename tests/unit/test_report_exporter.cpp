#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <fstream>
#include <iterator>
#include <string>

#include "utils/report_exporter.h"

namespace {

TEST(ReportExporterTest, EscapesWindowsPathsInJson) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString out_path = dir.filePath("report.json");
    videoeye::analyzer::StreamStats stats;
    stats.total_packets = 1;

    ASSERT_TRUE(videoeye::utils::ReportExporter::ExportJSON(
        out_path.toStdString(), stats, R"(D:\media\test.mp4)"));

    std::ifstream file(out_path.toStdString());
    ASSERT_TRUE(file.is_open());
    const std::string json(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("D:\\media\\test.mp4"), std::string::npos);
}

} // namespace