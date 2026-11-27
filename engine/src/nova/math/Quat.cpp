#include "nova/math/Quat.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace nova {

const char* eulerOrderName(EulerOrder order) {
    switch (order) {
        case EulerOrder::XYZ: return "XYZ";
        case EulerOrder::XZY: return "XZY";
        case EulerOrder::YXZ: return "YXZ";
        case EulerOrder::YZX: return "YZX";
        case EulerOrder::ZXY: return "ZXY";
        case EulerOrder::ZYX: return "ZYX";
    }
    return "?";
}

namespace detail {

glm::dvec3 eulerFromQuat(const glm::dquat& q, EulerOrder order) {
    // Rotation matrix elements m[row][col] of R = R_first * R_second * R_third.
    const glm::dmat3 m = glm::toMat3(glm::normalize(q));
    auto at = [&m](int row, int col) { return m[col][row]; }; // glm is column-major
    const f64 m11 = at(0, 0), m12 = at(0, 1), m13 = at(0, 2);
    const f64 m21 = at(1, 0), m22 = at(1, 1), m23 = at(1, 2);
    const f64 m31 = at(2, 0), m32 = at(2, 1), m33 = at(2, 2);
    // Q1.30 components carry ~1e-9 of error: closer to ±1 than that, the middle angle is ±90°.
    constexpr f64 kLock = 1.0 - 1e-9;
    auto clamp1 = [](f64 v) { return std::clamp(v, -1.0, 1.0); };

    glm::dvec3 e(0.0);
    switch (order) {
        case EulerOrder::XYZ:
            e.y = std::asin(clamp1(m13));
            if (std::abs(m13) < kLock) {
                e.x = std::atan2(-m23, m33);
                e.z = std::atan2(-m12, m11);
            } else {
                e.x = std::atan2(m32, m22);
            }
            break;
        case EulerOrder::XZY:
            e.z = std::asin(-clamp1(m12));
            if (std::abs(m12) < kLock) {
                e.x = std::atan2(m32, m22);
                e.y = std::atan2(m13, m11);
            } else {
                e.x = std::atan2(-m23, m33);
            }
            break;
        case EulerOrder::YXZ:
            e.x = std::asin(-clamp1(m23));
            if (std::abs(m23) < kLock) {
                e.y = std::atan2(m13, m33);
                e.z = std::atan2(m21, m22);
            } else {
                e.y = std::atan2(-m31, m11);
            }
            break;
        case EulerOrder::YZX:
            e.z = std::asin(clamp1(m21));
            if (std::abs(m21) < kLock) {
                e.x = std::atan2(-m23, m22);
                e.y = std::atan2(-m31, m11);
            } else {
                e.y = std::atan2(m13, m33);
            }
            break;
        case EulerOrder::ZXY:
            e.x = std::asin(clamp1(m32));
            if (std::abs(m32) < kLock) {
                e.y = std::atan2(-m31, m33);
                e.z = std::atan2(-m12, m22);
            } else {
                e.z = std::atan2(m21, m11);
            }
            break;
        case EulerOrder::ZYX:
            e.y = std::asin(-clamp1(m31));
            if (std::abs(m31) < kLock) {
                e.x = std::atan2(m32, m33);
                e.z = std::atan2(m21, m11);
            } else {
                e.z = std::atan2(-m12, m22);
            }
            break;
    }
    return e;
}

} // namespace detail

} // namespace nova
