#include <gtest/gtest.h>

#include <QByteArray>
#include <QTemporaryFile>

#include "core/analyzer/FormatDetector.h"

namespace {

TEST(FormatDetectorTest, detectsMpegTsSyncPattern) {
    QTemporaryFile file;
    ASSERT_TRUE(file.open());

    QByteArray data(188 * 3, '\0');
    data[0] = static_cast<char>(0x47);
    data[188] = static_cast<char>(0x47);
    data[376] = static_cast<char>(0x47);
    ASSERT_EQ(file.write(data), static_cast<qint64>(data.size()));
    file.close();

    EXPECT_EQ(videoeye::model::ContainerFormat::MPEG_TS,
              videoeye::analyzer::FormatDetector::Detect(file.fileName()));
}

TEST(FormatDetectorTest, detectsMpegTsWithLeadingOffset) {
    QTemporaryFile file;
    ASSERT_TRUE(file.open());

    QByteArray data(7 + 188 * 3, '\0');
    data[7] = static_cast<char>(0x47);
    data[7 + 188] = static_cast<char>(0x47);
    data[7 + 376] = static_cast<char>(0x47);
    ASSERT_EQ(file.write(data), static_cast<qint64>(data.size()));
    file.close();

    EXPECT_EQ(videoeye::model::ContainerFormat::MPEG_TS,
              videoeye::analyzer::FormatDetector::Detect(file.fileName()));
}

} // namespace