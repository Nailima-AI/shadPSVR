// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

//! Pose and view math that maps OpenXR tracking data onto what PSVR titles expect from
//! libSceHmd and libSceVrTracker. Pure functions over plain C structs, exposed via a C ABI
//! (see include/psvr_xr_math.h).
//!
//! Both spaces are right-handed, +Y up, -Z forward, metres. The mapping is therefore a
//! yaw-only recenter around a chosen origin plus a translation into the Orbis tracking
//! origin, which is where a title expects the head to sit relative to the PS Camera.
//!
//! # Safety
//!
//! Every exported function takes raw pointers that must each be null or point to a valid,
//! properly aligned value of the declared type for the duration of the call. Null pointers
//! turn the call into a no-op.

#![allow(clippy::missing_safety_doc)]

mod math;

pub use math::{Fov, MoveState, OrbisFov, Pose, Quat, TrackingSpace, Vec3};

macro_rules! deref_or_return {
    ($($name:ident),+) => {
        $(
            let Some($name) = (unsafe { $name.as_ref() }) else { return };
        )+
    };
}

macro_rules! deref_mut_or_return {
    ($name:ident) => {
        let Some($name) = (unsafe { $name.as_mut() }) else {
            return;
        };
    };
}

#[no_mangle]
pub unsafe extern "C" fn pxm_tracking_space_init(space: *mut TrackingSpace, orbis_origin: Vec3) {
    deref_mut_or_return!(space);
    *space = TrackingSpace::new(orbis_origin);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_tracking_space_recenter(
    space: *mut TrackingSpace,
    current_head: *const Pose,
) {
    deref_mut_or_return!(space);
    deref_or_return!(current_head);
    space.recenter(current_head);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_tracking_space_recenter_orientation(
    space: *mut TrackingSpace,
    current_head: *const Pose,
) {
    deref_mut_or_return!(space);
    deref_or_return!(current_head);
    space.recenter_orientation(current_head);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_xr_pose_to_orbis(
    space: *const TrackingSpace,
    xr: *const Pose,
    out: *mut Pose,
) {
    deref_or_return!(space, xr);
    deref_mut_or_return!(out);
    *out = space.pose_to_orbis(xr);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_xr_vector_to_orbis(
    space: *const TrackingSpace,
    xr: Vec3,
    out: *mut Vec3,
) {
    deref_or_return!(space);
    deref_mut_or_return!(out);
    *out = space.vector_to_orbis(xr);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_eye_offset(head: *const Pose, eye: *const Pose, out: *mut Vec3) {
    deref_or_return!(head, eye);
    deref_mut_or_return!(out);
    *out = math::eye_offset(head, eye);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_head_from_views(
    left: *const Pose,
    right: *const Pose,
    out: *mut Pose,
) {
    deref_or_return!(left, right);
    deref_mut_or_return!(out);
    *out = math::head_from_views(left, right);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_fov_to_orbis(left: *const Fov, right: *const Fov, out: *mut OrbisFov) {
    deref_or_return!(left, right);
    deref_mut_or_return!(out);
    *out = math::fov_to_orbis(left, right);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_pose_apply_local_offset(
    pose: *const Pose,
    offset: Vec3,
    out: *mut Pose,
) {
    deref_or_return!(pose);
    deref_mut_or_return!(out);
    *out = pose.apply_local_offset(offset);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_move_from_grip(
    grip: *const Pose,
    angular_velocity: Vec3,
    out: *mut MoveState,
) {
    deref_or_return!(grip);
    deref_mut_or_return!(out);
    *out = math::move_from_grip(grip, angular_velocity);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_orbis_pose_to_xr(
    space: *const TrackingSpace,
    orbis: *const Pose,
    out: *mut Pose,
) {
    deref_or_return!(space, orbis);
    deref_mut_or_return!(out);
    *out = space.pose_to_xr(orbis);
}

#[no_mangle]
pub unsafe extern "C" fn pxm_fov_from_uv_block(block: *const [f32; 4], out: *mut Fov) {
    deref_or_return!(block);
    deref_mut_or_return!(out);
    *out = math::fov_from_uv_block(*block);
}

#[cfg(test)]
mod ffi_tests {
    use super::*;
    use std::ptr;

    #[test]
    fn null_pointers_are_ignored() {
        let mut out = Vec3::ZERO;
        unsafe {
            pxm_tracking_space_init(ptr::null_mut(), Vec3::ZERO);
            pxm_xr_pose_to_orbis(ptr::null(), ptr::null(), ptr::null_mut());
            pxm_fov_to_orbis(ptr::null(), ptr::null(), ptr::null_mut());
            pxm_eye_offset(ptr::null(), ptr::null(), &mut out);
        }
        assert_eq!(out, Vec3::ZERO);
    }

    #[test]
    fn ffi_roundtrip() {
        let mut space = std::mem::MaybeUninit::<TrackingSpace>::uninit();
        let space = unsafe {
            pxm_tracking_space_init(space.as_mut_ptr(), Vec3::new(0.0, 0.0, 1.5));
            space.assume_init()
        };
        let head = Pose::IDENTITY;
        let mut out = Pose::IDENTITY;
        unsafe { pxm_xr_pose_to_orbis(&space, &head, &mut out) };
        assert!((out.position.z - 1.5).abs() < 1e-6);
    }
}
