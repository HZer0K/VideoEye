/****************************************************************************
** Meta object code from reading C++ file 'MediaPlayer.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.4.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../core/player/MediaPlayer.h"
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'MediaPlayer.h' doesn't include <QObject>."
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
struct qt_meta_stringdata_videoeye__player__MediaPlayer_t {
    uint offsetsAndSizes[94];
    char stringdata0[30];
    char stringdata1[13];
    char stringdata2[1];
    char stringdata3[19];
    char stringdata4[6];
    char stringdata5[11];
    char stringdata6[6];
    char stringdata7[16];
    char stringdata8[12];
    char stringdata9[12];
    char stringdata10[6];
    char stringdata11[8];
    char stringdata12[17];
    char stringdata13[17];
    char stringdata14[22];
    char stringdata15[6];
    char stringdata16[15];
    char stringdata17[24];
    char stringdata18[5];
    char stringdata19[19];
    char stringdata20[32];
    char stringdata21[6];
    char stringdata22[20];
    char stringdata23[20];
    char stringdata24[6];
    char stringdata25[11];
    char stringdata26[13];
    char stringdata27[4];
    char stringdata28[18];
    char stringdata29[20];
    char stringdata30[20];
    char stringdata31[13];
    char stringdata32[12];
    char stringdata33[9];
    char stringdata34[11];
    char stringdata35[17];
    char stringdata36[10];
    char stringdata37[16];
    char stringdata38[6];
    char stringdata39[24];
    char stringdata40[13];
    char stringdata41[25];
    char stringdata42[16];
    char stringdata43[25];
    char stringdata44[11];
    char stringdata45[25];
    char stringdata46[22];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_videoeye__player__MediaPlayer_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_videoeye__player__MediaPlayer_t qt_meta_stringdata_videoeye__player__MediaPlayer = {
    {
        QT_MOC_LITERAL(0, 29),  // "videoeye::player::MediaPlayer"
        QT_MOC_LITERAL(30, 12),  // "StateChanged"
        QT_MOC_LITERAL(43, 0),  // ""
        QT_MOC_LITERAL(44, 18),  // "model::PlayerState"
        QT_MOC_LITERAL(63, 5),  // "state"
        QT_MOC_LITERAL(69, 10),  // "FrameReady"
        QT_MOC_LITERAL(80, 5),  // "frame"
        QT_MOC_LITERAL(86, 15),  // "PositionChanged"
        QT_MOC_LITERAL(102, 11),  // "position_ms"
        QT_MOC_LITERAL(114, 11),  // "duration_ms"
        QT_MOC_LITERAL(126, 5),  // "Error"
        QT_MOC_LITERAL(132, 7),  // "message"
        QT_MOC_LITERAL(140, 16),  // "PlaybackFinished"
        QT_MOC_LITERAL(157, 16),  // "StreamStatsReady"
        QT_MOC_LITERAL(174, 21),  // "analyzer::StreamStats"
        QT_MOC_LITERAL(196, 5),  // "stats"
        QT_MOC_LITERAL(202, 14),  // "HistogramReady"
        QT_MOC_LITERAL(217, 23),  // "analyzer::HistogramData"
        QT_MOC_LITERAL(241, 4),  // "hist"
        QT_MOC_LITERAL(246, 18),  // "FaceDetectionReady"
        QT_MOC_LITERAL(265, 31),  // "std::vector<analyzer::FaceInfo>"
        QT_MOC_LITERAL(297, 5),  // "faces"
        QT_MOC_LITERAL(303, 19),  // "VideoFrameListReset"
        QT_MOC_LITERAL(323, 19),  // "VideoFrameInfoReady"
        QT_MOC_LITERAL(343, 5),  // "index"
        QT_MOC_LITERAL(349, 10),  // "frame_type"
        QT_MOC_LITERAL(360, 12),  // "is_key_frame"
        QT_MOC_LITERAL(373, 3),  // "pts"
        QT_MOC_LITERAL(377, 17),  // "timestamp_seconds"
        QT_MOC_LITERAL(395, 19),  // "AudioFrameListReset"
        QT_MOC_LITERAL(415, 19),  // "AudioFrameInfoReady"
        QT_MOC_LITERAL(435, 12),  // "sample_count"
        QT_MOC_LITERAL(448, 11),  // "sample_rate"
        QT_MOC_LITERAL(460, 8),  // "channels"
        QT_MOC_LITERAL(469, 10),  // "byte_count"
        QT_MOC_LITERAL(480, 16),  // "MediaModeChanged"
        QT_MOC_LITERAL(497, 9),  // "has_video"
        QT_MOC_LITERAL(507, 15),  // "AudioLevelReady"
        QT_MOC_LITERAL(523, 5),  // "level"
        QT_MOC_LITERAL(529, 23),  // "VideoFrameExportStarted"
        QT_MOC_LITERAL(553, 12),  // "total_frames"
        QT_MOC_LITERAL(566, 24),  // "VideoFrameExportProgress"
        QT_MOC_LITERAL(591, 15),  // "exported_frames"
        QT_MOC_LITERAL(607, 24),  // "VideoFrameExportFinished"
        QT_MOC_LITERAL(632, 10),  // "output_dir"
        QT_MOC_LITERAL(643, 24),  // "VideoFrameExportCanceled"
        QT_MOC_LITERAL(668, 21)   // "VideoFrameExportError"
    },
    "videoeye::player::MediaPlayer",
    "StateChanged",
    "",
    "model::PlayerState",
    "state",
    "FrameReady",
    "frame",
    "PositionChanged",
    "position_ms",
    "duration_ms",
    "Error",
    "message",
    "PlaybackFinished",
    "StreamStatsReady",
    "analyzer::StreamStats",
    "stats",
    "HistogramReady",
    "analyzer::HistogramData",
    "hist",
    "FaceDetectionReady",
    "std::vector<analyzer::FaceInfo>",
    "faces",
    "VideoFrameListReset",
    "VideoFrameInfoReady",
    "index",
    "frame_type",
    "is_key_frame",
    "pts",
    "timestamp_seconds",
    "AudioFrameListReset",
    "AudioFrameInfoReady",
    "sample_count",
    "sample_rate",
    "channels",
    "byte_count",
    "MediaModeChanged",
    "has_video",
    "AudioLevelReady",
    "level",
    "VideoFrameExportStarted",
    "total_frames",
    "VideoFrameExportProgress",
    "exported_frames",
    "VideoFrameExportFinished",
    "output_dir",
    "VideoFrameExportCanceled",
    "VideoFrameExportError"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_videoeye__player__MediaPlayer[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
      19,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      19,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    1,  128,    2, 0x06,    1 /* Public */,
       5,    1,  131,    2, 0x06,    3 /* Public */,
       7,    2,  134,    2, 0x06,    5 /* Public */,
      10,    1,  139,    2, 0x06,    8 /* Public */,
      12,    0,  142,    2, 0x06,   10 /* Public */,
      13,    1,  143,    2, 0x06,   11 /* Public */,
      16,    1,  146,    2, 0x06,   13 /* Public */,
      19,    1,  149,    2, 0x06,   15 /* Public */,
      22,    0,  152,    2, 0x06,   17 /* Public */,
      23,    5,  153,    2, 0x06,   18 /* Public */,
      29,    0,  164,    2, 0x06,   24 /* Public */,
      30,    7,  165,    2, 0x06,   25 /* Public */,
      35,    1,  180,    2, 0x06,   33 /* Public */,
      37,    2,  183,    2, 0x06,   35 /* Public */,
      39,    1,  188,    2, 0x06,   38 /* Public */,
      41,    1,  191,    2, 0x06,   40 /* Public */,
      43,    1,  194,    2, 0x06,   42 /* Public */,
      45,    2,  197,    2, 0x06,   44 /* Public */,
      46,    1,  202,    2, 0x06,   47 /* Public */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, QMetaType::QImage,    6,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,    8,    9,
    QMetaType::Void, QMetaType::QString,   11,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 14,   15,
    QMetaType::Void, 0x80000000 | 17,   18,
    QMetaType::Void, 0x80000000 | 20,   21,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::Int, QMetaType::Bool, QMetaType::LongLong, QMetaType::Double,   24,   25,   26,   27,   28,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::LongLong, QMetaType::Double, QMetaType::Int, QMetaType::Int, QMetaType::Int, QMetaType::Int,   24,   27,   28,   31,   32,   33,   34,
    QMetaType::Void, QMetaType::Bool,   36,
    QMetaType::Void, QMetaType::Double, QMetaType::Double,   38,   28,
    QMetaType::Void, QMetaType::Int,   40,
    QMetaType::Void, QMetaType::Int,   42,
    QMetaType::Void, QMetaType::QString,   44,
    QMetaType::Void, QMetaType::Int, QMetaType::QString,   42,   44,
    QMetaType::Void, QMetaType::QString,   11,

       0        // eod
};

Q_CONSTINIT const QMetaObject videoeye::player::MediaPlayer::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_videoeye__player__MediaPlayer.offsetsAndSizes,
    qt_meta_data_videoeye__player__MediaPlayer,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_videoeye__player__MediaPlayer_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<MediaPlayer, std::true_type>,
        // method 'StateChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<model::PlayerState, std::false_type>,
        // method 'FrameReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QImage &, std::false_type>,
        // method 'PositionChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'Error'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'PlaybackFinished'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'StreamStatsReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const analyzer::StreamStats &, std::false_type>,
        // method 'HistogramReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const analyzer::HistogramData &, std::false_type>,
        // method 'FaceDetectionReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::vector<analyzer::FaceInfo> &, std::false_type>,
        // method 'VideoFrameListReset'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'VideoFrameInfoReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'AudioFrameListReset'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'AudioFrameInfoReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'MediaModeChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'AudioLevelReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'VideoFrameExportStarted'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'VideoFrameExportProgress'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'VideoFrameExportFinished'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'VideoFrameExportCanceled'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'VideoFrameExportError'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>
    >,
    nullptr
} };

void videoeye::player::MediaPlayer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MediaPlayer *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->StateChanged((*reinterpret_cast< std::add_pointer_t<model::PlayerState>>(_a[1]))); break;
        case 1: _t->FrameReady((*reinterpret_cast< std::add_pointer_t<QImage>>(_a[1]))); break;
        case 2: _t->PositionChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 3: _t->Error((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 4: _t->PlaybackFinished(); break;
        case 5: _t->StreamStatsReady((*reinterpret_cast< std::add_pointer_t<analyzer::StreamStats>>(_a[1]))); break;
        case 6: _t->HistogramReady((*reinterpret_cast< std::add_pointer_t<analyzer::HistogramData>>(_a[1]))); break;
        case 7: _t->FaceDetectionReady((*reinterpret_cast< std::add_pointer_t<std::vector<analyzer::FaceInfo>>>(_a[1]))); break;
        case 8: _t->VideoFrameListReset(); break;
        case 9: _t->VideoFrameInfoReady((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<double>>(_a[5]))); break;
        case 10: _t->AudioFrameListReset(); break;
        case 11: _t->AudioFrameInfoReady((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<double>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[5])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[6])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[7]))); break;
        case 12: _t->MediaModeChanged((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 13: _t->AudioLevelReady((*reinterpret_cast< std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<double>>(_a[2]))); break;
        case 14: _t->VideoFrameExportStarted((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 15: _t->VideoFrameExportProgress((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 16: _t->VideoFrameExportFinished((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 17: _t->VideoFrameExportCanceled((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 18: _t->VideoFrameExportError((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (MediaPlayer::*)(model::PlayerState );
            if (_t _q_method = &MediaPlayer::StateChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(const QImage & );
            if (_t _q_method = &MediaPlayer::FrameReady; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(int , int );
            if (_t _q_method = &MediaPlayer::PositionChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(const QString & );
            if (_t _q_method = &MediaPlayer::Error; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)();
            if (_t _q_method = &MediaPlayer::PlaybackFinished; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(const analyzer::StreamStats & );
            if (_t _q_method = &MediaPlayer::StreamStatsReady; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(const analyzer::HistogramData & );
            if (_t _q_method = &MediaPlayer::HistogramReady; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(const std::vector<analyzer::FaceInfo> & );
            if (_t _q_method = &MediaPlayer::FaceDetectionReady; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)();
            if (_t _q_method = &MediaPlayer::VideoFrameListReset; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(int , int , bool , qint64 , double );
            if (_t _q_method = &MediaPlayer::VideoFrameInfoReady; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)();
            if (_t _q_method = &MediaPlayer::AudioFrameListReset; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(int , qint64 , double , int , int , int , int );
            if (_t _q_method = &MediaPlayer::AudioFrameInfoReady; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(bool );
            if (_t _q_method = &MediaPlayer::MediaModeChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(double , double );
            if (_t _q_method = &MediaPlayer::AudioLevelReady; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(int );
            if (_t _q_method = &MediaPlayer::VideoFrameExportStarted; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 14;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(int );
            if (_t _q_method = &MediaPlayer::VideoFrameExportProgress; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 15;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(const QString & );
            if (_t _q_method = &MediaPlayer::VideoFrameExportFinished; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 16;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(int , const QString & );
            if (_t _q_method = &MediaPlayer::VideoFrameExportCanceled; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 17;
                return;
            }
        }
        {
            using _t = void (MediaPlayer::*)(const QString & );
            if (_t _q_method = &MediaPlayer::VideoFrameExportError; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 18;
                return;
            }
        }
    }
}

const QMetaObject *videoeye::player::MediaPlayer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *videoeye::player::MediaPlayer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_videoeye__player__MediaPlayer.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int videoeye::player::MediaPlayer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 19)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 19;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 19)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 19;
    }
    return _id;
}

// SIGNAL 0
void videoeye::player::MediaPlayer::StateChanged(model::PlayerState _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void videoeye::player::MediaPlayer::FrameReady(const QImage & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void videoeye::player::MediaPlayer::PositionChanged(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void videoeye::player::MediaPlayer::Error(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void videoeye::player::MediaPlayer::PlaybackFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void videoeye::player::MediaPlayer::StreamStatsReady(const analyzer::StreamStats & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void videoeye::player::MediaPlayer::HistogramReady(const analyzer::HistogramData & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void videoeye::player::MediaPlayer::FaceDetectionReady(const std::vector<analyzer::FaceInfo> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void videoeye::player::MediaPlayer::VideoFrameListReset()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}

// SIGNAL 9
void videoeye::player::MediaPlayer::VideoFrameInfoReady(int _t1, int _t2, bool _t3, qint64 _t4, double _t5)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void videoeye::player::MediaPlayer::AudioFrameListReset()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void videoeye::player::MediaPlayer::AudioFrameInfoReady(int _t1, qint64 _t2, double _t3, int _t4, int _t5, int _t6, int _t7)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t6))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t7))) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void videoeye::player::MediaPlayer::MediaModeChanged(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void videoeye::player::MediaPlayer::AudioLevelReady(double _t1, double _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}

// SIGNAL 14
void videoeye::player::MediaPlayer::VideoFrameExportStarted(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}

// SIGNAL 15
void videoeye::player::MediaPlayer::VideoFrameExportProgress(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 15, _a);
}

// SIGNAL 16
void videoeye::player::MediaPlayer::VideoFrameExportFinished(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 16, _a);
}

// SIGNAL 17
void videoeye::player::MediaPlayer::VideoFrameExportCanceled(int _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 17, _a);
}

// SIGNAL 18
void videoeye::player::MediaPlayer::VideoFrameExportError(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 18, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
