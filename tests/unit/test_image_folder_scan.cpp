// 目录内同族图片枚举（图片翻页 / 图片序列的共同底座）的单元测试。
//
// 盯三件事：
//   1. 排序口径：抽帧输出几乎都是 frame_1..frame_120 这种命名，逐字节排序会把 10
//      排在 2 前面 —— "下一张"直接跳到第 10 帧，看似能翻但顺序是错的。自然序比较
//      单独钉住（含前导零、大小写、前缀三种边界）。
//   2. 成员判定：只用扩展名决定"算不算同族图片"，视频/文本/无扩展名一律不进列表；
//      当前文件若不在列表里，必须 contains_current=false，而不是给一个看似合理的下标。
//   3. 与"能不能打开"的一致性：kImageFileExtensions 里每个后缀都必须出现在
//      FormatDetector 的扩展名表里 —— 否则序列会列出打开对话框里根本选不中的文件。
//
// 目录用临时目录现场创建，不依赖仓库里任何样本。

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "core/analysis/orchestration/FormatDetector.h"
#include "core/domain/io/ImageFolderScan.h"
#include "core/domain/io/Utf8Path.h"
#include "core/domain/model/ImageDemuxer.h"

namespace fs = std::filesystem;

using videoeye::ToNativePath;
using videoeye::ToUtf8String;
using videoeye::io::ImageFolderListing;
using videoeye::io::ListImagesInFolder;
using videoeye::io::NaturalFileNameLess;
using videoeye::io::ScanImageFolder;

namespace {

// 每个用例用独立目录（同名目录先清空），互不干扰，也不必依赖测试执行顺序。
fs::path FreshFolder(const std::string& name) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("videoeye_image_folder_" + name);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void Touch(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "x";
}

std::vector<std::string> BaseNames(const ImageFolderListing& listing) {
    std::vector<std::string> names;
    names.reserve(listing.files.size());
    for (const std::string& path : listing.files) {
        names.push_back(ToUtf8String(ToNativePath(path).filename()));
    }
    return names;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 自然序
// ---------------------------------------------------------------------------
TEST(ImageFolderScanTests, NaturalOrderComparesDigitRunsAsNumbers) {
    EXPECT_TRUE(NaturalFileNameLess("frame_2.png", "frame_10.png"));
    EXPECT_FALSE(NaturalFileNameLess("frame_10.png", "frame_2.png"));
    // 前导零不改变数值大小：数值相等时两边互不为"小"（并列，排序保持原相对次序）。
    EXPECT_TRUE(NaturalFileNameLess("frame_007.png", "frame_8.png"));
    EXPECT_FALSE(NaturalFileNameLess("frame_07.png", "frame_7.png"));
    EXPECT_FALSE(NaturalFileNameLess("frame_7.png", "frame_07.png"));
    // 位数不同就是数值不同：9 < 10，即使字符串上 "9" > "1"。
    EXPECT_TRUE(NaturalFileNameLess("a9.png", "a10.png"));
}

TEST(ImageFolderScanTests, NaturalOrderIsStableForCaseAndPrefix) {
    // 大小写不敏感，避免 Windows 上 A.png / a.png 被拆到列表两端。
    EXPECT_FALSE(NaturalFileNameLess("A.png", "a.png"));
    EXPECT_FALSE(NaturalFileNameLess("a.png", "A.png"));
    // 一方是另一方前缀：短的在前。
    EXPECT_TRUE(NaturalFileNameLess("a.png", "a_1.png"));
    EXPECT_FALSE(NaturalFileNameLess("a_1.png", "a.png"));
}

// ---------------------------------------------------------------------------
// 2. 兄弟枚举
// ---------------------------------------------------------------------------
TEST(ImageFolderScanTests, ScanListsOnlyImageSiblingsInNaturalOrder) {
    const fs::path dir = FreshFolder("order");
    Touch(dir / "frame_1.png");
    Touch(dir / "frame_2.png");
    Touch(dir / "frame_10.png");
    Touch(dir / "notes.txt");
    Touch(dir / "clip.mp4");
    std::error_code ec;
    fs::create_directories(dir / "sub", ec);
    Touch(dir / "sub" / "frame_3.png");

    const ImageFolderListing listing = ScanImageFolder(ToUtf8String(dir / "frame_2.png"));

    ASSERT_TRUE(listing.contains_current);
    EXPECT_EQ(listing.files.size(), 3u); // 文本 / 视频 / 子目录都不算
    const std::vector<std::string> names = BaseNames(listing);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "frame_1.png");
    EXPECT_EQ(names[1], "frame_2.png");
    EXPECT_EQ(names[2], "frame_10.png");
    EXPECT_EQ(listing.index, 1u);
    EXPECT_TRUE(listing.has_previous());
    EXPECT_TRUE(listing.has_next());
}

TEST(ImageFolderScanTests, ScanReportsBoundariesAtBothEnds) {
    const fs::path dir = FreshFolder("bounds");
    Touch(dir / "a.png");
    Touch(dir / "b.png");

    const ImageFolderListing first = ScanImageFolder(ToUtf8String(dir / "a.png"));
    ASSERT_TRUE(first.contains_current);
    EXPECT_FALSE(first.has_previous());
    EXPECT_TRUE(first.has_next());

    const ImageFolderListing last = ScanImageFolder(ToUtf8String(dir / "b.png"));
    ASSERT_TRUE(last.contains_current);
    EXPECT_TRUE(last.has_previous());
    EXPECT_FALSE(last.has_next());
}

TEST(ImageFolderScanTests, ScanRejectsNonImageCurrentFile) {
    const fs::path dir = FreshFolder("nonimage");
    Touch(dir / "a.png");
    Touch(dir / "clip.mp4");

    // 视频不参与"上一张/下一张"：混进来会让一次点击从看图变成播视频。
    const ImageFolderListing listing = ScanImageFolder(ToUtf8String(dir / "clip.mp4"));
    EXPECT_FALSE(listing.contains_current);
    EXPECT_TRUE(listing.files.empty());
    EXPECT_FALSE(listing.has_previous());
    EXPECT_FALSE(listing.has_next());
}

TEST(ImageFolderScanTests, ScanOnMissingFileReportsNothing) {
    const fs::path dir = FreshFolder("missing");
    Touch(dir / "a.png");

    const ImageFolderListing listing = ScanImageFolder(ToUtf8String(dir / "ghost.png"));
    // 目录读得到、文件不在里面：列表可以给（供调用方展示），但必须声明"当前不在其中"。
    EXPECT_FALSE(listing.contains_current);
    EXPECT_EQ(listing.files.size(), 1u);
    EXPECT_FALSE(listing.has_next());

    EXPECT_FALSE(ScanImageFolder(std::string()).contains_current);
    EXPECT_TRUE(ScanImageFolder(std::string()).files.empty());
}

TEST(ImageFolderScanTests, ScanHandlesNonAsciiFileNames) {
    const fs::path dir = FreshFolder("utf8");
    Touch(dir / ToNativePath("照片_2.png"));
    Touch(dir / ToNativePath("照片_10.png"));

    const ImageFolderListing listing = ScanImageFolder(ToUtf8String(dir / ToNativePath("照片_2.png")));
    ASSERT_TRUE(listing.contains_current);
    ASSERT_EQ(listing.files.size(), 2u);
    const std::vector<std::string> names = BaseNames(listing);
    EXPECT_EQ(names[0], "照片_2.png");
    EXPECT_EQ(names[1], "照片_10.png");
}

// ---------------------------------------------------------------------------
// 3. 整目录枚举（图片序列）
// ---------------------------------------------------------------------------
TEST(ImageFolderScanTests, ListImagesInFolderSkipsNonImagesAndIsEmptyForFiles) {
    const fs::path dir = FreshFolder("list");
    Touch(dir / "f_1.jpg");
    Touch(dir / "f_2.jpg");
    Touch(dir / "readme.md");
    std::error_code ec;
    fs::create_directories(dir / "nested", ec);
    Touch(dir / "nested" / "f_3.jpg");

    const std::vector<std::string> files = ListImagesInFolder(ToUtf8String(dir));
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(ToUtf8String(ToNativePath(files[0]).filename()), "f_1.jpg");
    EXPECT_EQ(ToUtf8String(ToNativePath(files[1]).filename()), "f_2.jpg");

    // 传进来的是一个文件（不是目录）：明确返回空，不要"猜"它所在目录。
    EXPECT_TRUE(ListImagesInFolder(ToUtf8String(dir / "f_1.jpg")).empty());
    EXPECT_TRUE(ListImagesInFolder(std::string()).empty());
}

// ---------------------------------------------------------------------------
// 4. 后缀清单与"能不能打开"必须一致
// ---------------------------------------------------------------------------
TEST(ImageFolderScanTests, EveryImageFileExtensionIsOpenableThroughFormatDetector) {
    const std::vector<std::string>& openable = videoeye::FormatDetector::MediaExtensions();
    for (const std::string_view ext : videoeye::model::kImageFileExtensions) {
        const std::string entry(ext);
        // 出现在"同族图片"里却不在打开对话框的过滤器里 = 序列会列出选不中的文件。
        EXPECT_NE(std::find(openable.begin(), openable.end(), entry), openable.end())
            << "图片后缀未登记到 FormatDetector: " << entry;
    }
}

TEST(ImageFolderScanTests, ImageFileExtensionMatcherAcceptsDotAndAnyCase) {
    using videoeye::model::IsImageFileExtension;
    EXPECT_TRUE(IsImageFileExtension("png"));
    EXPECT_TRUE(IsImageFileExtension(".PNG"));
    EXPECT_TRUE(IsImageFileExtension("JpEg"));
    EXPECT_TRUE(IsImageFileExtension(".heic"));
    EXPECT_FALSE(IsImageFileExtension("mp4"));
    EXPECT_FALSE(IsImageFileExtension("ico")); // 有解复用器但没有解码器，故意不列
    EXPECT_FALSE(IsImageFileExtension(""));
    EXPECT_FALSE(IsImageFileExtension("."));
}
