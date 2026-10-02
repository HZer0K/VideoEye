#!/usr/bin/env python3
"""继续修桩：消除剩余假阳性，让离线语法检查只剩项目本身的问题。"""
import io
import os

BASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    ".cache", "syntaxcheck", "stub")


def patch(name, pairs):
    p = os.path.join(BASE, name)
    s = io.open(p, encoding="utf-8").read()
    for old, new in pairs:
        if old not in s:
            print("  !! 未命中 %s :: %s" % (name, old[:60]))
            continue
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)


# QByteArray::toStdString 返回 std::string（真实 Qt 就是），否则 const char* 拼接会误报
patch("QByteArray.h", [
    ('    const char* toStdString() const { return s_.c_str(); }\n    std::string toStdString2() const { return s_; }',
     '    std::string toStdString() const { return s_; }'),
])

# QByteArray::data() 非 const 上下文应返回 char*
patch("QByteArray.h", [
    ('    const char* data() const { return s_.c_str(); }',
     '    const char* data() const { return s_.c_str(); }\n'
     '    char* data() { return &s_[0]; }'),
])

# QString::operator[] 应返回可 .unicode() 的东西
patch("QString.h", [
    ('    char& operator[](int i) { return s_[static_cast<size_t>(i)]; }\n'
     '    char operator[](int i) const { return s_[static_cast<size_t>(i)]; }',
     '    class Ref {\n'
     '    public:\n'
     '        Ref(char& c) : c_(c) {}\n'
     '        char unicode() const { return c_; }\n'
     '        operator char() const { return c_; }\n'
     '        Ref& operator=(char v) { c_ = v; return *this; }\n'
     '        char& c_;\n'
     '    };\n'
     '    Ref operator[](int i) { return Ref(s_[static_cast<size_t>(i)]); }\n'
     '    char operator[](int i) const { return s_[static_cast<size_t>(i)]; }'),
])
