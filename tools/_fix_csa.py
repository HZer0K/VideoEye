# -*- coding: utf-8 -*-
"""精确替换 ContainerStructureAnalyzer.cpp：Qt 字符串/容器 -> std。

每一条替换都断言命中；任何一条没命中就整体放弃（不写文件）。
"""
import io

P = "core/analysis/container/ContainerStructureAnalyzer.cpp"
s = io.open(P, encoding="utf-8", newline="").read()
orig = s

R = [
    # ---------- Mp4FourccToName ----------
    ('QString Mp4FourccToName(const QString& fourcc) {',
     'std::string Mp4FourccToName(const std::string& fourcc) {'),
    ('    return QString();', '    return std::string();'),

    # ---------- Analyze ----------
    ('    result.file_path = file_path;\n    LOG_INFO("ContainerStructureAnalyzer::Analyze ENTER: " + file_path.toStdString());',
     '    result.file_path = file_path.toStdString();\n'
     '    LOG_INFO("ContainerStructureAnalyzer::Analyze ENTER: " + file_path.toStdString());'),
    ('    result.format_name = FormatDetector::FormatName(fmt);',
     '    result.format_name = FormatDetector::FormatName(fmt).toStdString();'),
    ('            std::function<int(const QVector<model::Mp4BoxNode>&)> count;\n'
     '            count = [&](const QVector<model::Mp4BoxNode>& nodes) -> int {',
     '            std::function<int(const std::vector<model::Mp4BoxNode>&)> count;\n'
     '            count = [&](const std::vector<model::Mp4BoxNode>& nodes) -> int {'),
    ('            result.summary = QString("MP4 Box | 顶级: %1 | 总计: %2 | Track: %3")\n'
     '                                 .arg(result.mp4_detail.box_tree.size())\n'
     '                                 .arg(box_count)\n'
     '                                 .arg(result.streams.size());',
     '            result.summary = (QString("MP4 Box | 顶级: %1 | 总计: %2 | Track: %3")\n'
     '                                  .arg(result.mp4_detail.box_tree.size())\n'
     '                                  .arg(box_count)\n'
     '                                  .arg(result.streams.size()))\n'
     '                                 .toStdString();'),
    ('            std::function<int(const QVector<model::EbmlElementNode>&)> count;\n'
     '            count = [&](const QVector<model::EbmlElementNode>& nodes) -> int {',
     '            std::function<int(const std::vector<model::EbmlElementNode>&)> count;\n'
     '            count = [&](const std::vector<model::EbmlElementNode>& nodes) -> int {'),
    ('            result.summary = QString("%1 | %2 个元素 | %3 轨道")\n'
     '                                 .arg(result.ebml_detail.doc_type)\n'
     '                                 .arg(elem_count)\n'
     '                                 .arg(result.streams.size());',
     '            result.summary = (QString("%1 | %2 个元素 | %3 轨道")\n'
     '                                  .arg(QString::fromStdString(result.ebml_detail.doc_type))\n'
     '                                  .arg(elem_count)\n'
     '                                  .arg(result.streams.size()))\n'
     '                                 .toStdString();'),

    # ---------- ExtractMp4StreamInfo ----------
    ('void ContainerStructureAnalyzer::ExtractMp4StreamInfo(const QVector<model::Mp4BoxNode>& box_tree,',
     'void ContainerStructureAnalyzer::ExtractMp4StreamInfo(const std::vector<model::Mp4BoxNode>& box_tree,'),
    ('    std::function<void(const QVector<model::Mp4BoxNode>&)> walk;\n'
     '    walk = [&](const QVector<model::Mp4BoxNode>& nodes) {',
     '    std::function<void(const std::vector<model::Mp4BoxNode>&)> walk;\n'
     '    walk = [&](const std::vector<model::Mp4BoxNode>& nodes) {'),
    ('                QString handler_type;', '                std::string handler_type;'),
    ('                std::function<void(const QVector<model::Mp4BoxNode>&)> extract_trak;\n'
     '                extract_trak = [&](const QVector<model::Mp4BoxNode>& children) {',
     '                std::function<void(const std::vector<model::Mp4BoxNode>&)> extract_trak;\n'
     '                extract_trak = [&](const std::vector<model::Mp4BoxNode>& children) {'),
    ('                                if (f.name == "track_id") si.details = QString("id=%1").arg(f.value);',
     '                                if (f.name == "track_id") si.details = "id=" + f.value;'),
    ('                                            si.details += QString(" %1x%2").arg(f.value, f2.value);',
     '                                            si.details += " " + f.value + "x" + f2.value;'),
    ('                                    std::function<void(const QVector<model::Mp4BoxNode>&)> find_stsd;\n'
     '                                    find_stsd = [&](const QVector<model::Mp4BoxNode>& stbl_nodes) {',
     '                                    std::function<void(const std::vector<model::Mp4BoxNode>&)> find_stsd;\n'
     '                                    find_stsd = [&](const std::vector<model::Mp4BoxNode>& stbl_nodes) {'),
    ('                                                    QString readable = Mp4FourccToName(codec_node.type);\n'
     '                                                    if (!readable.isEmpty()) {',
     '                                                    std::string readable = Mp4FourccToName(codec_node.type);\n'
     '                                                    if (!readable.empty()) {'),
    ('                                                        if (cf.name == "width" && !cf.value.isEmpty()) {\n'
     '                                                            if (si.details.contains("x")) {\n'
     '                                                                // 替换 tkhd 的粗略尺寸\n'
     '                                                                int idx = si.details.indexOf("x");\n'
     '                                                                int start = si.details.lastIndexOf(" ", idx);\n'
     '                                                                si.details = si.details.left(start + 1) +\n'
     '                                                                             cf.value + "x";',
     '                                                        if (cf.name == "width" && !cf.value.empty()) {\n'
     '                                                            if (si.details.find("x") != std::string::npos) {\n'
     '                                                                // 替换 tkhd 的粗略尺寸\n'
     '                                                                const size_t idx = si.details.find("x");\n'
     '                                                                size_t start = idx;\n'
     '                                                                while (start > 0 && si.details[start - 1] != \' \') {\n'
     '                                                                    --start;\n'
     '                                                                }\n'
     '                                                                si.details = si.details.substr(0, start) +\n'
     '                                                                             cf.value + "x";'),
    ('                                                                si.details += QString(" %1x").arg(cf.value);',
     '                                                                si.details += " " + cf.value + "x";'),
    ('                                                        if (cf.name == "sample_rate" && !cf.value.isEmpty()) {\n'
     '                                                            si.details += QString(" %1Hz").arg(cf.value);\n'
     '                                                        }\n'
     '                                                        if (cf.name == "channel_count" && !cf.value.isEmpty()) {\n'
     '                                                            si.details += QString(" %1ch").arg(cf.value);\n'
     '                                                        }',
     '                                                        if (cf.name == "sample_rate" && !cf.value.empty()) {\n'
     '                                                            si.details += " " + cf.value + "Hz";\n'
     '                                                        }\n'
     '                                                        if (cf.name == "channel_count" && !cf.value.empty()) {\n'
     '                                                            si.details += " " + cf.value + "ch";\n'
     '                                                        }'),
    ('                si.details = si.details.trimmed();\n'
     '                result.streams.append(si);',
     '                si.details = Trimmed(si.details);\n'
     '                result.streams.push_back(si);'),

    # ---------- ExtractEbmlStreamInfo ----------
    ('        // 构建丰富的 details 字符串\n'
     '        QStringList parts;\n'
     '        if (track.track_type == 1) {  // video\n'
     '            if (track.pixel_width > 0 && track.pixel_height > 0) {\n'
     '                parts << QString("%1x%2").arg(track.pixel_width).arg(track.pixel_height);\n'
     '            }\n'
     '            if (track.frame_rate > 0) {\n'
     '                parts << QString("%.2f fps").arg(track.frame_rate);\n'
     '            }\n'
     '        } else if (track.track_type == 2) {  // audio\n'
     '            if (track.sampling_frequency > 0) {\n'
     '                parts << QString("%1 Hz").arg(track.sampling_frequency, 0, \'f\', 0);\n'
     '            }\n'
     '            if (track.channels > 0) {\n'
     '                parts << QString("%1 ch").arg(track.channels);\n'
     '            }\n'
     '            if (track.bit_depth > 0) {\n'
     '                parts << QString("%1 bit").arg(track.bit_depth);\n'
     '            }\n'
     '        }\n'
     '        if (!track.language.isEmpty() && track.language != "und") {\n'
     '            parts << track.language;\n'
     '        }\n'
     '        if (!track.track_name.isEmpty()) {\n'
     '            parts << track.track_name;\n'
     '        }\n'
     '        si.details = parts.join(" ");\n'
     '        result.streams.append(si);',
     '        // 构建丰富的 details 字符串\n'
     '        std::vector<std::string> parts;\n'
     '        if (track.track_type == 1) {  // video\n'
     '            if (track.pixel_width > 0 && track.pixel_height > 0) {\n'
     '                parts.push_back(std::to_string(track.pixel_width) + "x"\n'
     '                                + std::to_string(track.pixel_height));\n'
     '            }\n'
     '            if (track.frame_rate > 0) {\n'
     '                parts.push_back(Format("%.2f fps", track.frame_rate));\n'
     '            }\n'
     '        } else if (track.track_type == 2) {  // audio\n'
     '            if (track.sampling_frequency > 0) {\n'
     '                parts.push_back(std::to_string(static_cast<long long>(track.sampling_frequency))\n'
     '                                + " Hz");\n'
     '            }\n'
     '            if (track.channels > 0) {\n'
     '                parts.push_back(std::to_string(track.channels) + " ch");\n'
     '            }\n'
     '            if (track.bit_depth > 0) {\n'
     '                parts.push_back(std::to_string(track.bit_depth) + " bit");\n'
     '            }\n'
     '        }\n'
     '        if (!track.language.empty() && track.language != "und") {\n'
     '            parts.push_back(track.language);\n'
     '        }\n'
     '        if (!track.track_name.empty()) {\n'
     '            parts.push_back(track.track_name);\n'
     '        }\n'
     '        si.details = Join(parts, " ");\n'
     '        result.streams.push_back(si);'),
    ('    if (!ebml_detail.title.isEmpty()) result.metadata["title"] = ebml_detail.title;\n'
     '    if (!ebml_detail.muxing_app.isEmpty()) result.metadata["muxing_app"] = ebml_detail.muxing_app;\n'
     '    if (!ebml_detail.writing_app.isEmpty()) result.metadata["writing_app"] = ebml_detail.writing_app;\n'
     '    if (ebml_detail.duration_seconds > 0) {\n'
     '        result.metadata["duration"] = QString("%1s").arg(ebml_detail.duration_seconds, 0, \'f\', 2);\n'
     '    }',
     '    if (!ebml_detail.title.empty()) result.metadata["title"] = ebml_detail.title;\n'
     '    if (!ebml_detail.muxing_app.empty()) result.metadata["muxing_app"] = ebml_detail.muxing_app;\n'
     '    if (!ebml_detail.writing_app.empty()) result.metadata["writing_app"] = ebml_detail.writing_app;\n'
     '    if (ebml_detail.duration_seconds > 0) {\n'
     '        result.metadata["duration"] =\n'
     '            QString("%1s").arg(ebml_detail.duration_seconds, 0, \'f\', 2).toStdString();\n'
     '    }'),

    # ---------- ConvertMp4Tree ----------
    ('void ContainerStructureAnalyzer::ConvertMp4Tree(const QVector<model::Mp4BoxNode>& nodes,\n'
     '                                                  int depth,\n'
     '                                                  QVector<model::ContainerElement>& out) {',
     'void ContainerStructureAnalyzer::ConvertMp4Tree(const std::vector<model::Mp4BoxNode>& nodes,\n'
     '                                                  int depth,\n'
     '                                                  std::vector<model::ContainerElement>& out) {'),
    ('        auto isKeyField = [](const QString& n) {', '        auto isKeyField = [](const std::string& n) {'),
    ('        for (const auto& f : node.fields) {\n'
     '            const QString kv = f.name + "=" + f.value;',
     '        for (const auto& f : node.fields) {\n'
     '            const std::string kv = f.name + "=" + f.value;'),
    ('        if (value.size() > 240) value = value.left(237) + "...";\n'
     '        elem.value = value;\n'
     '        elem.extra = all_props;\n'
     '\n'
     '        ConvertMp4Tree(node.children, depth + 1, elem.children);\n'
     '        out.append(elem);',
     '        if (value.size() > 240) value = value.left(237) + "...";\n'
     '        elem.value = value.toStdString();\n'
     '        elem.extra = all_props.toStdString();\n'
     '\n'
     '        ConvertMp4Tree(node.children, depth + 1, elem.children);\n'
     '        out.push_back(elem);'),

    # ---------- ConvertEbmlTree ----------
    ('void ContainerStructureAnalyzer::ConvertEbmlTree(const QVector<model::EbmlElementNode>& nodes,\n'
     '                                                   int depth,\n'
     '                                                   QVector<model::ContainerElement>& out) {',
     'void ContainerStructureAnalyzer::ConvertEbmlTree(const std::vector<model::EbmlElementNode>& nodes,\n'
     '                                                   int depth,\n'
     '                                                   std::vector<model::ContainerElement>& out) {'),
    ('        ConvertEbmlTree(node.children, depth + 1, elem.children);\n'
     '        out.append(elem);',
     '        ConvertEbmlTree(node.children, depth + 1, elem.children);\n'
     '        out.push_back(elem);'),

    # ---------- AnalyzeWithFFmpeg ----------
    ('    result.file_path = file_path;\n\n'
     '    // 构建通用结构树\n'
     '    model::ContainerElement root;\n'
     '    root.name = QString("%1 Container").arg(result.format_name.toUpper());',
     '    result.file_path = file_path.toStdString();\n\n'
     '    // 构建通用结构树\n'
     '    model::ContainerElement root;\n'
     '    root.name = QString("%1 Container")\n'
     '                    .arg(QString::fromStdString(result.format_name).toUpper())\n'
     '                    .toStdString();'),
    ('        stream_elem.name = QString("Stream #%1 (%2)").arg(i).arg(codec_type_name ? codec_type_name : "unknown");',
     '        stream_elem.name =\n'
     '            QString("Stream #%1 (%2)")\n'
     '                .arg(i)\n'
     '                .arg(codec_type_name ? codec_type_name : "unknown")\n'
     '                .toStdString();'),
    ('        stream_elem.value = QString("codec=%1").arg(codec_name ? codec_name : "?");',
     '        stream_elem.value =\n'
     '            QString("codec=%1").arg(codec_name ? codec_name : "?").toStdString();'),
    ('            si.details = QString("%1x%2").arg(st->codecpar->width).arg(st->codecpar->height);',
     '            si.details =\n'
     '                QString("%1x%2")\n'
     '                    .arg(st->codecpar->width)\n'
     '                    .arg(st->codecpar->height)\n'
     '                    .toStdString();'),
    ('            si.details = QString("%1 Hz, %2 ch").arg(st->codecpar->sample_rate).arg(st->codecpar->ch_layout.nb_channels);',
     '            si.details = QString("%1 Hz, %2 ch")\n'
     '                            .arg(st->codecpar->sample_rate)\n'
     '                            .arg(st->codecpar->ch_layout.nb_channels)\n'
     '                            .toStdString();'),
    ('        result.metadata[QString::fromUtf8(tag->key)] = QString::fromUtf8(tag->value);',
     '        result.metadata[tag->key] = tag->value;'),
    ('        meta_elem.name = QString::fromUtf8(tag->key);', '        meta_elem.name = tag->key;'),
    ('        meta_elem.value = QString::fromUtf8(tag->value);', '        meta_elem.value = tag->value;'),
    ('        ch_elem.name = QString("Chapter #%1").arg(i);',
     '        ch_elem.name = "Chapter #" + std::to_string(i);'),
    ('        ch_elem.value = QString("start=%.2fs end=%.2fs").arg(start_sec).arg(end_sec);',
     '        ch_elem.value =\n'
     '            QString("start=%.2fs end=%.2fs").arg(start_sec).arg(end_sec).toStdString();'),
    ('            result.metadata[QString("chapter%1_%2").arg(i).arg(ch_tag->key)] =\n'
     '                QString::fromUtf8(ch_tag->value);',
     '            result.metadata["chapter" + std::to_string(i) + "_" + ch_tag->key] = ch_tag->value;'),
    ('    result.element_tree.append(root);\n'
     '    result.valid = true;\n'
     '    result.summary = QString("%1 | %2 流 | %3 章节")\n'
     '                         .arg(result.format_name.toUpper())\n'
     '                         .arg(fmt_ctx->nb_streams)\n'
     '                         .arg(fmt_ctx->nb_chapters);',
     '    result.element_tree.push_back(root);\n'
     '    result.valid = true;\n'
     '    result.summary = (QString("%1 | %2 流 | %3 章节")\n'
     '                          .arg(QString::fromStdString(result.format_name).toUpper())\n'
     '                          .arg(fmt_ctx->nb_streams)\n'
     '                          .arg(fmt_ctx->nb_chapters))\n'
     '                         .toStdString();'),

    # ---------- AnalyzeStreamingManifest ----------
    ('        result.error_message = QString::fromStdString(pkg.error_message);',
     '        result.error_message = pkg.error_message.toStdString();'),
    ('        seg.container_error = ts_result.error_message.isEmpty()',
     '        seg.container_error = ts_result.error_message.empty()'),

    # ---------- BuildStreamingTree ----------
    ('    auto make_elem = [](const QString& name, const QString& type, int depth,\n'
     '                        const QString& value = QString(),\n'
     '                        const QString& extra = QString()) {\n'
     '        model::ContainerElement e;\n'
     '        e.name = name;\n'
     '        e.type = type;\n'
     '        e.depth = depth;\n'
     '        e.value = value;\n'
     '        e.extra = extra;\n'
     '        return e;\n'
     '    };',
     '    // 入参保持 QString（调用方大量用 .arg() 拼装），落地到 domain 时统一转 std::string\n'
     '    auto make_elem = [](const QString& name, const QString& type, int depth,\n'
     '                        const QString& value = QString(),\n'
     '                        const QString& extra = QString()) {\n'
     '        model::ContainerElement e;\n'
     '        e.name = name.toStdString();\n'
     '        e.type = type.toStdString();\n'
     '        e.depth = depth;\n'
     '        e.value = value.toStdString();\n'
     '        e.extra = extra.toStdString();\n'
     '        return e;\n'
     '    };'),
    ('    root.extra = QString::fromStdString(pkg.manifest_path);',
     '    root.extra = pkg.manifest_path.toStdString();'),
    ('        root.value = QString("%1 | %2 Period | %3 Representation")\n'
     '                         .arg(QString::fromStdString(pkg.mpd_type))\n'
     '                         .arg(pkg.periods.size())\n'
     '                         .arg(pkg.representations.size());',
     '        root.value = (QString("%1 | %2 Period | %3 Representation")\n'
     '                          .arg(QString::fromStdString(pkg.mpd_type))\n'
     '                          .arg(pkg.periods.size())\n'
     '                          .arg(pkg.representations.size()))\n'
     '                         .toStdString();'),
    ('            si.codec = QString::fromStdString(e.video_codec);\n'
     '            si.details = QString("%1x%2 %3 kbps")\n'
     '                             .arg(e.width)\n'
     '                             .arg(e.height)\n'
     '                             .arg(e.bandwidth_bps / 1000);\n'
     '            result.streams.append(si);',
     '            si.codec = e.video_codec.toStdString();\n'
     '            si.details = (QString("%1x%2 %3 kbps")\n'
     '                             .arg(e.width)\n'
     '                             .arg(e.height)\n'
     '                             .arg(e.bandwidth_bps / 1000))\n'
     '                             .toStdString();\n'
     '            result.streams.push_back(si);'),
    ('            si.codec = QString::fromStdString(rep.audio_codec);\n'
     '            si.details = QString("%1 kbps").arg(rep.bandwidth_bps / 1000);\n'
     '            result.streams.append(si);',
     '            si.codec = rep.audio_codec.toStdString();\n'
     '            si.details =\n'
     '                QString("%1 kbps").arg(rep.bandwidth_bps / 1000).toStdString();\n'
     '            result.streams.push_back(si);'),
    ('        result.metadata["MPD type"] = QString::fromStdString(pkg.mpd_type);\n'
     '        if (pkg.media_presentation_duration_s > 0.0) {\n'
     '            result.metadata["时长"] =\n'
     '                QString("%1 s").arg(pkg.media_presentation_duration_s, 0, \'f\', 3);\n'
     '        }\n'
     '        result.summary = QString("DASH | %1 Period | %2 Representation | %3 分片")\n'
     '                             .arg(pkg.periods.size())\n'
     '                             .arg(pkg.representations.size())\n'
     '                             .arg(static_cast<qulonglong>(pkg.TotalSegments()));',
     '        result.metadata["MPD type"] = pkg.mpd_type.toStdString();\n'
     '        if (pkg.media_presentation_duration_s > 0.0) {\n'
     '            result.metadata["时长"] =\n'
     '                QString("%1 s")\n'
     '                    .arg(pkg.media_presentation_duration_s, 0, \'f\', 3)\n'
     '                    .toStdString();\n'
     '        }\n'
     '        result.summary = (QString("DASH | %1 Period | %2 Representation | %3 分片")\n'
     '                              .arg(pkg.periods.size())\n'
     '                              .arg(pkg.representations.size())\n'
     '                              .arg(static_cast<qulonglong>(pkg.TotalSegments())))\n'
     '                             .toStdString();'),
    ('        root.value = QString("%1 | %2 variant | %3 playlist")\n'
     '                         .arg(pkg.kind == model::StreamingKind::HlsMaster ? "master" : "media")\n'
     '                         .arg(pkg.variants.size())\n'
     '                         .arg(pkg.playlists.size());',
     '        root.value = (QString("%1 | %2 variant | %3 playlist")\n'
     '                          .arg(pkg.kind == model::StreamingKind::HlsMaster ? "master" : "media")\n'
     '                          .arg(pkg.variants.size())\n'
     '                          .arg(pkg.playlists.size()))\n'
     '                         .toStdString();'),
    ('            v_elem.extra = QString::fromStdString(v.uri + "  codecs=" + v.codecs);\n'
     '            variants_elem.children.append(v_elem);',
     '            v_elem.extra = (v.uri + "  codecs=" + v.codecs).toStdString();\n'
     '            variants_elem.children.push_back(v_elem);'),
    ('            root.children.append(rend_elem);', '            root.children.push_back(rend_elem);'),
    ('            si.codec = QString::fromStdString(e.video_codec.empty() ? e.audio_codec : e.video_codec);\n'
     '            si.details = QString("%1 kbps %2")\n'
     '                             .arg(e.bandwidth_bps / 1000)\n'
     '                             .arg(e.width > 0 ? QString("%1x%2").arg(e.width).arg(e.height)\n'
     '                                              : QString("audio"));\n'
     '            result.streams.append(si);',
     '            si.codec = (e.video_codec.empty() ? e.audio_codec : e.video_codec).toStdString();\n'
     '            si.details = (QString("%1 kbps %2")\n'
     '                             .arg(e.bandwidth_bps / 1000)\n'
     '                             .arg(e.width > 0 ? QString("%1x%2")\n'
     '                                                     .arg(e.width)\n'
     '                                                     .arg(e.height)\n'
     '                                              : QString("audio")))\n'
     '                             .toStdString();\n'
     '            result.streams.push_back(si);'),
    ('                result.metadata[QString("EXT-X-TARGETDURATION #%1").arg(pl.index)] =\n'
     '                    QString("%1 s").arg(pl.target_duration_s);\n'
     '            }\n'
     '            if (pl.encrypted) {\n'
     '                result.metadata[QString("加密 #%1").arg(pl.index)] =\n'
     '                    QString::fromStdString(pl.key_method);\n'
     '            }',
     '                result.metadata["EXT-X-TARGETDURATION #" + std::to_string(pl.index)] =\n'
     '                    QString("%1 s").arg(pl.target_duration_s).toStdString();\n'
     '            }\n'
     '            if (pl.encrypted) {\n'
     '                result.metadata["加密 #" + std::to_string(pl.index)] =\n'
     '                    pl.key_method.toStdString();\n'
     '            }'),
    ('        result.summary = QString("HLS | %1 variant | %2 playlist | %3 分片")\n'
     '                             .arg(pkg.variants.size())\n'
     '                             .arg(pkg.playlists.size())\n'
     '                             .arg(static_cast<qulonglong>(pkg.TotalSegments()));',
     '        result.summary = (QString("HLS | %1 variant | %2 playlist | %3 分片")\n'
     '                              .arg(pkg.variants.size())\n'
     '                              .arg(pkg.playlists.size())\n'
     '                              .arg(static_cast<qulonglong>(pkg.TotalSegments())))\n'
     '                             .toStdString();'),
    ('    result.element_tree.append(root);\n}',
     '    result.element_tree.push_back(root);\n}'),

    # ---------- 零散 append -> push_back ----------
]

missing = []
for a, b in R:
    if a not in s:
        missing.append(a.strip().split("\n")[0][:70])
    else:
        s = s.replace(a, b, 1)

if missing:
    print("MISSING (%d):" % len(missing))
    for m in missing:
        print("   ", m)
    raise SystemExit(1)

# 补 <vector>/<string> 头
if '#include <vector>' not in s:
    s = s.replace('#include <QStringList>\n',
                  '#include <QStringList>\n\n#include <string>\n#include <vector>\n', 1)

# domain 容器上的 Qt append -> push_back（统一收尾）
SWEEP = [
    ('.children.append(', '.children.push_back('),
    ('.element_tree.append(', '.element_tree.push_back('),
    ('.streams.append(si);', '.streams.push_back(si);'),
    ('out.append(elem);', 'out.push_back(elem);'),
]
for a, b in SWEEP:
    s = s.replace(a, b)

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("OK, len", len(orig), "->", len(s))
