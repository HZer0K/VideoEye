#pragma once

// EXIF 方向 → QTransform（仅用于显示，不动分析/导出的像素）。
//
// 单独成头的理由：这层换算错一位就是"竖拍照片横着显示 / 镜像翻错方向"，而画面本身
// 是"对的像素、错的朝向"，界面测试几乎看不出来。换算逻辑放匿名 namespace 里测不到，
// 所以提成 inline 函数，由 tests/unit/test_image_orientation_transform.cpp 用 3x2 的
// 唯一色像素图逐个角点钉住。
//
// 依赖：只吃 QtGui（QTransform）与 domain 的枚举，不碰 PlayerPanel。

#include <QTransform>

#include "core/domain/model/ImageOrientation.h"

namespace videoeye {
namespace ui {

inline QTransform OrientationToTransform(model::ImageOrientation orientation) {
    switch (orientation) {
    case model::ImageOrientation::MirrorHorizontal:
        return QTransform().scale(-1, 1);
    case model::ImageOrientation::MirrorVertical:
        return QTransform().scale(1, -1);
    case model::ImageOrientation::Rotate180:
        return QTransform().rotate(180);
    case model::ImageOrientation::Rotate90Cw:
        return QTransform().rotate(90);
    case model::ImageOrientation::Rotate270Cw:
        return QTransform().rotate(270);
    // 转置/反转置不是"旋转 + 镜像"的直觉组合，照抄容易把 5 和 7 写反：
    //   5 要让 (x,y) 落到 (y,x)      → rotate(90) 后水平翻
    //   7 要让 (x,y) 落到 (-y,-x)    → rotate(90) 后垂直翻
    case model::ImageOrientation::Transpose:
        return QTransform().rotate(90).scale(1, -1);
    case model::ImageOrientation::Transverse:
        return QTransform().rotate(90).scale(-1, 1);
    case model::ImageOrientation::None:
        break;
    }
    return QTransform();
}

} // namespace ui
} // namespace videoeye
