#pragma once
//
// std::ifstream 的最小 std 替身（analysis 层内部）。
//
// 为什么不是直接上 std::ifstream
// -----------------------------
// core/analysis 里有六个容器分析器在用同一套 std::ifstream 惯用法：
//   std::ifstream f(path); f.open(std::ios::ReadOnly); f.read(n); f.seek(x); f.pos(); f.size();
// 直接换成 std::ifstream 的话，每个文件都得自己补 read 的短读处理、size 的 seek 到尾
// 再 seek 回来、以及"读到 EOF 之后还能不能 seek 回去"的语义 —— 六份实现迟早会长出
// 六种不一样的行为。这里按 std::ifstream 那七个方法的语义做成一个 1:1 替身，
// 迁移时调用点几乎不用动，也是 scripts/audit_qt_analysis_border.py 唯一放行的
// "文件 IO 过渡形态"。
//
// 与 std::ifstream 的三处差异（都是**故意**的，别照着 Qt 的文档改回去）：
//   1. Read(n) 返回 std::string（std::ifstream 返回 std::string），短读时两个都是"实际读到的长度"，
//      调用点用 .size() 判断即可，语义一致。
//   2. AtEnd() 用 "位置 >= 文件大小" 判定，不用 peek()==EOF —— 避免读到 EOF 时打上
//      eofbit 之后 AtEnd() 恒为真、把外层循环直接困死。
//   3. 每个方法开头都会 clear() 掉累进的 eofbit/failbit。这是最关键的一条：
//      std::ifstream 在 EOF 之后还能 seek 回去继续读，而 ifstream 一旦 failbit 上了，
//      seekg() 会静默变成空操作（不报错也不动），那类 bug 表现为"偶发读到半截数据"，
//      比编译错误难查一个量级。
//
#include <cstdint>
#include <fstream>
#include <string>

#include "core/domain/io/Utf8Path.h"

namespace videoeye {

class SeqFileReader {
public:
    SeqFileReader() = default;
    explicit SeqFileReader(const std::string& path) {
        Open(path);
    }
    ~SeqFileReader() = default;

    SeqFileReader(const SeqFileReader&) = delete;
    SeqFileReader& operator=(const SeqFileReader&) = delete;

    /// std::ifstream + open(std::ios::ReadOnly)：返回是否打开成功。
    /// path 按 UTF-8 接收，经 ToNativePath 转成原生路径（见 core/domain/io/Utf8Path.h）。
    bool Open(const std::string& path) {
        Clear();
        stream_.open(ToNativePath(path), std::ios::in | std::ios::binary);
        return stream_.is_open();
    }
    void Close() {
        if (stream_.is_open())
            stream_.close();
    }
    bool IsOpen() const {
        return stream_.is_open();
    }

    /// std::ifstream::read(n)：返回实际读到的字节（短读时短于 n，EOF 时为空串）。
    std::string Read(int64_t n) {
        if (n <= 0)
            return {};
        Clear();
        std::string out(static_cast<size_t>(n), '\0');
        stream_.read(out.data(), static_cast<std::streamsize>(n));
        out.resize(static_cast<size_t>(stream_.gcount()));
        Clear(); // 见顶部第 3 条：别把 eofbit/failbit 留给下一个操作
        return out;
    }

    /// 读进一块已存在的内存（std::istream::read(char*, n) 那条路径）。
    bool ReadRaw(void* dst, int64_t n) {
        if (n <= 0)
            return true;
        Clear();
        stream_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        const bool ok = stream_.gcount() == static_cast<std::streamsize>(n);
        Clear();
        return ok;
    }

    /// std::ifstream::size()：文件总字节数。
    int64_t Size() const {
        SeqFileReader* self = const_cast<SeqFileReader*>(this);
        self->Clear();
        const std::streampos cur = stream_.tellg();
        stream_.seekg(0, std::ios::end);
        const int64_t size = static_cast<int64_t>(stream_.tellg());
        stream_.seekg(cur);
        self->Clear();
        return size < 0 ? 0 : size;
    }

    /// std::ifstream::pos()：当前读位置。
    ///
    /// ❗ tellg() 在**恰好停在文件末尾**时返回 -1（不是 size）。旧实现把 -1 映射成 0，
    /// 于是"已经在末尾"变成了"还在开头"：所有 `while (Pos() < Size() - k)` 的循环判据
    /// 都会认为还有得读，回头把文件头再读一遍。若循环体里的 Seek 目标又被夹回 0
    /// （越界 / 负偏移都夹到 0，见 Seek），就变成死循环 —— FuzzContainerStructure
    /// 抓到 ASF 死循环就是这么来的：31 字节的输入，header_size 声明得比文件大，
    /// `Seek(Size())` 之后 Pos() 报 0，第二段循环原地打转。
    ///
    /// 所以这里把 -1 解释成"已到末尾"，与 AtEnd() 的判定口径一致。
    int64_t Pos() const {
        SeqFileReader* self = const_cast<SeqFileReader*>(this);
        self->Clear();
        const std::streampos p = stream_.tellg();
        self->Clear();
        if (p == std::streampos(std::streamoff(-1))) {
            // 见上：EOF 处的 -1 是"末尾"，不是"开头"
            return Size();
        }
        return static_cast<int64_t>(p);
    }

    /// std::ifstream::seek(x)：把位置挪到文件内偏移。超界会被夹到文件末尾，与 std::ifstream 一致
    /// （std::ifstream 允许 seek 到越界位置，但随后 read 返回空；这里夹住更不容易读出错偏移）。
    bool Seek(int64_t off) {
        Clear();
        const int64_t size = Size();
        int64_t target = off;
        if (target < 0)
            target = 0;
        if (target > size)
            target = size;
        stream_.seekg(static_cast<std::streamoff>(target), std::ios::beg);
        Clear();
        return stream_.good();
    }

    /// std::ifstream::atEnd()：位置已经到文件尾。
    bool AtEnd() const {
        return Pos() >= Size();
    }

    /// 只读一整份（std::ifstream::readAll 的路径：先探大小再一次性读完）。
    std::string ReadAll() {
        Clear();
        const int64_t size = Size();
        std::string out;
        if (size <= 0)
            return out;
        out.resize(static_cast<size_t>(size));
        stream_.seekg(0, std::ios::beg);
        stream_.read(out.data(), static_cast<std::streamsize>(size));
        out.resize(static_cast<size_t>(stream_.gcount()));
        Clear();
        return out;
    }

private:
    /// 只在"已经处于失败态"时清标志；good() 时 clear() 是空操作。
    void Clear() {
        if (!stream_.good())
            stream_.clear();
    }

    mutable std::ifstream stream_;
};

} // namespace videoeye
