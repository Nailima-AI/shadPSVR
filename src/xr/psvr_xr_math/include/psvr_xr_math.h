// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PxmVec3 {
    float x;
    float y;
    float z;
} PxmVec3;

typedef struct PxmQuat {
    float x;
    float y;
    float z;
    float w;
} PxmQuat;

typedef struct PxmPose {
    PxmQuat orientation;
    PxmVec3 position;
} PxmPose;

typedef struct PxmFov {
    float angle_left;
    float angle_right;
    float angle_up;
    float angle_down;
} PxmFov;

typedef struct PxmOrbisFov {
    float tan_out;
    float tan_in;
    float tan_top;
    float tan_bottom;
} PxmOrbisFov;

typedef struct PxmTrackingSpace {
    PxmQuat recenter_yaw_inverse;
    PxmVec3 recenter_origin;
    PxmVec3 orbis_origin;
} PxmTrackingSpace;

typedef struct PxmMoveState {
    PxmPose sphere;
    PxmVec3 accelerometer;
    PxmVec3 gyro;
} PxmMoveState;

typedef struct PxmHmdBodyPoses {
    PxmPose device;
    PxmPose head;
} PxmHmdBodyPoses;

void pxm_tracking_space_init(PxmTrackingSpace* space, PxmVec3 orbis_origin);
void pxm_tracking_space_recenter(PxmTrackingSpace* space, const PxmPose* current_head,
                                 float floor_y);
void pxm_tracking_space_recenter_orientation(PxmTrackingSpace* space, const PxmPose* current_head);
void pxm_xr_pose_to_orbis(const PxmTrackingSpace* space, const PxmPose* xr, PxmPose* out);
void pxm_xr_vector_to_orbis(const PxmTrackingSpace* space, PxmVec3 xr, PxmVec3* out);
void pxm_eye_offset(const PxmPose* head, const PxmPose* eye, PxmVec3* out);
void pxm_head_from_views(const PxmPose* left, const PxmPose* right, PxmPose* out);
void pxm_fov_to_orbis(const PxmFov* left, const PxmFov* right, PxmOrbisFov* out);
void pxm_pose_apply_local_offset(const PxmPose* pose, PxmVec3 offset, PxmPose* out);
void pxm_orbis_pose_to_xr(const PxmTrackingSpace* space, const PxmPose* orbis, PxmPose* out);
void pxm_fov_from_uv_block(const float block[4], PxmFov* out);
void pxm_hmd_body_poses(const PxmPose* eye_center, PxmHmdBodyPoses* out);
void pxm_move_from_grip(const PxmPose* grip, PxmVec3 angular_velocity, PxmMoveState* out);

#ifdef __cplusplus
}
#endif
