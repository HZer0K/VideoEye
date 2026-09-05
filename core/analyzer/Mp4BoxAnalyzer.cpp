#include "core/analyzer/Mp4BoxAnalyzer.h"
#include "utils/Logger.h"

#ifdef HAVE_BENTO4

// Bento4 includes
#include "Ap4.h"
#include "Ap4FileByteStream.h"
#include "Ap4Atom.h"
#include "Ap4StszAtom.h"
#include "Ap4StcoAtom.h"
#include "Ap4Co64Atom.h"
#include "Ap4StscAtom.h"
#include "Ap4SttsAtom.h"

namespace videoeye {
namespace analyzer {

// ============================================================
// 自定义 AP4_AtomInspector: 遍历 Box 树并收集数据
// ============================================================
class Mp4BoxAnalyzer::VideoEyeInspector : public AP4_AtomInspector {
public:
    VideoEyeInspector(model::Mp4BoxAnalysisResult& result)
        : result_(result) {}

    // AP4_AtomInspector 接口实现
    void StartAtom(const char* name, AP4_UI08 version, AP4_UI32 flags,
                   AP4_Size header_size, AP4_UI64 size) override {
        model::Mp4BoxNode node;
        node.type = QString::fromLatin1(name);
        node.size = static_cast<uint64_t>(size);
        // 偏移量: 取父 Box 的"下一个子 Box 起始位置"游标 (顶层则从 0 开始)。
        // 不能像旧实现那样在 StartAtom 里就把自身 size 累加进全局游标,
        // 否则子 Box 的偏移会被所有祖先容器的 size 整体推后, 出现
        // "子节点偏移大于父节点" 的荒谬结果。
        node.offset = child_cursor_.isEmpty() ? root_cursor_ : child_cursor_.top();
        node.depth = node_stack_.size();
        // 子 Box 数据区从 header 之后开始
        child_cursor_.push(node.offset + header_size);

        // 记录 trak 上下文
        if (node.type == "trak") {
            trak_nesting_++;
        }
        // 进入 trak 子原子时，沿用当前 trak 上下文
        if (trak_nesting_ > 0 && node.type == "tkhd") {
            inside_tkhd_ = true;
        }
        if (trak_nesting_ > 0 && node.type == "hdlr") {
            inside_hdlr_ = true;
        }

        // 记录版本和标志作为字段
        if (version != 0 || flags != 0) {
            model::Mp4BoxNode::Field f;
            // 将 version/flags 设为第一组字段
            f.name = "version";
            f.value = QString::number(version);
            node.fields.push_back(f);
            f.name = "flags";
            f.value = QString("0x%1").arg(flags, 0, 16);
            node.fields.push_back(f);
        }

        node_stack_.push(node);
        current_array_name_.clear();
        tracking_entries_ = false;
        in_entry_object_ = false;
        current_entry_fields_.clear();
        entry_bare_values_.clear();
    }

    void EndAtom() override {
        if (node_stack_.isEmpty()) return;

        model::Mp4BoxNode node = node_stack_.pop();

        // 弹出本 Box 的子游标; 父游标直接推进到本 Box 末尾 (offset+size),
        // 这样即使子 Box 之间有填充/未解析数据, 父的下一个子 Box 偏移依然正确。
        child_cursor_.pop();
        if (!child_cursor_.isEmpty()) {
            child_cursor_.top() = node.offset + node.size;
        } else {
            root_cursor_ = node.offset + node.size;  // 顶层 Box 依次向后排
        }

        // 退出 trak 上下文
        if (node.type == "trak") {
            trak_nesting_--;
            if (trak_nesting_ == 0) {
                current_track_id_ = 0;
                current_track_type_.clear();
            }
        }
        if (node.type == "tkhd") inside_tkhd_ = false;
        if (node.type == "hdlr") inside_hdlr_ = false;

        // 收集该 Box 的入口数据到 track_tables_
        CollectEntryData(node);

        if (node_stack_.isEmpty()) {
            result_.box_tree.push_back(node);
        } else {
            node_stack_.top().children.push_back(node);
        }
    }

    void StartArray(const char* name, unsigned int element_count) override {
        current_array_name_ = QString::fromLatin1(name);
        tracking_entries_ = (current_array_name_ == "entries");
        if (tracking_entries_) {
            current_entry_fields_.clear();
            entry_bare_values_.clear();
            entry_index_ = 0;  // 每条 entry 从 0 开始独立编号
        }
        (void)element_count;
    }

    void EndArray() override {
        if (tracking_entries_) {
            // stco/stsz 风格: 裸 AddField 条目 → 保存到当前 Box 字段
            if (!entry_bare_values_.isEmpty()) {
                FlushBareFieldEntries();
            }
            current_entry_fields_.clear();
            entry_bare_values_.clear();
        }
        current_array_name_.clear();
        tracking_entries_ = false;
        in_entry_object_ = false;
    }

    void StartObject(const char* name, unsigned int field_count, bool compact) override {
        if (tracking_entries_) {
            current_entry_fields_.clear();
            in_entry_object_ = true;
        }
        (void)name;
        (void)field_count;
        (void)compact;
    }

    void EndObject() override {
        if (tracking_entries_ && in_entry_object_ && !current_entry_fields_.isEmpty()) {
            // stts/stsc 风格: 将当前条目的字段保存到 Box 节点
            FlushObjectEntry();
            current_entry_fields_.clear();
        }
        in_entry_object_ = false;
    }

    void AddField(const char* name, AP4_UI64 value, FormatHint hint) override {
        QString val_str;
        if (hint == HINT_HEX) {
            val_str = QString("0x%1").arg(static_cast<quint64>(value), 0, 16);
        } else if (hint == HINT_BOOLEAN) {
            val_str = value ? "true" : "false";
        } else {
            val_str = QString::number(static_cast<quint64>(value));
        }
        AddFieldCommon(name, val_str);
        (void)hint;
    }

    void AddFieldF(const char* name, float value, FormatHint hint) override {
        AddFieldCommon(name, QString::number(static_cast<double>(value), 'f', 4));
        (void)hint;
    }

    void AddField(const char* name, const char* value, FormatHint hint) override {
        AddFieldCommon(name, QString::fromUtf8(value));
        (void)hint;
    }

    void AddField(const char* name, const unsigned char* bytes,
                  AP4_Size byte_count, FormatHint hint) override {
        QString hex;
        for (AP4_Size i = 0; i < byte_count && i < 16; ++i) {
            hex += QString::number(bytes[i], 16).rightJustified(2, '0');
        }
        if (byte_count > 16) hex += "...";
        AddFieldCommon(name, hex);
        (void)hint;
    }

private:
    void AddFieldCommon(const char* name, const QString& value) {
        // 追踪 trak 上下文: tkhd.id → current_track_id_, hdlr.handler_type → current_track_type_
        if (inside_tkhd_ && name && strcmp(name, "id") == 0) {
            current_track_id_ = value.toInt();
        }
        if (inside_hdlr_ && name && strcmp(name, "handler_type") == 0) {
            // 只取 trak 内第一个 hdlr (mdia/hdlr) 的 handler_type 作为媒体类型。
            // QuickTime MOV 在 minf 下还有一个 hdlr (handler_type='url ', DataHandler),
            // 若无条件覆盖会把视频轨道类型显示成 "url "。
            if (current_track_type_.isEmpty()) {
                current_track_type_ = value;
            }
        }

        if (tracking_entries_) {
            if (in_entry_object_) {
                // stts/stsc: 命名条目字段
                model::Mp4BoxNode::Field field;
                field.name = name ? QString::fromLatin1(name) : QString();
                field.value = value;
                current_entry_fields_.push_back(field);
            } else {
                // stco/stsz: 裸值条目
                entry_bare_values_.push_back(value);
            }
        } else {
            if (!node_stack_.isEmpty()) {
                model::Mp4BoxNode::Field field;
                field.name = name ? QString::fromLatin1(name) : "unnamed";
                field.value = value;
                const_cast<model::Mp4BoxNode&>(node_stack_.top()).fields.push_back(field);
            }
        }
    }

    // 将 stco/stsz 风格的裸值条目保存为 entry[N]=key=value 格式
    // 封顶: 单个 Box 的样本条目最多保留 kMaxEntryFields 条, 避免大文件展开成
    // 几十万个 QString 字段, 拖垮后续 ConvertMp4Tree / ExtractMp4StreamInfo 遍历。
    static const int kMaxEntryFields = 5000;
    void FlushBareFieldEntries() {
        if (node_stack_.isEmpty()) return;
        const QString& box_type = node_stack_.top().type;
        auto& current = const_cast<model::Mp4BoxNode&>(node_stack_.top());

        for (int i = 0; i < entry_bare_values_.size(); ++i) {
            if (current.fields.size() >= kMaxEntryFields) break;
            QString entry_str;
            if (box_type == "stco" || box_type == "Co64Atom"
                || box_type == "co64") {
                entry_str = QString("chunk_offset=%1").arg(entry_bare_values_[i]);
            } else if (box_type == "stsz" || box_type == "StszAtom") {
                entry_str = QString("size=%1").arg(entry_bare_values_[i]);
            } else {
                entry_str = QString("value=%1").arg(entry_bare_values_[i]);
            }
            model::Mp4BoxNode::Field entry_field;
            entry_field.name = QString("entry[%1]").arg(entry_index_++);
            entry_field.value = entry_str;
            current.fields.push_back(entry_field);
        }
        entry_bare_values_.clear();
    }

    // 将 stts/stsc 风格的对象条目保存为 entry[N]=k1=v1, k2=v2 格式
    void FlushObjectEntry() {
        if (node_stack_.isEmpty()) return;
        auto& current = const_cast<model::Mp4BoxNode&>(node_stack_.top());
        if (current.fields.size() >= kMaxEntryFields) {
            current_entry_fields_.clear();
            return;
        }
        QString entry_str;
        for (const auto& f : current_entry_fields_) {
            if (!entry_str.isEmpty()) entry_str += ", ";
            entry_str += f.name + "=" + f.value;
        }
        model::Mp4BoxNode::Field entry_field;
        entry_field.name = QString("entry[%1]").arg(entry_index_++);
        entry_field.value = entry_str;
        current.fields.push_back(entry_field);
    }

    // 从当前结束的 Box 中提取 stts/stco/stsc/stsz 数据到 track_tables_
    void CollectEntryData(const model::Mp4BoxNode& node) {
        if (node.type == "stts") {
            CollectSttsFromFields(node);
        } else if (node.type == "stco") {
            CollectStcoFromFields(node);
        } else if (node.type == "co64") {
            CollectCo64FromFields(node);
        } else if (node.type == "stsc") {
            CollectStscFromFields(node);
        } else if (node.type == "stsz") {
            CollectStszFromFields(node);
        } else if (node.type == "stss") {
            CollectStssFromFields(node);
        }
    }

    // 合并 track_tables: 将属于同一 track 的记录合并到一条
    void ConsolidateAndAddTable(model::TrackBoxTables&& new_table) {
        for (auto& existing : result_.track_tables) {
            if (existing.track_id == new_table.track_id) {
                existing.stts_entries.append(new_table.stts_entries);
                existing.stco_entries.append(new_table.stco_entries);
                existing.co64_entries.append(new_table.co64_entries);
                existing.stsc_entries.append(new_table.stsc_entries);
                existing.stsz_entries.append(new_table.stsz_entries);
                existing.stsz_default_size = new_table.stsz_default_size;
                existing.stsz_sample_count = new_table.stsz_sample_count;
                // 关键帧表: 原实现漏了这一行, stss 条目在合并时被静默丢弃,
                // 导致关键帧(stss)表在 UI 里永远为空。
                existing.stss_entries.append(new_table.stss_entries);
                return;
            }
        }
        // 未找到同 track → 新增
        result_.track_tables.push_back(new_table);
    }

    void CollectSttsFromFields(const model::Mp4BoxNode& node) {
        model::TrackBoxTables tables;
        tables.track_id = current_track_id_;
        tables.track_type = current_track_type_;
        for (const auto& f : node.fields) {
            if (f.name.startsWith("entry[")) {
                // Parse "sample_count=123, sample_duration=456"
                auto parts = f.value.split(", ");
                model::SttsEntry entry;
                for (const auto& p : parts) {
                    auto kv = p.split('=');
                    if (kv.size() == 2) {
                        if (kv[0] == "sample_count")
                            entry.sample_count = kv[1].toUInt();
                        else if (kv[0] == "sample_duration")
                            entry.sample_delta = kv[1].toUInt();
                    }
                }
                tables.stts_entries.push_back(entry);
            }
        }
        if (!tables.stts_entries.isEmpty()) {
            ConsolidateAndAddTable(std::move(tables));
        }
    }

    void CollectStcoFromFields(const model::Mp4BoxNode& node) {
        model::TrackBoxTables tables;
        tables.track_id = current_track_id_;
        tables.track_type = current_track_type_;
        for (const auto& f : node.fields) {
            if (f.name.startsWith("entry[")) {
                auto parts = f.value.split(", ");
                model::StcoEntry entry;
                for (const auto& p : parts) {
                    auto kv = p.split('=');
                    if (kv.size() == 2 && kv[0] == "chunk_offset") {
                        entry.chunk_offset = kv[1].toUInt();
                    }
                }
                tables.stco_entries.push_back(entry);
            }
        }
        if (!tables.stco_entries.isEmpty()) {
            ConsolidateAndAddTable(std::move(tables));
        }
    }

    void CollectCo64FromFields(const model::Mp4BoxNode& node) {
        model::TrackBoxTables tables;
        tables.track_id = current_track_id_;
        tables.track_type = current_track_type_;
        for (const auto& f : node.fields) {
            if (f.name.startsWith("entry[")) {
                auto parts = f.value.split(", ");
                model::Co64Entry entry;
                for (const auto& p : parts) {
                    auto kv = p.split('=');
                    if (kv.size() == 2 && kv[0] == "chunk_offset") {
                        entry.chunk_offset = kv[1].toULongLong();
                    }
                }
                tables.co64_entries.push_back(entry);
            }
        }
        if (!tables.co64_entries.isEmpty()) {
            ConsolidateAndAddTable(std::move(tables));
        }
    }

    void CollectStscFromFields(const model::Mp4BoxNode& node) {
        model::TrackBoxTables tables;
        tables.track_id = current_track_id_;
        tables.track_type = current_track_type_;
        for (const auto& f : node.fields) {
            if (f.name.startsWith("entry[")) {
                auto parts = f.value.split(", ");
                model::StscEntry entry;
                for (const auto& p : parts) {
                    auto kv = p.split('=');
                    if (kv.size() == 2) {
                        if (kv[0] == "first_chunk")
                            entry.first_chunk = kv[1].toUInt();
                        else if (kv[0] == "samples_per_chunk")
                            entry.samples_per_chunk = kv[1].toUInt();
                        // Bento4 使用的字段名是 sample_desc_index
                        else if (kv[0] == "sample_desc_index" ||
                                 kv[0] == "sample_description_index")
                            entry.sample_description_index = kv[1].toUInt();
                    }
                }
                tables.stsc_entries.push_back(entry);
            }
        }
        if (!tables.stsc_entries.isEmpty()) {
            ConsolidateAndAddTable(std::move(tables));
        }
    }

    void CollectStszFromFields(const model::Mp4BoxNode& node) {
        model::TrackBoxTables tables;
        tables.track_id = current_track_id_;
        tables.track_type = current_track_type_;
        uint32_t default_size = 0;
        for (const auto& f : node.fields) {
            if (f.name == "sample_size" && !f.value.startsWith("entry")) {
                default_size = f.value.toUInt();
            } else if (f.name == "sample_count") {
                tables.stsz_sample_count = f.value.toUInt();
            } else if (f.name.startsWith("entry[")) {
                auto parts = f.value.split(", ");
                model::StszEntry entry;
                for (const auto& p : parts) {
                    auto kv = p.split('=');
                    if (kv.size() == 2 && kv[0] == "size") {
                        entry.sample_size = kv[1].toUInt();
                    }
                }
                tables.stsz_entries.push_back(entry);
            }
        }
        tables.stsz_default_size = default_size;
        if (!tables.stsz_entries.isEmpty() || default_size > 0) {
            ConsolidateAndAddTable(std::move(tables));
        }
    }

    void CollectStssFromFields(const model::Mp4BoxNode& node) {
        model::TrackBoxTables tables;
        tables.track_id = current_track_id_;
        tables.track_type = current_track_type_;
        for (const auto& f : node.fields) {
            if (f.name.startsWith("entry[")) {
                auto parts = f.value.split(", ");
                model::StssEntry entry;
                for (const auto& p : parts) {
                    auto kv = p.split('=');
                    if (kv.size() == 2 && kv[0] == "sample_number") {
                        entry.sample_number = kv[1].toUInt();
                    }
                }
                tables.stss_entries.push_back(entry);
            }
        }
        if (!tables.stss_entries.isEmpty()) {
            ConsolidateAndAddTable(std::move(tables));
        }
    }

    model::Mp4BoxAnalysisResult& result_;
    QStack<model::Mp4BoxNode> node_stack_;
    // 每层记录"下一个子 Box 的起始偏移": 栈顶 = 当前 Box 的子 Box 游标
    QStack<uint64_t> child_cursor_;
    uint64_t root_cursor_ = 0;     // 顶层 Box 游标 (栈空时使用)
    QString current_array_name_;
    bool tracking_entries_ = false;
    bool in_entry_object_ = false;
    QVector<model::Mp4BoxNode::Field> current_entry_fields_;
    QVector<QString> entry_bare_values_;
    int entry_index_ = 0;      // 当前 entries 数组内的条目序号 (0-based)

    // 当前 trak 上下文
    int current_track_id_ = 0;
    QString current_track_type_;
    int trak_nesting_ = 0;      // trak 嵌套深度
    bool inside_tkhd_ = false;  // 当前在 tkhd 原子内
    bool inside_hdlr_ = false;  // 当前在 hdlr 原子内
};

// ============================================================
// Mp4BoxAnalyzer 实现
// ============================================================
Mp4BoxAnalyzer::Mp4BoxAnalyzer() = default;

Mp4BoxAnalyzer::~Mp4BoxAnalyzer() = default;

bool Mp4BoxAnalyzer::AnalyzeFile(const QString& file_path,
                                  model::Mp4BoxAnalysisResult& result) {
    result = model::Mp4BoxAnalysisResult();
    result.file_path = file_path;
    LOG_INFO("Mp4BoxAnalyzer::AnalyzeFile ENTER: " + file_path.toStdString());

    // 打开文件
    AP4_ByteStream* stream = nullptr;
    AP4_Result ap4_result = AP4_FileByteStream::Create(
        file_path.toUtf8().constData(),
        AP4_FileByteStream::STREAM_MODE_READ,
        stream);

    if (AP4_FAILED(ap4_result)) {
        result.error_message = QString("无法打开文件: %1 (Bento4 error: %2)")
            .arg(file_path)
            .arg(ap4_result);
        LOG_ERROR(result.error_message.toStdString());
        return false;
    }
    LOG_INFO("Mp4BoxAnalyzer: file opened");

    // 解析文件
    // moov_only = false: 解析全部顶层 Box (ftyp/free/mdat/moov/...),
    // 否则树里只剩 moov 一个顶层节点, 且偏移从 0 起算全部失真。
    // 注意 mdat 等未知 Box 是叶子节点, 不会被递归展开, 因此开销可忽略。
    AP4_File* file = nullptr;
    try {
        file = new AP4_File(*stream, false);
    } catch (...) {
        result.error_message = "解析 MP4 文件时发生异常";
        LOG_ERROR(result.error_message.toStdString());
        stream->Release();
        return false;
    }

    if (!file) {
        result.error_message = "无法创建 AP4_File 对象";
        LOG_ERROR(result.error_message.toStdString());
        stream->Release();
        return false;
    }

    // 使用自定义 Inspector 遍历 Box 树
    // 设置 verbosity=2 以输出完整的 entry 明细数据
    // (stts/stco/stsc 需要 ≥1, stsz 需要 ≥2)
    VideoEyeInspector inspector(result);
    inspector.SetVerbosity(2);
    LOG_INFO("Mp4BoxAnalyzer: BEFORE Inspect (verbosity=2, 会遍历全部 sample 表)");
    // 直接遍历顶层子节点, 而不是 file->Inspect():
    // AP4_File::Inspect() 会先单独 dump 一次 moov (通过 m_Movie), 再 dump 全部子 Box,
    // 导致 moov 在树中出现两次。
    AP4_AtomListInspector list_inspector(inspector);
    file->GetChildren().Apply(list_inspector);
    LOG_INFO("Mp4BoxAnalyzer: AFTER Inspect");

    // 清理
    delete file;
    stream->Release();

    result.valid = true;
    LOG_INFO("MP4 Box 分析完成: " + std::to_string(result.box_tree.size()) +
             " 个顶级 Box, " + std::to_string(result.track_tables.size()) +
             " 个 Track 表");

    return true;
}

void Mp4BoxAnalyzer::Reset() {
    // 无状态需要清理
}

} // namespace analyzer
} // namespace videoeye

#else

namespace videoeye {
namespace analyzer {

Mp4BoxAnalyzer::Mp4BoxAnalyzer() = default;

Mp4BoxAnalyzer::~Mp4BoxAnalyzer() = default;

bool Mp4BoxAnalyzer::AnalyzeFile(const QString& file_path,
                                  model::Mp4BoxAnalysisResult& result) {
    result = model::Mp4BoxAnalysisResult();
    result.file_path = file_path;
    result.error_message = "Bento4 库未链接，无法分析 MP4 文件";
    LOG_WARN(result.error_message.toStdString());
    return false;
}

void Mp4BoxAnalyzer::Reset() {
    // 无状态需要清理
}

} // namespace analyzer
} // namespace videoeye

#endif // HAVE_BENTO4
