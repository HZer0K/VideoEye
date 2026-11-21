#pragma once

// 命令行历史（输入框里按 ↑ / ↓ 翻）。纯数据，不碰 QSettings ——
// 存盘由页面负责，这样"什么时候记住草稿、翻到底再按 ↓ 回到什么"
// 这些边界可以单独测，不必先起一个窗口。

#include <QString>
#include <QStringList>

namespace videoeye {
namespace ffmpeg {

class FfmpegCommandHistory {
public:
    /// 最多留多少条。再多也翻不到，且 QSettings 里的字符串会越来越长。
    static constexpr int kMaxEntries = 30;

    /// 从设置读回来的历史（最新的在前）。
    void SetEntries(const QStringList& entries);
    QStringList Entries() const {
        return entries_;
    }

    /// 记录一条新命令：去首尾空白、忽略空串、**相邻重复不再记一次**，
    /// 置顶并截断到 kMaxEntries。返回 true 表示历史变了（页面据此回写设置）。
    bool Push(const QString& command);

    /// 上一条。第一次调用会记住 draft（输入框里当前的内容）；
    /// 已经翻到最早那条时继续按 ↑ 就停在原地。
    QString Previous(const QString& draft);
    /// 下一条。越过最新一条时回到 draft（也就是翻回用户还没动过的输入）。
    QString Next();
    /// 退出浏览状态：用户改了输入框，或又跑了一条命令。
    void ResetCursor();

    bool browsing() const {
        return cursor_ >= 0;
    }

private:
    QStringList entries_; // 最新在前
    int cursor_ = -1;     // -1 = 没在翻历史
    QString draft_;
};

} // namespace ffmpeg
} // namespace videoeye
