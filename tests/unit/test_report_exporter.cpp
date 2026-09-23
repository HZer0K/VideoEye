#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <fstream>
#include <iterator>
#include <string>

#include "utils/ReportExporter.h"

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
    // 用花括号: 写成 std::string json(...) 会触发 most vexing parse,
    // 被当成"函数声明", 后面 json.find() 就报"左边必须有类/结构/联合"。
    const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

    // EscapeJSON 会把 \ 变成 \\ —— 文件里落的是两个反斜杠。
    // 用原始字符串字面量，免得 C++ 的转义和 JSON 的转义混在一起看不清。
    EXPECT_NE(json.find(R"(D:\\media\\test.mp4)"), std::string::npos);
}

} // namespace