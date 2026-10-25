#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

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
        // Matroska / MPEG-TS 家族的"别名扩展名"：QC 侧的扩展名一致性表早就认它们，
        // 解析层以前不认识 → 同一个扩展名在两处得出不同结论。
        {"clip.mka", videoeye::model::ContainerFormat::MKV},
        {"clip.mk3d", videoeye::model::ContainerFormat::MKV},
        {"clip.m2ts", videoeye::model::ContainerFormat::MPEG_TS},
        {"clip.tts", videoeye::model::ContainerFormat::MPEG_TS},
        {"clip.wmv", videoeye::model::ContainerFormat::ASF},
        {"clip.ogv", videoeye::model::ContainerFormat::OGG},
        {"clip.av1", videoeye::model::ContainerFormat::RAW_ES},
        {"clip.m3u8", videoeye::model::ContainerFormat::HLS},
        {"clip.mpd", videoeye::model::ContainerFormat::DASH},
    };

    for (const auto& c : cases) {
        const QString path = dir.filePath(QString::fromLatin1(c.name));
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << c.name; // 空文件：魔数无从判断
        f.close();
        EXPECT_EQ(c.expected, videoeye::FormatDetector::Detect(path.toStdString())) << c.name;
    }
}

namespace {

// 写一个临时文件并返回路径。用 QTemporaryFile 保持"没有扩展名"，
// 确保走的是魔数分支而不是扩展名兜底。
QString WriteMagicSample(QTemporaryFile& file, const QByteArray& data) {
    if (!file.open()) {
        return QString();
    }
    if (file.write(data) != static_cast<qint64>(data.size())) {
        return QString();
    }
    file.close();
    return file.fileName();
}

QByteArray PadTo(QByteArray data, int size) {
    if (data.size() < size) {
        data.append(QByteArray(size - data.size(), '\0'));
    }
    return data;
}

} // namespace

// ---------------------------------------------------------------------------
// ISOBMFF 家族：ftyp / styp / moof 与 MP4-MOV 的区分
// ---------------------------------------------------------------------------

TEST(FormatDetectorMagicTest, detectsIsobmffBrands) {
    struct Case {
        const char* hex;
        videoeye::model::ContainerFormat expected;
        const char* note;
    };
    // ftyp: size(4) + "ftyp" + major_brand(4) + minor_version(4) + compatible_brands
    const Case cases[] = {
        {"000000206674797069736F6D0000020069736F6D69736F32617663316D703431", videoeye::model::ContainerFormat::MP4,
         "major_brand=isom"},
        {"0000001466747970717420200000020071742020", videoeye::model::ContainerFormat::MOV, "major_brand=qt"},
        // major_brand 是 mp42，但 compatible_brands 里带 "qt  " —— 这是 QuickTime
        // 工具链写出来的典型文件，以前只看 major_brand 会判成 MP4。
        {"0000001C667479706D703432000000006D70343269736F6D71742020", videoeye::model::ContainerFormat::MOV,
         "compatible_brands 含 qt"},
        // 以前留了个 `brand == "mdat"` 的死分支（major_brand 不可能是 mdat），
        // 这个用例保证它真的没了：major_brand=mdat 就是一个普通的 MP4。
        {"00000014667479706D646174000000006D646174", videoeye::model::ContainerFormat::MP4,
         "major_brand=mdat 仍是 MP4"},
        // fMP4 分片：首盒是 styp（DASH）或 moof（无 styp 的分片）
        {"00000014737479706D736468000000006D736468", videoeye::model::ContainerFormat::MP4, "styp 分片"},
        {"000000106D6F6F6600000000", videoeye::model::ContainerFormat::MP4, "moof 分片"},
    };

    for (const Case& c : cases) {
        QTemporaryFile file;
        const QString path = WriteMagicSample(file, PadTo(QByteArray::fromHex(c.hex), 64));
        ASSERT_FALSE(path.isEmpty()) << c.note;
        EXPECT_EQ(c.expected, videoeye::FormatDetector::Detect(path.toStdString())) << c.note;
    }
}

// ---------------------------------------------------------------------------
// EBML：按 DocType 元素（0x4282）区分 MKV / WebM
// ---------------------------------------------------------------------------

TEST(FormatDetectorMagicTest, detectsEbmlDocType) {
    const struct {
        QByteArray data;
        videoeye::model::ContainerFormat expected;
        const char* note;
    } cases[] = {
        {QByteArray::fromHex("1A45DFA3") + QByteArray::fromHex("428284") + QByteArray("webm"),
         videoeye::model::ContainerFormat::WebM, "DocType=webm"},
        {QByteArray::fromHex("1A45DFA3") + QByteArray::fromHex("428288") + QByteArray("matroska"),
         videoeye::model::ContainerFormat::MKV, "DocType=matroska"},
        // 关键回归：Matroska 文件里出现 "webm" 字样（MuxingApp 常写成
        // "libwebm-0.2.1"，部分标签也带这串）。以前是前 1 KiB 裸搜子串，
        // 这类文件会被判成 WebM；按 DocType 读就不会。
        {QByteArray::fromHex("1A45DFA3") + QByteArray::fromHex("428288") + QByteArray("matroska") +
             QByteArray("libwebm-0.2.1"),
         videoeye::model::ContainerFormat::MKV, "matroska 内含 webm 子串"},
    };

    for (const auto& c : cases) {
        QTemporaryFile file;
        const QString path = WriteMagicSample(file, PadTo(c.data, 64));
        ASSERT_FALSE(path.isEmpty()) << c.note;
        EXPECT_EQ(c.expected, videoeye::FormatDetector::Detect(path.toStdString())) << c.note;
    }
}

// ---------------------------------------------------------------------------
// 其余强魔数容器：AVI / FLV / ASF / OGG / MPEG-PS / MXF
// ---------------------------------------------------------------------------

TEST(FormatDetectorMagicTest, detectsRemainingStrongSignatures) {
    const struct {
        const char* hex;
        videoeye::model::ContainerFormat expected;
        const char* note;
    } cases[] = {
        {"52494646E0A5000041564920", videoeye::model::ContainerFormat::AVI, "RIFF....AVI "},
        {"464C56010500000009", videoeye::model::ContainerFormat::FLV, "FLV"},
        {"3026B2758E66CF11A6D900AA0062CE6C", videoeye::model::ContainerFormat::ASF, "ASF Header GUID"},
        {"4F67675300020000", videoeye::model::ContainerFormat::OGG, "OggS"},
        {"000001BA44000400040189C3F800", videoeye::model::ContainerFormat::MPEG_PS, "PS pack header"},
        {"060E2B34020501010D01020101020100", videoeye::model::ContainerFormat::MXF, "MXF partition pack key"},
    };

    for (const auto& c : cases) {
        QTemporaryFile file;
        const QString path = WriteMagicSample(file, PadTo(QByteArray::fromHex(c.hex), 64));
        ASSERT_FALSE(path.isEmpty()) << c.note;
        EXPECT_EQ(c.expected, videoeye::FormatDetector::Detect(path.toStdString())) << c.note;
    }
}

// ---------------------------------------------------------------------------
// MPEG-TS：三种包长
// ---------------------------------------------------------------------------

TEST(FormatDetectorMagicTest, detectsAllMpegTsPacketSizes) {
    struct Case {
        int packet_size;
        int sync_offset;
        const char* note;
    };
    // 188=标准 TS；192=M2TS，同步字节在每包第 5 字节（偏移 4）；
    // 204=RS(204,188) 纠错包，同步字节仍从 0 开始。
    const Case cases[] = {{188, 0, "188 字节包"}, {192, 4, "192 字节包 (M2TS)"}, {204, 0, "204 字节包 (RS)"}};

    for (const Case& c : cases) {
        QByteArray data(c.packet_size * 3, '\0');
        for (int k = 0; k < 3; ++k) {
            data[c.sync_offset + k * c.packet_size] = static_cast<char>(0x47);
        }
        QTemporaryFile file;
        const QString path = WriteMagicSample(file, data);
        ASSERT_FALSE(path.isEmpty()) << c.note;
        EXPECT_EQ(videoeye::model::ContainerFormat::MPEG_TS, videoeye::FormatDetector::Detect(path.toStdString()))
            << c.note;
    }
}

// ---------------------------------------------------------------------------
// 判定顺序：强魔数必须压过 TS 的弱魔数
// ---------------------------------------------------------------------------

// TS 的判据只是"单字节 0x47 以固定步长重复"，是整个探测器里唯一的弱魔数。
// 它曾经排在 OGG / ASF / PS / MXF 之前，而它会在前 1 KiB 的所有 offset 上试
// （约 564 个候选位置），单个文件被误判成 TS 的概率约 0.86%。
// 这两个用例构造"强签名 + 恰好落在 TS 步长上的 0x47"，锁死新顺序。
TEST(FormatDetectorOrderTest, strongSignaturesWinOverTsSyncPattern) {
    QTemporaryFile ogg_file;
    {
        QByteArray data(188 * 3, '\0');
        data.replace(0, 4, "OggS");
        data[10] = static_cast<char>(0x47);
        data[10 + 188] = static_cast<char>(0x47);
        data[10 + 376] = static_cast<char>(0x47);
        const QString path = WriteMagicSample(ogg_file, data);
        ASSERT_FALSE(path.isEmpty());
        EXPECT_EQ(videoeye::model::ContainerFormat::OGG, videoeye::FormatDetector::Detect(path.toStdString()));
    }

    QTemporaryFile asf_file;
    {
        QByteArray data(188 * 3, '\0');
        data.replace(0, 8, QByteArray::fromHex("3026B2758E66CF11"));
        data[100] = static_cast<char>(0x47);
        data[288] = static_cast<char>(0x47);
        data[476] = static_cast<char>(0x47);
        const QString path = WriteMagicSample(asf_file, data);
        ASSERT_FALSE(path.isEmpty());
        EXPECT_EQ(videoeye::model::ContainerFormat::ASF, videoeye::FormatDetector::Detect(path.toStdString()));
    }
}

// 反方向也要成立：真正的 TS 没有任何多字节签名，仍然要能认出来。
TEST(FormatDetectorOrderTest, realTsStillDetectedAfterReorder) {
    QByteArray data(188 * 3, '\0');
    for (int k = 0; k < 3; ++k) {
        data[k * 188] = static_cast<char>(0x47);
    }
    QTemporaryFile file;
    const QString path = WriteMagicSample(file, data);
    ASSERT_FALSE(path.isEmpty());
    EXPECT_EQ(videoeye::model::ContainerFormat::MPEG_TS, videoeye::FormatDetector::Detect(path.toStdString()));
}

// ---------------------------------------------------------------------------
// 流媒体清单：内容嗅探（以前只能靠扩展名）
// ---------------------------------------------------------------------------

TEST(FormatDetectorMagicTest, sniffsStreamingManifestsByContent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const struct {
        const char* name; // 故意不带 .m3u8/.mpd：CDN 上叫 index / playlist 很常见
        QByteArray content;
        videoeye::model::ContainerFormat expected;
        const char* note;
    } cases[] = {
        {"playlist", QByteArray("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n"),
         videoeye::model::ContainerFormat::HLS, "无扩展名的 HLS"},
        {"index", QByteArray("\xEF\xBB\xBF#EXTM3U\r\n#EXT-X-VERSION:3\r\n"), videoeye::model::ContainerFormat::HLS,
         "带 UTF-8 BOM 的 HLS"},
        {"manifest", QByteArray("  \n#EXTM3U\n"), videoeye::model::ContainerFormat::HLS, "前导空白的 HLS"},
        {"stream",
         QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\">\n</MPD>\n"),
         videoeye::model::ContainerFormat::DASH, "带 XML 声明的 DASH"},
        {"dash-root", QByteArray("<MPD mediaPresentationDuration=\"PT10S\"></MPD>"),
         videoeye::model::ContainerFormat::DASH, "根元素即 MPD"},
    };

    for (const auto& c : cases) {
        const QString path = dir.filePath(QString::fromLatin1(c.name));
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << c.name;
        ASSERT_EQ(f.write(c.content), static_cast<qint64>(c.content.size()));
        f.close();
        EXPECT_EQ(c.expected, videoeye::FormatDetector::Detect(path.toStdString())) << c.note;
    }
}

// 嗅探必须有闸：二进制文件里恰好出现 "<MPD" 不能判成清单。
TEST(FormatDetectorMagicTest, doesNotSniffManifestInsideBinary) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // 前 4 字节是 00 00 00 10（不是任何强签名），中间夹 NUL，再拼 <MPD
    QByteArray data = QByteArray::fromHex("00000010") + QByteArray("\0\0<MPD\0rest", 10);
    data = PadTo(data, 64);
    const QString path = dir.filePath(QStringLiteral("binary"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), static_cast<qint64>(data.size()));
    f.close();

    EXPECT_EQ(videoeye::model::ContainerFormat::Unknown, videoeye::FormatDetector::Detect(path.toStdString()));
}

// ---------------------------------------------------------------------------
// AV1 裸流（OBU 流）：不受 Annex-B 起始码约束
// ---------------------------------------------------------------------------

TEST(FormatDetectorMagicTest, detectsRawAv1ObuStream) {
    // temporal delimiter OBU：header 0x12，载荷长度 0（0x00）；随后是
    // sequence header OBU（type=1 → 0x0A）。
    const QByteArray data = QByteArray::fromHex("12000A0A0000000010203040");
    QTemporaryFile file;
    const QString path = WriteMagicSample(file, PadTo(data, 32));
    ASSERT_FALSE(path.isEmpty());
    EXPECT_EQ(videoeye::model::ContainerFormat::RAW_ES, videoeye::FormatDetector::Detect(path.toStdString()));
}

// ---------------------------------------------------------------------------
// 扩展名清单是唯一事实源：三张表必须自洽
// ---------------------------------------------------------------------------

TEST(FormatDetectorExtensionsTest, tablesAreConsistent) {
    const auto& all = videoeye::FormatDetector::SupportedExtensions();
    const auto& media = videoeye::FormatDetector::MediaExtensions();
    const auto& manifest = videoeye::FormatDetector::ManifestExtensions();

    EXPECT_FALSE(all.empty());
    EXPECT_FALSE(media.empty());
    EXPECT_FALSE(manifest.empty());

    // 无重复
    std::set<std::string> all_set(all.begin(), all.end());
    EXPECT_EQ(all_set.size(), all.size());
    std::set<std::string> media_set(media.begin(), media.end());
    EXPECT_EQ(media_set.size(), media.size());
    std::set<std::string> manifest_set(manifest.begin(), manifest.end());
    EXPECT_EQ(manifest_set.size(), manifest.size());

    // 两张分组表正好是全集的一个划分
    std::set<std::string> union_set = media_set;
    union_set.insert(manifest_set.begin(), manifest_set.end());
    EXPECT_EQ(union_set, all_set);

    // 每个扩展名都必须能由 DetectByExtension 还原成"表里登记的那个格式"，
    // 且清单扩展名不能混进媒体分组。
    for (const std::string& ext : all) {
        const auto fmt = videoeye::FormatDetector::DetectByExtension("clip." + ext);
        ASSERT_NE(videoeye::model::ContainerFormat::Unknown, fmt) << ext;
        const bool is_manifest =
            (fmt == videoeye::model::ContainerFormat::HLS || fmt == videoeye::model::ContainerFormat::DASH);
        EXPECT_EQ(is_manifest, manifest_set.count(ext) > 0) << ext;
    }
}

TEST(FormatDetectorExtensionsTest, coversEveryDocumentedFormat) {
    // 这张断言表就是 README「容器结构分析」那一行的代码版：新增格式时如果只改了
    // DetectByMagic 却忘了登记扩展名，这里会红。
    const auto& all = videoeye::FormatDetector::SupportedExtensions();
    const std::set<std::string> exts(all.begin(), all.end());
    const char* required[] = {"mp4",  "mov", "mkv", "webm", "avi",  "flv",  "ts",  "m2ts", "asf",  "wmv", "ogg",
                              "opus", "mpg", "vob", "mxf",  "h264", "h265", "av1", "obu",  "m3u8", "mpd"};
    for (const char* ext : required) {
        EXPECT_TRUE(exts.count(ext) > 0) << "缺少扩展名: " << ext;
    }
}

} // namespace