#include "core/analyzer/Mp4BoxAnalyzer.h"
#include "utils/Logger.h"

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
        node.depth = node_stack_.size();

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
            current_track_type_ = value;
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
    void FlushBareFieldEntries() {
        if (node_stack_.isEmpty()) return;
        const QString& box_type = node_stack_.top().type;
        auto& current = const_cast<model::Mp4BoxNode&>(node_stack_.top());

        for (int i = 0; i < entry_bare_values_.size(); ++i) {
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
            entry_field.name = QString("entry[%1]").arg(current.fields.size());
            entry_field.value = entry_str;
            current.fields.push_back(entry_field);
        }
        entry_bare_values_.clear();
    }

    // 将 stts/stsc 风格的对象条目保存为 entry[N]=k1=v1, k2=v2 格式
    void FlushObjectEntry() {
        if (node_stack_.isEmpty()) return;
        auto& current = const_cast<model::Mp4BoxNode&>(node_stack_.top());
        QString entry_str;
        for (const auto& f : current_entry_fields_) {
            if (!entry_str.isEmpty()) entry_str += ", ";
            entry_str += f.name + "=" + f.value;
        }
        model::Mp4BoxNode::Field entry_field;
        entry_field.name = QString("entry[%1]").arg(current.fields.size());
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
    QString current_array_name_;
    bool tracking_entries_ = false;
    bool in_entry_object_ = false;
    QVector<model::Mp4BoxNode::Field> current_entry_fields_;
    QVector<QString> entry_bare_values_;

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

    // 解析文件
    AP4_File* file = nullptr;
    try {
        file = new AP4_File(*stream, true); // moov_only = true 只解析 moov
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
    file->Inspect(inspector);

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
