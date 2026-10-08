#pragma once

// ============================================================
// 轻量 PDF 结构验证器（测试专用，不依赖 qpdf / mutool / poppler）
//
// 为什么自己写: "PDF 导出成功"以前只被断言成"文件写出来了"，而一个字节都画不出
// 来的 PDF 一样能被写出来。这里只检查**对象关系与 xref**这两件真正决定
// "阅读器能不能打开"的事：
//   1. 文件头 / startxref 偏移 / xref 每条记录的偏移是否真的落在 "<n> 0 obj" 上
//   2. trailer -> /Root -> /Pages -> /Kids -> 每个 /Page -> /Contents / /Font
//      这条引用链上的每个对象是否都存在、类型是否对得上
//   3. 内容流的 /Length 与实际字节数是否一致
// 另外把内容流里的文字还原出来（含中文用的 UTF-16BE 十六进制串），让测试能直接
// 断言"中文没变成 ?"。
// ============================================================

#include <string>
#include <vector>

namespace videoeye_test {

struct PdfVerifyResult {
    bool ok = false;
    std::string error;              // 第一条失败原因（ok=false 时非空）

    std::string version;            // 形如 "%PDF-1.4"
    long long xref_offset = -1;     // startxref 给的偏移
    int object_count = 0;           // trailer /Size（含 0 号 free 对象）
    int root_object = 0;
    int pages_object = 0;
    int page_count = 0;             // /Pages 里的 /Count
    std::vector<int> page_objects;
    std::vector<int> content_objects;
    std::vector<int> font_objects;
    int stream_length_mismatch = 0; // /Length 与实际字节数对不上的流数量
    int text_runs = 0;              // 内容流里 Tj 的次数
    int line_advances = 0;          // T* / Td / TD 的次数（换行操作符）
    int text_lines = 0;             // 由上面两个推出的"实际占了几行"
    std::string text;               // 还原出的纯文本（UTF-8）
};

// 读取 + 结构校验。任何一个环节对不上就 ok=false 并在 error 里写明原因。
PdfVerifyResult VerifyPdfFile(const std::string& path);

// 把整个文件读成字符串（二进制安全）
bool ReadWholeFile(const std::string& path, std::string& out);

}  // namespace videoeye_test
