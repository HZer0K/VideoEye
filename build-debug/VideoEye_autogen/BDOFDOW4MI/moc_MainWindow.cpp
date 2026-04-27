/****************************************************************************
** Meta object code from reading C++ file 'MainWindow.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.4.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../ui/main_window/MainWindow.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'MainWindow.h' doesn't include <QObject>."
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
struct qt_meta_stringdata_videoeye__ui__MainWindow_t {
    uint offsetsAndSizes[86];
    char stringdata0[25];
    char stringdata1[11];
    char stringdata2[1];
    char stringdata3[10];
    char stringdata4[7];
    char stringdata5[20];
    char stringdata6[15];
    char stringdata7[15];
    char stringdata8[7];
    char stringdata9[8];
    char stringdata10[7];
    char stringdata11[7];
    char stringdata12[6];
    char stringdata13[15];
    char stringdata14[19];
    char stringdata15[6];
    char stringdata16[13];
    char stringdata17[6];
    char stringdata18[18];
    char stringdata19[12];
    char stringdata20[12];
    char stringdata21[8];
    char stringdata22[8];
    char stringdata23[19];
    char stringdata24[20];
    char stringdata25[22];
    char stringdata26[6];
    char stringdata27[18];
    char stringdata28[24];
    char stringdata29[5];
    char stringdata30[22];
    char stringdata31[32];
    char stringdata32[6];
    char stringdata33[19];
    char stringdata34[10];
    char stringdata35[18];
    char stringdata36[6];
    char stringdata37[18];
    char stringdata38[27];
    char stringdata39[16];
    char stringdata40[27];
    char stringdata41[11];
    char stringdata42[24];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_videoeye__ui__MainWindow_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_videoeye__ui__MainWindow_t qt_meta_stringdata_videoeye__ui__MainWindow = {
    {
        QT_MOC_LITERAL(0, 24),  // "videoeye::ui::MainWindow"
        QT_MOC_LITERAL(25, 10),  // "OnOpenFile"
        QT_MOC_LITERAL(36, 0),  // ""
        QT_MOC_LITERAL(37, 9),  // "OnOpenURL"
        QT_MOC_LITERAL(47, 6),  // "OnExit"
        QT_MOC_LITERAL(54, 19),  // "OnExportVideoFrames"
        QT_MOC_LITERAL(74, 14),  // "OnPrevRawFrame"
        QT_MOC_LITERAL(89, 14),  // "OnNextRawFrame"
        QT_MOC_LITERAL(104, 6),  // "OnPlay"
        QT_MOC_LITERAL(111, 7),  // "OnPause"
        QT_MOC_LITERAL(119, 6),  // "OnStop"
        QT_MOC_LITERAL(126, 6),  // "OnSeek"
        QT_MOC_LITERAL(133, 5),  // "value"
        QT_MOC_LITERAL(139, 14),  // "OnStateChanged"
        QT_MOC_LITERAL(154, 18),  // "model::PlayerState"
        QT_MOC_LITERAL(173, 5),  // "state"
        QT_MOC_LITERAL(179, 12),  // "OnFrameReady"
        QT_MOC_LITERAL(192, 5),  // "frame"
        QT_MOC_LITERAL(198, 17),  // "OnPositionChanged"
        QT_MOC_LITERAL(216, 11),  // "position_ms"
        QT_MOC_LITERAL(228, 11),  // "duration_ms"
        QT_MOC_LITERAL(240, 7),  // "OnError"
        QT_MOC_LITERAL(248, 7),  // "message"
        QT_MOC_LITERAL(256, 18),  // "OnPlaybackFinished"
        QT_MOC_LITERAL(275, 19),  // "OnStreamStatsUpdate"
        QT_MOC_LITERAL(295, 21),  // "analyzer::StreamStats"
        QT_MOC_LITERAL(317, 5),  // "stats"
        QT_MOC_LITERAL(323, 17),  // "OnHistogramUpdate"
        QT_MOC_LITERAL(341, 23),  // "analyzer::HistogramData"
        QT_MOC_LITERAL(365, 4),  // "hist"
        QT_MOC_LITERAL(370, 21),  // "OnFaceDetectionUpdate"
        QT_MOC_LITERAL(392, 31),  // "std::vector<analyzer::FaceInfo>"
        QT_MOC_LITERAL(424, 5),  // "faces"
        QT_MOC_LITERAL(430, 18),  // "OnMediaModeChanged"
        QT_MOC_LITERAL(449, 9),  // "has_video"
        QT_MOC_LITERAL(459, 17),  // "OnAudioLevelReady"
        QT_MOC_LITERAL(477, 5),  // "level"
        QT_MOC_LITERAL(483, 17),  // "timestamp_seconds"
        QT_MOC_LITERAL(501, 26),  // "OnVideoFrameExportProgress"
        QT_MOC_LITERAL(528, 15),  // "exported_frames"
        QT_MOC_LITERAL(544, 26),  // "OnVideoFrameExportFinished"
        QT_MOC_LITERAL(571, 10),  // "output_dir"
        QT_MOC_LITERAL(582, 23)   // "OnVideoFrameExportError"
    },
    "videoeye::ui::MainWindow",
    "OnOpenFile",
    "",
    "OnOpenURL",
    "OnExit",
    "OnExportVideoFrames",
    "OnPrevRawFrame",
    "OnNextRawFrame",
    "OnPlay",
    "OnPause",
    "OnStop",
    "OnSeek",
    "value",
    "OnStateChanged",
    "model::PlayerState",
    "state",
    "OnFrameReady",
    "frame",
    "OnPositionChanged",
    "position_ms",
    "duration_ms",
    "OnError",
    "message",
    "OnPlaybackFinished",
    "OnStreamStatsUpdate",
    "analyzer::StreamStats",
    "stats",
    "OnHistogramUpdate",
    "analyzer::HistogramData",
    "hist",
    "OnFaceDetectionUpdate",
    "std::vector<analyzer::FaceInfo>",
    "faces",
    "OnMediaModeChanged",
    "has_video",
    "OnAudioLevelReady",
    "level",
    "timestamp_seconds",
    "OnVideoFrameExportProgress",
    "exported_frames",
    "OnVideoFrameExportFinished",
    "output_dir",
    "OnVideoFrameExportError"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_videoeye__ui__MainWindow[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
      23,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,  152,    2, 0x08,    1 /* Private */,
       3,    0,  153,    2, 0x08,    2 /* Private */,
       4,    0,  154,    2, 0x08,    3 /* Private */,
       5,    0,  155,    2, 0x08,    4 /* Private */,
       6,    0,  156,    2, 0x08,    5 /* Private */,
       7,    0,  157,    2, 0x08,    6 /* Private */,
       8,    0,  158,    2, 0x08,    7 /* Private */,
       9,    0,  159,    2, 0x08,    8 /* Private */,
      10,    0,  160,    2, 0x08,    9 /* Private */,
      11,    1,  161,    2, 0x08,   10 /* Private */,
      13,    1,  164,    2, 0x08,   12 /* Private */,
      16,    1,  167,    2, 0x08,   14 /* Private */,
      18,    2,  170,    2, 0x08,   16 /* Private */,
      21,    1,  175,    2, 0x08,   19 /* Private */,
      23,    0,  178,    2, 0x08,   21 /* Private */,
      24,    1,  179,    2, 0x08,   22 /* Private */,
      27,    1,  182,    2, 0x08,   24 /* Private */,
      30,    1,  185,    2, 0x08,   26 /* Private */,
      33,    1,  188,    2, 0x08,   28 /* Private */,
      35,    2,  191,    2, 0x08,   30 /* Private */,
      38,    1,  196,    2, 0x08,   33 /* Private */,
      40,    1,  199,    2, 0x08,   35 /* Private */,
      42,    1,  202,    2, 0x08,   37 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   12,
    QMetaType::Void, 0x80000000 | 14,   15,
    QMetaType::Void, QMetaType::QImage,   17,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,   19,   20,
    QMetaType::Void, QMetaType::QString,   22,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 25,   26,
    QMetaType::Void, 0x80000000 | 28,   29,
    QMetaType::Void, 0x80000000 | 31,   32,
    QMetaType::Void, QMetaType::Bool,   34,
    QMetaType::Void, QMetaType::Double, QMetaType::Double,   36,   37,
    QMetaType::Void, QMetaType::Int,   39,
    QMetaType::Void, QMetaType::QString,   41,
    QMetaType::Void, QMetaType::QString,   22,

       0        // eod
};

Q_CONSTINIT const QMetaObject videoeye::ui::MainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_videoeye__ui__MainWindow.offsetsAndSizes,
    qt_meta_data_videoeye__ui__MainWindow,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_videoeye__ui__MainWindow_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<MainWindow, std::true_type>,
        // method 'OnOpenFile'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnOpenURL'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnExit'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnExportVideoFrames'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnPrevRawFrame'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnNextRawFrame'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnPlay'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnPause'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnStop'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnSeek'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'OnStateChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<model::PlayerState, std::false_type>,
        // method 'OnFrameReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QImage &, std::false_type>,
        // method 'OnPositionChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'OnError'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'OnPlaybackFinished'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'OnStreamStatsUpdate'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const analyzer::StreamStats &, std::false_type>,
        // method 'OnHistogramUpdate'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const analyzer::HistogramData &, std::false_type>,
        // method 'OnFaceDetectionUpdate'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::vector<analyzer::FaceInfo> &, std::false_type>,
        // method 'OnMediaModeChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'OnAudioLevelReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'OnVideoFrameExportProgress'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'OnVideoFrameExportFinished'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'OnVideoFrameExportError'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>
    >,
    nullptr
} };

void videoeye::ui::MainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MainWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->OnOpenFile(); break;
        case 1: _t->OnOpenURL(); break;
        case 2: _t->OnExit(); break;
        case 3: _t->OnExportVideoFrames(); break;
        case 4: _t->OnPrevRawFrame(); break;
        case 5: _t->OnNextRawFrame(); break;
        case 6: _t->OnPlay(); break;
        case 7: _t->OnPause(); break;
        case 8: _t->OnStop(); break;
        case 9: _t->OnSeek((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 10: _t->OnStateChanged((*reinterpret_cast< std::add_pointer_t<model::PlayerState>>(_a[1]))); break;
        case 11: _t->OnFrameReady((*reinterpret_cast< std::add_pointer_t<QImage>>(_a[1]))); break;
        case 12: _t->OnPositionChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 13: _t->OnError((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 14: _t->OnPlaybackFinished(); break;
        case 15: _t->OnStreamStatsUpdate((*reinterpret_cast< std::add_pointer_t<analyzer::StreamStats>>(_a[1]))); break;
        case 16: _t->OnHistogramUpdate((*reinterpret_cast< std::add_pointer_t<analyzer::HistogramData>>(_a[1]))); break;
        case 17: _t->OnFaceDetectionUpdate((*reinterpret_cast< std::add_pointer_t<std::vector<analyzer::FaceInfo>>>(_a[1]))); break;
        case 18: _t->OnMediaModeChanged((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 19: _t->OnAudioLevelReady((*reinterpret_cast< std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<double>>(_a[2]))); break;
        case 20: _t->OnVideoFrameExportProgress((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 21: _t->OnVideoFrameExportFinished((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 22: _t->OnVideoFrameExportError((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
}

const QMetaObject *videoeye::ui::MainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *videoeye::ui::MainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_videoeye__ui__MainWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int videoeye::ui::MainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 23)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 23;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 23)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 23;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
