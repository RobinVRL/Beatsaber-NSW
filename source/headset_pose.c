#include <math.h>

#include "headset_pose.h"

void bs_headset_turn(BsHeadsetAngles *angles, int horizontal, int vertical) {
    const int step = 5;
    angles->yaw_degrees = (angles->yaw_degrees + step * horizontal + 540) % 360 - 180;
    angles->pitch_degrees += step * vertical;
    if (angles->pitch_degrees > 85) angles->pitch_degrees = 85;
    if (angles->pitch_degrees < -85) angles->pitch_degrees = -85;
}

void bs_headset_orientation(const BsHeadsetAngles *angles, float orientation[4]) {
    const float half_radians = 0.00872664626f;
    const float yaw = -angles->yaw_degrees * half_radians;
    const float pitch = angles->pitch_degrees * half_radians;
    const float yaw_sin = sinf(yaw);
    const float yaw_cos = cosf(yaw);
    const float pitch_sin = sinf(pitch);
    const float pitch_cos = cosf(pitch);
    orientation[0] = yaw_cos * pitch_sin;
    orientation[1] = yaw_sin * pitch_cos;
    orientation[2] = -yaw_sin * pitch_sin;
    orientation[3] = yaw_cos * pitch_cos;
}
