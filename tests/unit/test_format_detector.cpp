#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "core/analysis/orchestration/FormatDetector.h"

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
              videoeye::FormatDetector::Detect(file.fileName().toStdString()));
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
              videoeye::FormatDetector::Detect(file.fileName().toStdString()));
}

TEST(FormatDetectorTest, detectsMpegPsPackHeader) {
    QTemporaryFile file;
    ASSERT_TRUE(file.open());

    // MPEG-2 Pack Header（14 字节，stuffing=0）
    const QByteArray data = QByteArray::fromHex("000001BA44000400040189C3F800");
    ASSERT_EQ(file.write(data), static_cast<qint64>(data.size()));
    file.close();

    EXPECT_EQ(videoeye::model::ContainerFormat::MPEG_PS,
              videoeye::FormatDetector::Detect(file.fileName().toStdString()));
}

TEST(FormatDetectorTest, detectsMxfPartitionPackKey) {
    QTemporaryFile file;
    ASSERT_TRUE(file.open());

    QByteArray data = QByteArray::fromHex("060E2B34020501010D01020101020100");
    data.append(QByteArray(32, '\0'));
    ASSERT_EQ(file.write(data), static_cast<qint64>(data.size()));
    file.close();

    EXPECT_EQ(videoeye::model::ContainerFormat::MXF, videoeye::FormatDetector::Detect(file.fileName().toStdString()));
}

TEST(FormatDetectorTest, detectsRawAnnexBWithSecondStartCode) {
    QTemporaryFile file;
    ASSERT_TRUE(file.open());

    // 00 00 00 01 + SPS(0x67) ... 再接一个起始码 + IDR(0x65)
    const QByteArray data = QByteArray::fromHex("000000016742001E00000001658884");
    ASSERT_EQ(file.write(data), static_cast<qint64>(data.size()));
    file.close();

    EXPECT_EQ(videoeye::model::ContainerFormat::RAW_ES,
              videoeye::FormatDetector::Detect(file.fileName().toStdString()));
}

TEST(FormatDetectorTest, fallsBackToExtensionForNewFormats) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const struct {
        const char* name;
        videoeye::model::ContainerFormat expected;
    } cases[] = {
        {"clip.mpg", videoeye::model::ContainerFormat::MPEG_PS},
        {"clip.vob", videoeye::model::ContainerFormat::MPEG_PS},
        {"clip.mxf", videoeye::model::ContainerFormat::MXF},
        {"clip.h264", videoeye::model::ContainerFormat::RAW_ES},
        {"clip.hevc", videoeye::model::ContainerFormat::RAW_ES},
    };

    for (const auto& c : cases) {
        const QString path = dir.filePath(QString::fromLatin1(c.name));
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << c.name; // 空文件：魔数无从判断
        f.close();
        EXPECT_EQ(c.expected, videoeye::FormatDetector::Detect(path.toStdString())) << c.name;
    }
}

} // namespace