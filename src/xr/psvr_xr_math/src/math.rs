// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

use std::ops::{Add, Mul, Neg, Sub};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vec3 {
    pub const ZERO: Self = Self::new(0.0, 0.0, 0.0);

    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    pub fn cross(self, o: Self) -> Self {
        Self::new(
            self.y * o.z - self.z * o.y,
            self.z * o.x - self.x * o.z,
            self.x * o.y - self.y * o.x,
        )
    }

    pub fn length(self) -> f32 {
        (self.x * self.x + self.y * self.y + self.z * self.z).sqrt()
    }
}

impl Add for Vec3 {
    type Output = Self;
    fn add(self, o: Self) -> Self {
        Self::new(self.x + o.x, self.y + o.y, self.z + o.z)
    }
}

impl Sub for Vec3 {
    type Output = Self;
    fn sub(self, o: Self) -> Self {
        Self::new(self.x - o.x, self.y - o.y, self.z - o.z)
    }
}

impl Mul<f32> for Vec3 {
    type Output = Self;
    fn mul(self, s: f32) -> Self {
        Self::new(self.x * s, self.y * s, self.z * s)
    }
}

impl Neg for Vec3 {
    type Output = Self;
    fn neg(self) -> Self {
        Self::new(-self.x, -self.y, -self.z)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Quat {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

impl Default for Quat {
    fn default() -> Self {
        Self::IDENTITY
    }
}

impl Quat {
    pub const IDENTITY: Self = Self {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 1.0,
    };

    pub fn from_axis_angle(axis: Vec3, radians: f32) -> Self {
        let (s, c) = (radians * 0.5).sin_cos();
        let len = axis.length();
        if len <= f32::EPSILON {
            return Self::IDENTITY;
        }
        let a = axis * (1.0 / len);
        Self {
            x: a.x * s,
            y: a.y * s,
            z: a.z * s,
            w: c,
        }
    }

    pub fn conjugate(self) -> Self {
        Self {
            x: -self.x,
            y: -self.y,
            z: -self.z,
            w: self.w,
        }
    }

    pub fn dot(self, o: Self) -> f32 {
        self.x * o.x + self.y * o.y + self.z * o.z + self.w * o.w
    }

    pub fn normalized(self) -> Self {
        let len = self.dot(self).sqrt();
        if len <= f32::EPSILON || !len.is_finite() {
            return Self::IDENTITY;
        }
        let inv = 1.0 / len;
        Self {
            x: self.x * inv,
            y: self.y * inv,
            z: self.z * inv,
            w: self.w * inv,
        }
    }

    pub fn rotate(self, v: Vec3) -> Vec3 {
        let u = Vec3::new(self.x, self.y, self.z);
        let t = u.cross(v) * 2.0;
        v + t * self.w + u.cross(t)
    }

    /// Rotation about +Y that matches this orientation's heading.
    pub fn yaw_only(self) -> Self {
        let forward = self.rotate(Vec3::new(0.0, 0.0, -1.0));
        let horizontal = (forward.x * forward.x + forward.z * forward.z).sqrt();
        if horizontal <= 1e-4 {
            // Looking straight up or down: heading is undefined, derive it from "up" instead.
            let up = self.rotate(Vec3::new(0.0, 1.0, 0.0));
            let sign = if forward.y > 0.0 { 1.0 } else { -1.0 };
            let yaw = (sign * up.x).atan2(sign * up.z);
            return Self::from_axis_angle(Vec3::new(0.0, 1.0, 0.0), yaw);
        }
        let yaw = (-forward.x).atan2(-forward.z);
        Self::from_axis_angle(Vec3::new(0.0, 1.0, 0.0), yaw)
    }

    pub fn nlerp_half(self, o: Self) -> Self {
        let o = if self.dot(o) < 0.0 {
            Self {
                x: -o.x,
                y: -o.y,
                z: -o.z,
                w: -o.w,
            }
        } else {
            o
        };
        Self {
            x: self.x + o.x,
            y: self.y + o.y,
            z: self.z + o.z,
            w: self.w + o.w,
        }
        .normalized()
    }
}

impl Mul for Quat {
    type Output = Self;
    fn mul(self, b: Self) -> Self {
        let a = self;
        Self {
            w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        }
    }
}

/// Field order matches XrPosef so a C++ caller can copy one straight across.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Pose {
    pub orientation: Quat,
    pub position: Vec3,
}

impl Pose {
    pub const IDENTITY: Self = Self {
        orientation: Quat::IDENTITY,
        position: Vec3::ZERO,
    };

    pub fn apply_local_offset(&self, offset: Vec3) -> Self {
        Self {
            orientation: self.orientation,
            position: self.position + self.orientation.rotate(offset),
        }
    }
}

/// Matches XrFovf: radians, left and down are normally negative.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Fov {
    pub angle_left: f32,
    pub angle_right: f32,
    pub angle_up: f32,
    pub angle_down: f32,
}

/// Matches OrbisHmdFieldOfView: positive tangents, "out" is the temporal side of an eye.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OrbisFov {
    pub tan_out: f32,
    pub tan_in: f32,
    pub tan_top: f32,
    pub tan_bottom: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TrackingSpace {
    pub recenter_yaw_inverse: Quat,
    pub recenter_origin: Vec3,
    pub orbis_origin: Vec3,
}

impl TrackingSpace {
    pub fn new(orbis_origin: Vec3) -> Self {
        Self {
            recenter_yaw_inverse: Quat::IDENTITY,
            recenter_origin: Vec3::ZERO,
            orbis_origin,
        }
    }

    /// Makes `head`'s current position the Orbis origin and its heading Orbis forward.
    /// Pitch and roll are kept so the horizon stays level.
    pub fn recenter(&mut self, head: &Pose) {
        self.recenter_yaw_inverse = head.orientation.normalized().yaw_only().conjugate();
        self.recenter_origin = head.position;
    }

    pub fn pose_to_orbis(&self, xr: &Pose) -> Pose {
        Pose {
            orientation: (self.recenter_yaw_inverse * xr.orientation.normalized()).normalized(),
            position: self.orbis_origin
                + self
                    .recenter_yaw_inverse
                    .rotate(xr.position - self.recenter_origin),
        }
    }

    pub fn vector_to_orbis(&self, xr: Vec3) -> Vec3 {
        self.recenter_yaw_inverse.rotate(xr)
    }
}

/// Eye position expressed in the head's local frame (what sceHmdGet2DEyeOffset reports).
pub fn eye_offset(head: &Pose, eye: &Pose) -> Vec3 {
    head.orientation
        .normalized()
        .conjugate()
        .rotate(eye.position - head.position)
}

/// OpenXR has no head pose in xrLocateViews, so take the midpoint between the two eyes.
pub fn head_from_views(left: &Pose, right: &Pose) -> Pose {
    Pose {
        orientation: left
            .orientation
            .normalized()
            .nlerp_half(right.orientation.normalized()),
        position: (left.position + right.position) * 0.5,
    }
}

/// PSVR reports one FOV for both eyes, mirrored horizontally. Average the two OpenXR eyes
/// after mirroring the right eye so asymmetric runtimes still produce a sensible value.
pub fn fov_to_orbis(left: &Fov, right: &Fov) -> OrbisFov {
    let left_out = (-left.angle_left).tan();
    let left_in = left.angle_right.tan();
    let right_out = right.angle_right.tan();
    let right_in = (-right.angle_left).tan();
    OrbisFov {
        tan_out: (left_out + right_out) * 0.5,
        tan_in: (left_in + right_in) * 0.5,
        tan_top: (left.angle_up.tan() + right.angle_up.tan()) * 0.5,
        tan_bottom: ((-left.angle_down).tan() + (-right.angle_down).tan()) * 0.5,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::{FRAC_PI_2, FRAC_PI_4, PI};

    const EPS: f32 = 1e-5;
    const UP: Vec3 = Vec3::new(0.0, 1.0, 0.0);

    fn approx_v(a: Vec3, b: Vec3) {
        assert!((a - b).length() < EPS, "{a:?} != {b:?}");
    }

    fn approx_q(a: Quat, b: Quat) {
        assert!(a.dot(b).abs() > 1.0 - EPS, "{a:?} != {b:?}");
    }

    #[test]
    fn rotate_quarter_turn_about_y() {
        let q = Quat::from_axis_angle(UP, FRAC_PI_2);
        approx_v(
            q.rotate(Vec3::new(0.0, 0.0, -1.0)),
            Vec3::new(-1.0, 0.0, 0.0),
        );
    }

    #[test]
    fn yaw_only_strips_pitch_and_roll() {
        let yaw = Quat::from_axis_angle(UP, 0.7);
        let pitch = Quat::from_axis_angle(Vec3::new(1.0, 0.0, 0.0), 0.4);
        let roll = Quat::from_axis_angle(Vec3::new(0.0, 0.0, 1.0), -0.3);
        approx_q((yaw * pitch * roll).yaw_only(), yaw);
    }

    #[test]
    fn yaw_only_survives_looking_straight_down() {
        let yaw = Quat::from_axis_angle(UP, 1.1);
        let down = Quat::from_axis_angle(Vec3::new(1.0, 0.0, 0.0), -FRAC_PI_2);
        approx_q((yaw * down).yaw_only(), yaw);
    }

    #[test]
    fn identity_space_only_translates() {
        let space = TrackingSpace::new(Vec3::new(0.0, 0.0, 1.5));
        let pose = Pose {
            orientation: Quat::IDENTITY,
            position: Vec3::new(0.1, 1.7, -0.2),
        };
        let out = space.pose_to_orbis(&pose);
        approx_v(out.position, Vec3::new(0.1, 1.7, 1.3));
        approx_q(out.orientation, Quat::IDENTITY);
    }

    #[test]
    fn recenter_puts_head_at_origin_facing_forward() {
        let mut space = TrackingSpace::new(Vec3::ZERO);
        let head = Pose {
            orientation: Quat::from_axis_angle(UP, PI * 0.75)
                * Quat::from_axis_angle(Vec3::new(1.0, 0.0, 0.0), 0.2),
            position: Vec3::new(2.0, 1.6, -3.0),
        };
        space.recenter(&head);
        let out = space.pose_to_orbis(&head);
        approx_v(out.position, Vec3::ZERO);
        approx_q(
            out.orientation,
            Quat::from_axis_angle(Vec3::new(1.0, 0.0, 0.0), 0.2),
        );
    }

    #[test]
    fn recenter_rotates_world_motion() {
        let mut space = TrackingSpace::new(Vec3::ZERO);
        let head = Pose {
            orientation: Quat::from_axis_angle(UP, FRAC_PI_2),
            position: Vec3::ZERO,
        };
        space.recenter(&head);
        // Head faces world -X; stepping along -X must become stepping forward (-Z).
        let stepped = Pose {
            position: Vec3::new(-1.0, 0.0, 0.0),
            ..head
        };
        approx_v(
            space.pose_to_orbis(&stepped).position,
            Vec3::new(0.0, 0.0, -1.0),
        );
        approx_v(
            space.vector_to_orbis(Vec3::new(-1.0, 0.0, 0.0)),
            Vec3::new(0.0, 0.0, -1.0),
        );
    }

    #[test]
    fn eye_offset_is_head_local() {
        let head = Pose {
            orientation: Quat::from_axis_angle(UP, FRAC_PI_2),
            position: Vec3::new(5.0, 1.0, 0.0),
        };
        let left_eye = head.apply_local_offset(Vec3::new(-0.032, 0.0, 0.0));
        approx_v(eye_offset(&head, &left_eye), Vec3::new(-0.032, 0.0, 0.0));
    }

    #[test]
    fn head_is_midpoint_of_eyes() {
        let ori = Quat::from_axis_angle(UP, 0.3);
        let l = Pose {
            orientation: ori,
            position: Vec3::new(-0.03, 1.6, 0.0),
        };
        let r = Pose {
            orientation: ori,
            position: Vec3::new(0.03, 1.6, 0.0),
        };
        let h = head_from_views(&l, &r);
        approx_v(h.position, Vec3::new(0.0, 1.6, 0.0));
        approx_q(h.orientation, ori);
    }

    #[test]
    fn head_from_views_handles_opposite_quat_signs() {
        let ori = Quat::from_axis_angle(UP, 0.3);
        let flipped = Quat {
            x: -ori.x,
            y: -ori.y,
            z: -ori.z,
            w: -ori.w,
        };
        let l = Pose {
            orientation: ori,
            position: Vec3::ZERO,
        };
        let r = Pose {
            orientation: flipped,
            position: Vec3::ZERO,
        };
        approx_q(head_from_views(&l, &r).orientation, ori);
    }

    #[test]
    fn symmetric_fov_maps_to_tangents() {
        let fov = Fov {
            angle_left: -FRAC_PI_4,
            angle_right: FRAC_PI_4,
            angle_up: FRAC_PI_4,
            angle_down: -FRAC_PI_4,
        };
        let out = fov_to_orbis(&fov, &fov);
        for v in [out.tan_out, out.tan_in, out.tan_top, out.tan_bottom] {
            assert!((v - 1.0).abs() < EPS);
        }
    }

    #[test]
    fn mirrored_asymmetric_fov_keeps_out_and_in_apart() {
        let left = Fov {
            angle_left: -0.9,
            angle_right: 0.7,
            angle_up: 0.8,
            angle_down: -0.85,
        };
        let right = Fov {
            angle_left: -0.7,
            angle_right: 0.9,
            ..left
        };
        let out = fov_to_orbis(&left, &right);
        assert!((out.tan_out - 0.9f32.tan()).abs() < EPS);
        assert!((out.tan_in - 0.7f32.tan()).abs() < EPS);
        assert!((out.tan_top - 0.8f32.tan()).abs() < EPS);
        assert!((out.tan_bottom - 0.85f32.tan()).abs() < EPS);
    }

    #[test]
    fn layouts_match_c_header() {
        assert_eq!(std::mem::size_of::<Vec3>(), 12);
        assert_eq!(std::mem::size_of::<Quat>(), 16);
        assert_eq!(std::mem::size_of::<Pose>(), 28);
        assert_eq!(std::mem::offset_of!(Pose, position), 16);
        assert_eq!(std::mem::size_of::<Fov>(), 16);
        assert_eq!(std::mem::size_of::<OrbisFov>(), 16);
        assert_eq!(std::mem::size_of::<TrackingSpace>(), 40);
    }
}
