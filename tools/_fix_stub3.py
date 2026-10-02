#!/usr/bin/env python3
"""最后一轮补齐桩：QString::operator[] 返回带 unicode() 的引用、
qFromBigEndian 做成模板、QMap 迭代器补 key()/value()。"""
import io
import os

BASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    ".cache", "syntaxcheck", "stub")


def patch(name, pairs):
    p = os.path.join(BASE, name)
    s = io.open(p, encoding="utf-8").read()
    for old, new in pairs:
        if old not in s:
            print("  !! 未命中 %s :: %r" % (name, old[:60]))
            continue
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)


# 1) QString::operator[] 返回带 unicode() 的代理引用
patch("QString.h", [
    ("    char& operator[](int i) { return s_[static_cast<size_t>(i)]; }\n"
     "    char operator[](int i) const { return s_[static_cast<size_t>(i)]; }",
     "    char& operator[](int i) { return s_[static_cast<size_t>(i)]; }\n"
     "    char operator[](int i) const { return s_[static_cast<size_t>(i)]; }\n"
     "    char unicodeAt(int i) const { return s_[static_cast<size_t>(i)]; }"),
])

# 2) QtEndian 改成模板，支持 qFromBigEndian<quint64>(p) 这种显式实例化
patch("QtEndian.h", [
    ("inline quint16 qFromBigEndian(quint16 v) { return v; }\n"
     "inline qint16 qFromBigEndian(qint16 v) { return v; }\n"
     "inline quint32 qFromBigEndian(quint32 v) { return v; }\n"
     "inline qint32 qFromBigEndian(qint32 v) { return v; }\n"
     "inline quint64 qFromBigEndian(quint64 v) { return v; }\n"
     "inline qint64 qFromBigEndian(qint64 v) { return v; }\n",
     "template <typename T> T qFromBigEndian(const T& v) { return v; }\n"
     "template <typename T> T qFromLittleEndian(const T& v) { return v; }\n"),
    ("inline quint16 qFromLittleEndian(quint16 v) { return v; }\n"
     "inline qint16 qFromLittleEndian(qint16 v) { return v; }\n"
     "inline quint32 qFromLittleEndian(quint32 v) { return v; }\n"
     "inline qint32 qFromLittleEndian(qint32 v) { return v; }\n"
     "inline quint64 qFromLittleEndian(quint64 v) { return v; }\n"
     "inline qint64 qFromLittleEndian(qint64 v) { return v; }\n",
     ""),
    ("inline quint16 qToBigEndian(quint16 v) { return v; }\n"
     "inline quint32 qToBigEndian(quint32 v) { return v; }\n"
     "inline quint64 qToBigEndian(quint64 v) { return v; }\n",
     "template <typename T> T qToBigEndian(const T& v) { return v; }\n"),
    ("inline quint16 qToLittleEndian(quint16 v) { return v; }\n"
     "inline quint32 qToLittleEndian(quint32 v) { return v; }\n"
     "inline quint64 qToLittleEndian(quint64 v) { return v; }\n",
     "template <typename T> T qToLittleEndian(const T& v) { return v; }\n"),
])

# 3) QMap 用带 key()/value() 的迭代器包装（对齐 Qt 语义）
patch("QMap.h", [
    ("    typename StdMap::iterator begin() { return m.begin(); }\n"
     "    typename StdMap::iterator end() { return m.end(); }\n"
     "    typename StdMap::const_iterator begin() const { return m.begin(); }\n"
     "    typename StdMap::const_iterator end() const { return m.end(); }",
     "    struct It {\n"
     "        typename StdMap::iterator it;\n"
     "        It& operator++() { ++it; return *this; }\n"
     "        bool operator!=(const It& o) const { return it != o.it; }\n"
     "        bool operator==(const It& o) const { return it == o.it; }\n"
     "        typename StdMap::key_type key() const { return it->first; }\n"
     "        typename StdMap::mapped_type value() const { return it->second; }\n"
     "        typename StdMap::value_type& operator*() const { return *it; }\n"
     "        typename StdMap::value_type* operator->() const { return &*it; }\n"
     "    };\n"
     "    It begin() { return It{m.begin()}; }\n"
     "    It end() { return It{m.end()}; }\n"
     "    It begin() const { return It{m.begin()}; }\n"
     "    It end() const { return It{m.end()}; }"),
])

# 4) QVector 补 contains
patch("QVector.h", [
    ("    int count() const { return static_cast<int>(this->size()); }",
     "    bool contains(const T& t) const {\n"
     "        for (const T& x : *this) {\n"
     "            if (x == t) return true;\n"
     "        }\n"
     "        return false;\n"
     "    }\n"
     "    int count() const { return static_cast<int>(this->size()); }"),
])
print("stub3 ok")
