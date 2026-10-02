#!/usr/bin/env python3
"""给 Qt 桩补上 Qt 风格接口，让离线语法检查尽量少出假阳性。"""
import io
import os

BASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    ".cache", "syntaxcheck", "stub")


def w(name, text):
    io.open(os.path.join(BASE, name), "w", encoding="utf-8", newline="").write(text)


def patch(name, pairs):
    p = os.path.join(BASE, name)
    s = io.open(p, encoding="utf-8").read()
    for old, new in pairs:
        if old not in s:
            print("  !! 未命中: %s :: %s" % (name, old[:60]))
            continue
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)


# --- QtGlobal: qMin / qMax / qAbs ---
patch("QtGlobal.h", [(
    "#define Q_LITTLE_ENDIAN 1",
    "template <typename T> const T& qMin(const T& a, const T& b) { return a < b ? a : b; }\n"
    "template <typename T> const T& qMax(const T& a, const T& b) { return a < b ? b : a; }\n"
    "template <typename T> T qAbs(const T& a) { return a < 0 ? static_cast<T>(-a) : a; }\n"
    "#define Q_LITTLE_ENDIAN 1",
)])

# --- QString: fromUtf16 / remove / 多参数 arg / number 重载 ---
patch("QString.h", [
    ("    static QString fromUtf8(const char* s)",
     "    QString remove(QChar) const { return *this; }\n"
     "    QString remove(const QString& s) const { return *this; }\n"
     "    static QString fromUtf16(const char16_t* u, int n = -1) {\n"
     "        return QString(u && n > 0 ? std::string(reinterpret_cast<const char*>(u),\n"
     "                                                static_cast<size_t>(n) * 2) : std::string());\n"
     "    }\n"
     "    static QString fromUtf8(const char* s)"),
    ("    QString arg(double a, int fieldWidth = 0, char fmt = 'g', int prec = 6,\n"
     "                QChar fill = QChar(' ')) const;",
     "    QString arg(double a, int fieldWidth = 0, char fmt = 'g', int prec = 6,\n"
     "                QChar fill = QChar(' ')) const;\n"
     "    QString arg(const QString& a1, const QString& a2) const { return arg(a1).arg(a2); }\n"
     "    QString arg(const QString& a1, const QString& a2, const QString& a3) const { return arg(a1).arg(a2).arg(a3); }\n"
     "    QString arg(const QString& a1, const QString& a2, const QString& a3,\n"
     "                const QString& a4) const { return arg(a1).arg(a2).arg(a3).arg(a4); }\n"
     "    QString arg(const QString& a1, const QString& a2, const QString& a3,\n"
     "                const QString& a4, const QString& a5) const {\n"
     "        return arg(a1).arg(a2).arg(a3).arg(a4).arg(a5);\n"
     "    }"),
    ("    static QString number(int v)",
     "    static QString number(short v) { return QString(std::to_string(v)); }\n"
     "    static QString number(ushort v) { return QString(std::to_string(v)); }\n"
     "    static QString number(uint v) { return QString(std::to_string(v)); }\n"
     "    static QString number(ulong v) { return QString(std::to_string(v)); }\n"
     "    static QString number(quint64 v) {\n"
     "        return QString(std::to_string(static_cast<unsigned long long>(v)));\n"
     "    }\n"
     "    static QString number(int v)"),
])

# --- QByteArray: QByteArray 版本的 startsWith / endsWith / contains ---
patch("QByteArray.h", [
    ("    bool startsWith(const char* s) const { return s_.rfind(s, 0) == 0; }",
     "    bool startsWith(const char* s) const { return s_.rfind(s, 0) == 0; }\n"
     "    bool startsWith(const QByteArray& s) const { return s_.rfind(s.s_, 0) == 0; }\n"
     "    bool endsWith(const QByteArray& s) const { return endsWith(s.s_.c_str()); }\n"
     "    bool contains(const QByteArray& s) const {\n"
     "        return s_.find(s.s_) != std::string::npos;\n"
     "    }\n"
     "    int indexOf(const QByteArray& s) const { return static_cast<int>(s_.find(s.s_)); }"),
])

# --- QMap: Qt 风格接口 ---
w("QMap.h", '''// 仅供离线语法检查用的最小 QMap 桩（不属于工程源码）。
#pragma once
#include <map>
#include <vector>
template <typename K, typename V> class QMap {
public:
    using StdMap = std::map<K, V>;
    StdMap m;
    V& operator[](const K& k) { return m[k]; }
    const V& operator[](const K& k) const { return m.find(k)->second; }
    typename StdMap::iterator find(const K& k) { return m.find(k); }
    typename StdMap::const_iterator find(const K& k) const { return m.find(k); }
    bool contains(const K& k) const { return m.count(k) != 0; }
    int count(const K& k) const { return static_cast<int>(m.count(k)); }
    void insert(const K& k, const V& v) { m[k] = v; }
    void insert(const typename StdMap::value_type& p) { m[p.first] = p.second; }
    int size() const { return static_cast<int>(m.size()); }
    bool isEmpty() const { return m.empty(); }
    void clear() { m.clear(); }
    std::vector<V> values() const {
        std::vector<V> out;
        out.reserve(m.size());
        for (const auto& kv : m) out.push_back(kv.second);
        return out;
    }
    std::vector<K> keys() const {
        std::vector<K> out;
        out.reserve(m.size());
        for (const auto& kv : m) out.push_back(kv.first);
        return out;
    }
    K key(const V& v, K def = K()) const {
        for (const auto& kv : m) {
            if (kv.second == v) return kv.first;
        }
        return def;
    }
    V value(const K& k, V def = V()) const {
        auto it = m.find(k);
        return it == m.end() ? def : it->second;
    }
    typename StdMap::iterator begin() { return m.begin(); }
    typename StdMap::iterator end() { return m.end(); }
    typename StdMap::const_iterator begin() const { return m.begin(); }
    typename StdMap::const_iterator end() const { return m.end(); }
};
''')

# --- QVector: Qt 风格 append / prepend / insert ---
w("QVector.h", '''// 仅供离线语法检查用的最小 QVector 桩（不属于工程源码）。
#pragma once
#include <vector>
template <typename T> class QVector : public std::vector<T> {
public:
    using std::vector<T>::vector;
    void append(const T& t) { this->push_back(t); }
    void append(const QVector& o) { this->insert(this->end(), o.begin(), o.end()); }
    void prepend(const T& t) { this->insert(this->begin(), t); }
    void insert(int i, const T& t) { this->insert(this->begin() + i, t); }
    int count() const { return static_cast<int>(this->size()); }
    bool isEmpty() const { return this->empty(); }
    T& operator[](int i) { return std::vector<T>::operator[](static_cast<size_t>(i)); }
    const T& operator[](int i) const {
        return std::vector<T>::operator[](static_cast<size_t>(i));
    }
};
''')

print("stub extended ok")
