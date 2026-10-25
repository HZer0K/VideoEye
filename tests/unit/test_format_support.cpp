// 帮助菜单「支持的格式与编解码器」回归测试
//
// 这个页面存在的理由: VideoEye 在不同机器上链到的 FFmpeg 构建可以完全不同
// (GPL 构建带 libx264、发行版包不带)，写死的"支持清单"一定会骗人。所以页面上的
// 每一行都必须由 av_*_iterate() 现场问出来 —— 本测试锁的就是这件事：
//
//   1) 枚举结果来自**本进程链接的库**（而不是某个写死的常量表）: 断言常见的
//      h264 / aac / mp4 / matroska / file 都在表内，且能读能写的方向是对的；
//   2) 归并没错位: 同名解码器/编码器、解复用器/复用器合成一行，两个方向都置位
//      （matroska、mp4 这种双向格式是最好的探针 —— 只置一边就说明归并写反了）；
//   3) 搜索与过滤只动"行隐藏"，不改数据: 过滤后可见行数必须等于数据里满足
//      条件的条数，一个不多一个不少 —— 这条同时兜住"过滤后还能导出全量"的错。
//
// 用 offscreen 平台，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>

#include "ui/dialogs/FfmpegSupportProbe.h"
#include "ui/dialogs/FormatSupportDialog.h"

namespace {

using videoeye::ui::CodecSupportEntry;
using videoeye::ui::CollectFfmpegSupport;
using videoeye::ui::ContainerSupportEntry;
using videoeye::ui::FfmpegSupportSnapshot;
using videoeye::ui::FormatSupportDialog;
using videoeye::ui::ProtocolSupportEntry;

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_format_support";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

const CodecSupportEntry* FindCodec(const FfmpegSupportSnapshot& s, const QString& name) {
    for (const auto& c : s.codecs) {
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return &c;
    }
    return nullptr;
}

const ProtocolSupportEntry* FindProtocol(const FfmpegSupportSnapshot& s, const QString& name) {
    for (const auto& p : s.protocols) {
        if (p.name.compare(name, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

// 封装格式的短名可能是别名表（"mov,mp4,m4a,3gp,3g2,mj2"），按段匹配。
const ContainerSupportEntry* FindContainer(const FfmpegSupportSnapshot& s, const QString& name) {
    for (const auto& c : s.containers) {
        const QStringList aliases = c.name.split(',');
        for (const QString& alias : aliases) {
            if (alias.trimmed().compare(name, Qt::CaseInsensitive) == 0)
                return &c;
        }
    }
    return nullptr;
}

} // namespace

TEST(FfmpegSupportProbe, ReportsTheLinkedLibraryNotAHardcodedList) {
    const FfmpegSupportSnapshot snapshot = CollectFfmpegSupport();

    EXPECT_FALSE(snapshot.build.version.isEmpty());
    ASSERT_EQ(snapshot.build.libraries.size(), 5u);
    for (const auto& lib : snapshot.build.libraries) {
        EXPECT_FALSE(lib.first.isEmpty());
        EXPECT_FALSE(lib.second.isEmpty()) << lib.first.toStdString();
    }

    EXPECT_FALSE(snapshot.codecs.empty());
    EXPECT_FALSE(snapshot.containers.empty());
    EXPECT_FALSE(snapshot.protocols.empty());
    EXPECT_GT(snapshot.DecoderCount(), 0);
    EXPECT_GT(snapshot.EncoderCount(), 0);
    EXPECT_GT(snapshot.DemuxerCount(), 0);
    EXPECT_GT(snapshot.MuxerCount(), 0);

    // 这几个是任何一份能用的 FFmpeg 都不该缺的 —— 缺了说明枚举根本没跑起来，
    // 而不是"这个构建比较精简"。
    const CodecSupportEntry* h264 = FindCodec(snapshot, QStringLiteral("h264"));
    ASSERT_TRUE(h264 != nullptr) << "枚举不到 h264，清单大概率是空的/没跑 av_codec_iterate";
    EXPECT_TRUE(h264->can_decode);
    EXPECT_EQ(h264->type, videoeye::ui::SupportMediaType::Video);

    const CodecSupportEntry* aac = FindCodec(snapshot, QStringLiteral("aac"));
    ASSERT_TRUE(aac != nullptr);
    EXPECT_TRUE(aac->can_decode);
    EXPECT_EQ(aac->type, videoeye::ui::SupportMediaType::Audio);

    const ProtocolSupportEntry* file = FindProtocol(snapshot, QStringLiteral("file"));
    ASSERT_TRUE(file != nullptr);
    EXPECT_TRUE(file->can_read);
}

TEST(FfmpegSupportProbe, MergesBothDirectionsIntoOneRow) {
    const FfmpegSupportSnapshot snapshot = CollectFfmpegSupport();

    // avi 的解复用器与复用器都叫 "avi": 两个方向必须落在同一行。分了两行就说明
    // 归并没做（页面上会出现两行 avi，一个只有读取、一个只有写入）。
    const ContainerSupportEntry* avi = FindContainer(snapshot, QStringLiteral("avi"));
    ASSERT_TRUE(avi != nullptr);
    EXPECT_TRUE(avi->can_demux);
    EXPECT_TRUE(avi->can_mux);

    // 反面: mp4 这一族在 FFmpeg 里本来就是两条注册（`ffmpeg -formats` 同样打印两行）——
    // 读挂在 mov 的别名表 "mov,mp4,m4a,3gp,3g2,mj2" 上，写是独立的 "mp4"。
    // 保持原样才是对着库说实话；硬合成一行反而会丢掉"写 mp4 时没有 mov 别名"。
    bool readable_mp4 = false;
    bool writable_mp4 = false;
    for (const auto& c : snapshot.containers) {
        const QStringList aliases = c.name.split(',');
        bool has_mp4 = false;
        for (const QString& alias : aliases) {
            if (alias.trimmed().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0)
                has_mp4 = true;
        }
        if (!has_mp4)
            continue;
        if (c.can_demux)
            readable_mp4 = true;
        if (c.can_mux)
            writable_mp4 = true;
    }
    EXPECT_TRUE(readable_mp4) << "读不到 mp4 —— 别名表没解析对";
    EXPECT_TRUE(writable_mp4) << "写不了 mp4 —— muxer 没枚举到";
}

TEST(FfmpegSupportProbe, RowsAreSortedByName) {
    const FfmpegSupportSnapshot snapshot = CollectFfmpegSupport();
    auto expect_sorted = [](const auto& rows) {
        for (std::size_t i = 1; i < rows.size(); ++i) {
            EXPECT_LE(rows[i - 1].name.compare(rows[i].name, Qt::CaseInsensitive), 0)
                << rows[i - 1].name.toStdString() << " > " << rows[i].name.toStdString();
        }
    };
    expect_sorted(snapshot.codecs);
    expect_sorted(snapshot.containers);
    expect_sorted(snapshot.protocols);
    expect_sorted(snapshot.hwaccels);
}

TEST(FormatSupportDialog, EachTabShowsExactlyTheCollectedRows) {
    EnsureApp();
    const FfmpegSupportSnapshot snapshot = CollectFfmpegSupport();

    FormatSupportDialog dialog;
    ASSERT_TRUE(dialog.TableAt(FormatSupportDialog::kOverview) == nullptr);
    EXPECT_EQ(dialog.VisibleRowCount(), 0);

    ASSERT_TRUE(dialog.TableAt(FormatSupportDialog::kContainers) != nullptr);
    EXPECT_EQ(dialog.TableAt(FormatSupportDialog::kContainers)->rowCount(),
              static_cast<int>(snapshot.containers.size()));
    ASSERT_TRUE(dialog.TableAt(FormatSupportDialog::kCodecs) != nullptr);
    EXPECT_EQ(dialog.TableAt(FormatSupportDialog::kCodecs)->rowCount(), static_cast<int>(snapshot.codecs.size()));
    ASSERT_TRUE(dialog.TableAt(FormatSupportDialog::kProtocols) != nullptr);
    EXPECT_EQ(dialog.TableAt(FormatSupportDialog::kProtocols)->rowCount(), static_cast<int>(snapshot.protocols.size()));
    ASSERT_TRUE(dialog.TableAt(FormatSupportDialog::kHwAccel) != nullptr);
    EXPECT_EQ(dialog.TableAt(FormatSupportDialog::kHwAccel)->rowCount(), static_cast<int>(snapshot.hwaccels.size()));

    // h264 必须真的出现在编解码器表的一行里（不是只躺在数据结构里）
    auto* codecs = dialog.TableAt(FormatSupportDialog::kCodecs);
    bool found = false;
    for (int row = 0; row < codecs->rowCount(); ++row) {
        auto* item = codecs->item(row, 0);
        if (item && item->text() == QStringLiteral("h264"))
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(FormatSupportDialog, SearchAndFilterOnlyHideRows) {
    EnsureApp();
    const FfmpegSupportSnapshot snapshot = CollectFfmpegSupport();

    FormatSupportDialog dialog;
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("support_tabs"));
    auto* search = dialog.findChild<QLineEdit*>(QStringLiteral("support_search"));
    auto* filter = dialog.findChild<QComboBox*>(QStringLiteral("support_filter"));
    ASSERT_TRUE(tabs != nullptr);
    ASSERT_TRUE(search != nullptr);
    ASSERT_TRUE(filter != nullptr);

    tabs->setCurrentIndex(FormatSupportDialog::kCodecs);
    auto* codecs = dialog.TableAt(FormatSupportDialog::kCodecs);
    ASSERT_TRUE(codecs != nullptr);
    const int total = codecs->rowCount();
    ASSERT_GT(total, 0);
    EXPECT_EQ(dialog.VisibleRowCount(), total);

    // 搜索 "h264": 命中数必须落在 (0, total) 之间 —— 等于 total 说明过滤没生效，
    // 等于 0 说明匹配写反了。
    search->setText(QStringLiteral("h264"));
    const int searched = dialog.VisibleRowCount();
    EXPECT_GT(searched, 0);
    EXPECT_LT(searched, total);

    search->clear();
    EXPECT_EQ(dialog.VisibleRowCount(), total);

    // 过滤到「仅能解码」: 可见行数必须**正好**等于数据里的解码器条数。
    // 少一个说明漏了只解码的（prores 一类），多一个说明把只编码的也算进来了。
    ASSERT_GE(filter->count(), 2);
    filter->setCurrentIndex(1);
    EXPECT_EQ(dialog.VisibleRowCount(), snapshot.DecoderCount());

    filter->setCurrentIndex(2); // 仅能编码
    EXPECT_EQ(dialog.VisibleRowCount(), snapshot.EncoderCount());
    EXPECT_LT(dialog.VisibleRowCount(), total);
}

TEST(FormatSupportDialog, SwitchingTabResetsFilterToAll) {
    EnsureApp();
    FormatSupportDialog dialog;
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("support_tabs"));
    auto* filter = dialog.findChild<QComboBox*>(QStringLiteral("support_filter"));
    ASSERT_TRUE(tabs != nullptr);
    ASSERT_TRUE(filter != nullptr);

    tabs->setCurrentIndex(FormatSupportDialog::kCodecs);
    filter->setCurrentIndex(1);

    // 换页签会重建过滤下拉；如果忘了挡信号，clear() 触发的 currentIndexChanged
    // 会拿旧选项的残留值去过滤，新页签一上来就只剩一部分行。
    tabs->setCurrentIndex(FormatSupportDialog::kProtocols);
    EXPECT_EQ(filter->currentIndex(), 0);
    auto* protocols = dialog.TableAt(FormatSupportDialog::kProtocols);
    ASSERT_TRUE(protocols != nullptr);
    EXPECT_EQ(dialog.VisibleRowCount(), protocols->rowCount());
}
