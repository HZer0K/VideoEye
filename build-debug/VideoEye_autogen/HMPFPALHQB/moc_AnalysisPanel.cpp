/****************************************************************************
** Meta object code from reading C++ file 'AnalysisPanel.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.4.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../ui/analysis_panel/AnalysisPanel.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'AnalysisPanel.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.4.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
namespace {
struct qt_meta_stringdata_videoeye__ui__AnalysisPanel_t {
    uint offsetsAndSizes[90];
    char stringdata0[28];
    char stringdata1[18];
    char stringdata2[1];
    char stringdata3[22];
    char stringdata4[6];
    char stringdata5[16];
    char stringdata6[24];
    char stringdata7[5];
    char stringdata8[20];
    char stringdata9[32];
    char stringdata10[6];
    char stringdata11[20];
    char stringdata12[21];
    char stringdata13[6];
    char stringdata14[11];
    char stringdata15[13];
    char stringdata16[4];
    char stringdata17[18];
    char stringdata18[20];
    char stringdata19[21];
    char stringdata20[13];
    char stringdata21[12];
    char stringdata22[9];
    char stringdata23[11];
    char stringdata24[16];
    char stringdata25[17];
    char stringdata26[18];
    char stringdata27[12];
    char stringdata28[23];
    char stringdata29[20];
    char stringdata30[21];
    char stringdata31[11];
    char stringdata32[20];
    char stringdata33[17];
    char stringdata34[18];
    char stringdata35[7];
    char stringdata36[23];
    char stringdata37[20];
    char stringdata38[21];
    char stringdata39[6];
    char stringdata40[24];
    char stringdata41[25];
    char stringdata42[31];
    char stringdata43[6];
    char stringdata44[15];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_videoeye__ui__AnalysisPanel_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_videoeye__ui__AnalysisPanel_t qt_meta_stringdata_videoeye__ui__AnalysisPanel = {
    {
        QT_MOC_LITERAL(0, 27),  // "videoeye::ui::AnalysisPanel"
        QT_MOC_LITERAL(28, 17),  // "UpdateStreamStats"
        QT_MOC_LITERAL(46, 0),  // ""
        QT_MOC_LITERAL(47, 21),  // "analyzer::StreamStats"
        QT_MOC_LITERAL(69, 5),  // "stats"
        QT_MOC_LITERAL(75, 15),  // "UpdateHistogram"
        QT_MOC_LITERAL(91, 23),  // "analyzer::HistogramData"
        QT_MOC_LITERAL(115, 4),  // "hist"
        QT_MOC_LITERAL(120, 19),  // "UpdateFaceDetection"
        QT_MOC_LITERAL(140, 31),  // "std::vector<analyzer::FaceInfo>"
        QT_MOC_LITERAL(172, 5),  // "faces"
        QT_MOC_LITERAL(178, 19),  // "ResetVideoFrameList"
        QT_MOC_LITERAL(198, 20),  // "AppendVideoFrameInfo"
        QT_MOC_LITERAL(219, 5),  // "index"
        QT_MOC_LITERAL(225, 10),  // "frame_type"
        QT_MOC_LITERAL(236, 12),  // "is_key_frame"
        QT_MOC_LITERAL(249, 3),  // "pts"
        QT_MOC_LITERAL(253, 17),  // "timestamp_seconds"
        QT_MOC_LITERAL(271, 19),  // "ResetAudioFrameList"
        QT_MOC_LITERAL(291, 20),  // "AppendAudioFrameInfo"
        QT_MOC_LITERAL(312, 12),  // "sample_count"
        QT_MOC_LITERAL(325, 11),  // "sample_rate"
        QT_MOC_LITERAL(337, 8),  // "channels"
        QT_MOC_LITERAL(346, 10),  // "byte_count"
        QT_MOC_LITERAL(357, 15),  // "ResetPacketList"
        QT_MOC_LITERAL(373, 16),  // "AppendPacketInfo"
        QT_MOC_LITERAL(390, 17),  // "model::PacketInfo"
        QT_MOC_LITERAL(408, 11),  // "packet_info"
        QT_MOC_LITERAL(420, 22),  // "ResetAnalysisEventList"
        QT_MOC_LITERAL(443, 19),  // "AppendAnalysisEvent"
        QT_MOC_LITERAL(463, 20),  // "model::AnalysisEvent"
        QT_MOC_LITERAL(484, 10),  // "event_info"
        QT_MOC_LITERAL(495, 19),  // "ResetSyncSampleList"
        QT_MOC_LITERAL(515, 16),  // "AppendSyncSample"
        QT_MOC_LITERAL(532, 17),  // "model::SyncSample"
        QT_MOC_LITERAL(550, 6),  // "sample"
        QT_MOC_LITERAL(557, 22),  // "ResetTimelineEventList"
        QT_MOC_LITERAL(580, 19),  // "AppendTimelineEvent"
        QT_MOC_LITERAL(600, 20),  // "model::TimelineEvent"
        QT_MOC_LITERAL(621, 5),  // "event"
        QT_MOC_LITERAL(627, 23),  // "ResetAudioVisualization"
        QT_MOC_LITERAL(651, 24),  // "AppendAudioVisualization"
        QT_MOC_LITERAL(676, 30),  // "model::AudioVisualizationFrame"
        QT_MOC_LITERAL(707, 5),  // "frame"
        QT_MOC_LITERAL(713, 14)   // "OnExportReport"
    },
    "videoeye::ui::AnalysisPanel",
    "UpdateStreamStats",
    "",
    "analyzer::StreamStats",
    "stats",
    "UpdateHistogram",
    "analyzer::HistogramData",
    "hist",
    "UpdateFaceDetection",
    "std::vector<analyzer::FaceInfo>",
    "faces",
    "ResetVideoFrameList",
    "AppendVideoFrameInfo",
    "index",
    "frame_type",
    "is_key_frame",
    "pts",
    "timestamp_seconds",
    "ResetAudioFrameList",
    "AppendAudioFrameInfo",
    "sample_count",
    "sample_rate",
    "channels",
    "byte_count",
    "ResetPacketList",
    "AppendPacketInfo",
    "model::PacketInfo",
    "packet_info",
    "ResetAnalysisEventList",
    "AppendAnalysisEvent",
    "model::AnalysisEvent",
    "event_info",
    "ResetSyncSampleList",
    "AppendSyncSample",
    "model::SyncSample",
    "sample",
    "ResetTimelineEventList",
    "AppendTimelineEvent",
    "model::TimelineEvent",
    "event",
    "ResetAudioVisualization",
    "AppendAudioVisualization",
    "model::AudioVisualizationFrame",
    "frame",
    "OnExportReport"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_videoeye__ui__AnalysisPanel[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
      18,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    1,  122,    2, 0x0a,    1 /* Public */,
       5,    1,  125,    2, 0x0a,    3 /* Public */,
       8,    1,  128,    2, 0x0a,    5 /* Public */,
      11,    0,  131,    2, 0x0a,    7 /* Public */,
      12,    5,  132,    2, 0x0a,    8 /* Public */,
      18,    0,  143,    2, 0x0a,   14 /* Public */,
      19,    7,  144,    2, 0x0a,   15 /* Public */,
      24,    0,  159,    2, 0x0a,   23 /* Public */,
      25,    1,  160,    2, 0x0a,   24 /* Public */,
      28,    0,  163,    2, 0x0a,   26 /* Public */,
      29,    1,  164,    2, 0x0a,   27 /* Public */,
      32,    0,  167,    2, 0x0a,   29 /* Public */,
      33,    1,  168,    2, 0x0a,   30 /* Public */,
      36,    0,  171,    2, 0x0a,   32 /* Public */,
      37,    1,  172,    2, 0x0a,   33 /* Public */,
      40,    0,  175,    2, 0x0a,   35 /* Public */,
      41,    1,  176,    2, 0x0a,   36 /* Public */,
      44,    0,  179,    2, 0x0a,   38 /* Public */,

 // slots: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 6,    7,
    QMetaType::Void, 0x80000000 | 9,   10,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::Int, QMetaType::Bool, QMetaType::LongLong, QMetaType::Double,   13,   14,   15,   16,   17,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::LongLong, QMetaType::Double, QMetaType::Int, QMetaType::Int, QMetaType::Int, QMetaType::Int,   13,   16,   17,   20,   21,   22,   23,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 26,   27,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 30,   31,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 34,   35,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 38,   39,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 42,   43,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject videoeye::ui::AnalysisPanel::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_videoeye__ui__AnalysisPanel.offsetsAndSizes,
    qt_meta_data_videoeye__ui__AnalysisPanel,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_videoeye__ui__AnalysisPanel_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<AnalysisPanel, std::true_type>,
        // method 'UpdateStreamStats'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const analyzer::StreamStats &, std::false_type>,
        // method 'UpdateHistogram'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const analyzer::HistogramData &, std::false_type>,
        // method 'UpdateFaceDetection'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::vector<analyzer::FaceInfo> &, std::false_type>,
        // method 'ResetVideoFrameList'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AppendVideoFrameInfo'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'ResetAudioFrameList'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AppendAudioFrameInfo'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'ResetPacketList'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AppendPacketInfo'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const model::PacketInfo &, std::false_type>,
        // method 'ResetAnalysisEventList'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AppendAnalysisEvent'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const model::AnalysisEvent &, std::false_type>,
        // method 'ResetSyncSampleList'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AppendSyncSample'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const model::SyncSample &, std::false_type>,
        // method 'ResetTimelineEventList'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AppendTimelineEvent'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const model::TimelineEvent &, std::false_type>,
        // method 'ResetAudioVisualization'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AppendAudioVisualization'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const model::AudioVisualizationFrame &, std::false_type>,
        // method 'OnExportReport'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void videoeye::ui::AnalysisPanel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<AnalysisPanel *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->UpdateStreamStats((*reinterpret_cast< std::add_pointer_t<analyzer::StreamStats>>(_a[1]))); break;
        case 1: _t->UpdateHistogram((*reinterpret_cast< std::add_pointer_t<analyzer::HistogramData>>(_a[1]))); break;
        case 2: _t->UpdateFaceDetection((*reinterpret_cast< std::add_pointer_t<std::vector<analyzer::FaceInfo>>>(_a[1]))); break;
        case 3: _t->ResetVideoFrameList(); break;
        case 4: _t->AppendVideoFrameInfo((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<double>>(_a[5]))); break;
        case 5: _t->ResetAudioFrameList(); break;
        case 6: _t->AppendAudioFrameInfo((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<double>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[5])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[6])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[7]))); break;
        case 7: _t->ResetPacketList(); break;
        case 8: _t->AppendPacketInfo((*reinterpret_cast< std::add_pointer_t<model::PacketInfo>>(_a[1]))); break;
        case 9: _t->ResetAnalysisEventList(); break;
        case 10: _t->AppendAnalysisEvent((*reinterpret_cast< std::add_pointer_t<model::AnalysisEvent>>(_a[1]))); break;
        case 11: _t->ResetSyncSampleList(); break;
        case 12: _t->AppendSyncSample((*reinterpret_cast< std::add_pointer_t<model::SyncSample>>(_a[1]))); break;
        case 13: _t->ResetTimelineEventList(); break;
        case 14: _t->AppendTimelineEvent((*reinterpret_cast< std::add_pointer_t<model::TimelineEvent>>(_a[1]))); break;
        case 15: _t->ResetAudioVisualization(); break;
        case 16: _t->AppendAudioVisualization((*reinterpret_cast< std::add_pointer_t<model::AudioVisualizationFrame>>(_a[1]))); break;
        case 17: _t->OnExportReport(); break;
        default: ;
        }
    }
}

const QMetaObject *videoeye::ui::AnalysisPanel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *videoeye::ui::AnalysisPanel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_videoeye__ui__AnalysisPanel.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int videoeye::ui::AnalysisPanel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 18)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 18;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 18)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 18;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
