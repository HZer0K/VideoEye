// 打开会话的两条判定口径的回归（ui/main_window/open_session.h）。
//
// 这两条判定错了都不会崩，只会静默走错分支：
//   * 分派错了 —— 该走异步的走了同步分支，收尾信号无人认领，界面停在"已打开"的
//     占位状态；该走同步的走了异步，就会干等一个永远不会来的 OpenFinished；
//   * 待收尾登记判错了 —— 过期回调拿着上一个文件的路径去刷界面，
//     用户看到的是"打开了 B，界面显示 A 的结果"。
// 抽出来之前它们住在 MainWindow 的 UI 回调里，测不到。

#include "ui/main_window/open_session.h"

#include <gtest/gtest.h>

namespace {

using videoeye::ui::ArmOpenTicket;
using videoeye::ui::ClassifyOpenTarget;
using videoeye::ui::ClearOpenTicket;
using videoeye::ui::IsOpenTicketArmed;
using videoeye::ui::OpenTargetKind;
using videoeye::ui::OpenTicket;
using videoeye::ui::TakeOpenTicket;

} // namespace

TEST(OpenSessionTests, RawImageSuffixesRouteToRawImage) {
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.yuv")), OpenTargetKind::RawImage);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.nv12")), OpenTargetKind::RawImage);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.rgb")), OpenTargetKind::RawImage);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.bgr")), OpenTargetKind::RawImage);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.yuy2")), OpenTargetKind::RawImage);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.raw")), OpenTargetKind::RawImage);
}

// 扩展名比较必须大小写无关：Windows 资源管理器里 *.YUV 是常态，
// 判不中的话 .YUV 会被当成普通媒体送进 avformat，直接"打开失败"。
TEST(OpenSessionTests, ClassificationIsCaseInsensitive) {
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("frame.YUV")), OpenTargetKind::RawImage);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("clip.PCM")), OpenTargetKind::Pcm);
}

TEST(OpenSessionTests, PcmSuffixRoutesToPcm) {
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("audio.pcm")), OpenTargetKind::Pcm);
}

TEST(OpenSessionTests, EverythingElseRoutesToAsyncMedia) {
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.mp4")), OpenTargetKind::Media);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("a.mkv")), OpenTargetKind::Media);
    // 无扩展名：URL（rtmp://… / 流媒体地址）走通用异步打开，与历史行为一致
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("rtmp://host/app/stream")), OpenTargetKind::Media);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("/tmp/no_extension_file")), OpenTargetKind::Media);
}

// 只有"最后一个点之后"的那一段算扩展名：路径里的点不能被误判。
TEST(OpenSessionTests, DotsInDirectoryNamesAreNotSuffixes) {
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("/my.dir/clip.mp4")), OpenTargetKind::Media);
    EXPECT_EQ(ClassifyOpenTarget(QString::fromUtf8("/clips/raw.data/video.mp4")), OpenTargetKind::Media);
}

TEST(OpenSessionTests, FreshTicketIsNotArmed) {
    OpenTicket ticket;
    EXPECT_FALSE(IsOpenTicketArmed(ticket));
    QString source;
    EXPECT_FALSE(TakeOpenTicket(ticket, &source, nullptr));
}

TEST(OpenSessionTests, ArmAndTakeRoundTrips) {
    OpenTicket ticket;
    ArmOpenTicket(ticket, QString::fromUtf8("a.mp4"), true);

    ASSERT_TRUE(IsOpenTicketArmed(ticket));
    QString source;
    bool autoplay = false;
    ASSERT_TRUE(TakeOpenTicket(ticket, &source, &autoplay));
    EXPECT_EQ(source, QString::fromUtf8("a.mp4"));
    EXPECT_TRUE(autoplay);
}

// 取走即作废：一次登记只能被消费一次。这是"过期回调"判定的基础 ——
// 第二条 OpenFinished（迟到的旧结果）必须什么都拿不到。
TEST(OpenSessionTests, TakeConsumesTheTicket) {
    OpenTicket ticket;
    ArmOpenTicket(ticket, QString::fromUtf8("a.mp4"), false);

    QString first;
    ASSERT_TRUE(TakeOpenTicket(ticket, &first, nullptr));
    EXPECT_FALSE(IsOpenTicketArmed(ticket));

    QString second = QString::fromUtf8("不该被写到");
    EXPECT_FALSE(TakeOpenTicket(ticket, &second, nullptr));
    EXPECT_EQ(second, QString::fromUtf8("不该被写到")); // 失败时不动出参
}

// 新打开作废上一次登记：这正是"同步分支期间迟到的旧回调"被丢掉的路径。
TEST(OpenSessionTests, ClearDropsAPendingTicket) {
    OpenTicket ticket;
    ArmOpenTicket(ticket, QString::fromUtf8("old.mp4"), true);
    ClearOpenTicket(ticket);

    EXPECT_FALSE(IsOpenTicketArmed(ticket));
    QString source;
    EXPECT_FALSE(TakeOpenTicket(ticket, &source, nullptr));
}

// 空 source 不登记：空串是不合法的打开目标，拿它当"没有待收尾请求"的哨兵。
// 否则同步分支（裸图像 / PCM）清空登记后反而会被判成"已登记"。
TEST(OpenSessionTests, ArmingAnEmptySourceLeavesItUnarmed) {
    OpenTicket ticket;
    ArmOpenTicket(ticket, QString(), true);
    EXPECT_FALSE(IsOpenTicketArmed(ticket));

    // 以及：已登记的票被空 source 覆盖后同样作废
    ArmOpenTicket(ticket, QString::fromUtf8("a.mp4"), true);
    ASSERT_TRUE(IsOpenTicketArmed(ticket));
    ArmOpenTicket(ticket, QString(), false);
    EXPECT_FALSE(IsOpenTicketArmed(ticket));
}

// 控制器在同步分支（Raw / PCM）里不登记，异步回调因此必然被判过期。
// 这条是"打开 B 之后 A 的结果不许刷界面"的组合验证。
TEST(OpenSessionTests, ReopeningWithoutAsyncLeavesNoTicketForLateCallback) {
    OpenTicket ticket;
    // 第一次：通用媒体，登记待收尾
    ArmOpenTicket(ticket, QString::fromUtf8("a.mp4"), true);
    // 第二次：裸图像（同步分支）—— 打开入口先 Clear 再走同步路径，全程不登记
    ClearOpenTicket(ticket);

    QString source;
    EXPECT_FALSE(TakeOpenTicket(ticket, &source, nullptr));
}
