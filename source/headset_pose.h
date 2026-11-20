#ifndef BS_HEADSET_POSE_H
#define BS_HEADSET_POSE_H

typedef struct {
    int yaw_degrees;
    int pitch_degrees;
} BsHeadsetAngles;

/* Right-handed view, with +Y up and -Z forward. Quaternion order is x,y,z,w. */
void bs_headset_orientation(const BsHeadsetAngles *angles, float orientation[4]);
void bs_headset_turn(BsHeadsetAngles *angles, int horizontal, int vertical);

#endif
