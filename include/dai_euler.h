/* dai_euler.h - the ONE conversion between a quaternion and three degrees.
 *
 * The document stores orientations as quaternions, and everything a human
 * touches - the inspector's Rotation fields, a level script saying "turn this
 * staircase around" - speaks degrees. That conversion existed once, privately,
 * inside src/dai_editor_ui.cpp, so the component table could not offer
 * "transform.rotation" at all: a script could set a node's SCALE by name but
 * not its rotation, and the level script of INNEN turned a handset and a
 * staircase with a property nobody answered. Both silently stayed straight.
 *
 * ZYX order (roll about X, then pitch about Y, then yaw about Z), which is
 * what every DCC tool's rotation fields mean and what the inspector already
 * showed. Header only, no state: two functions that are each other's inverse.
 */

#ifndef DAI_EULER_H
#define DAI_EULER_H

#include <cmath>

#include "daidalos.h"

/* Quaternion -> degrees, ZYX. Gimbal lock is clamped, not signalled: the
 * caller is a text field or a script, and neither has anything better to do
 * with an error than show 90 degrees. */
static inline void dai_quat_to_euler(dai_quat q, float *deg) {
    float sinr = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float roll = std::atan2(sinr, cosr);
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    float pitch = std::fabs(sinp) >= 1.0f ? std::copysign(1.5707963f, sinp)
                                          : std::asin(sinp);
    float siny = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float yaw = std::atan2(siny, cosy);
    const float R2D = 57.2957795f;
    deg[0] = roll * R2D;
    deg[1] = pitch * R2D;
    deg[2] = yaw * R2D;
}

/* Degrees -> quaternion, the same order read backwards. */
static inline dai_quat dai_euler_to_quat(const float *deg) {
    const float D2R = 3.14159265f / 180.0f;
    float rx = deg[0] * D2R, ry = deg[1] * D2R, rz = deg[2] * D2R;
    struct H {
        static dai_quat axis(float ax, float ay, float az, float a) {
            float sn = std::sin(a * 0.5f);
            dai_quat q{ ax * sn, ay * sn, az * sn, std::cos(a * 0.5f) };
            return q;
        }
        static dai_quat mul(dai_quat a, dai_quat b) {
            dai_quat q{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
            return q;
        }
    };
    return H::mul(H::mul(H::axis(0, 0, 1, rz), H::axis(0, 1, 0, ry)),
                  H::axis(1, 0, 0, rx));
}

#endif /* DAI_EULER_H */
