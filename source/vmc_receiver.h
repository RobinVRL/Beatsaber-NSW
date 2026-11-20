/* SlimeVR (or any VMC-protocol sender) full-body hand tracking over UDP.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_VMC_RECEIVER_H
#define BS_VMC_RECEIVER_H

#include <stdbool.h>

/* Starts the receiver once, only if sdmc:/switch/beat-saber-quest/slimevr.txt
 * exists. Safe to call every frame. */
void bs_vmc_start(void);

/* Hand pose (0 = left, 1 = right) relative to the tracked head, in OpenXR
 * axes (x right, y up, -z forward) with the recenter-time body heading
 * removed. Orientation is calibrated so the controller pose at recenter
 * points straight ahead. Returns false if no fresh data for that hand. */
bool bs_vmc_hand_pose(int hand, float pos[3], float quat_xyzw[4]);

/* Makes the current body heading "forward" and the current hand rotations
 * "pointing ahead" (hold both controllers pointing forward). */
void bs_vmc_recenter(void);

#endif
