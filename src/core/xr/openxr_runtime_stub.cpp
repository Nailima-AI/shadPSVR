// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/xr/openxr_runtime.h"

// Without OpenXR there is never a headset, so the math the HLE libraries call is unreachable.
extern "C" {
void pxm_fov_from_uv_block(const float*, PxmFov* out) {
    *out = {};
}
void pxm_head_from_views(const PxmPose* left, const PxmPose*, PxmPose* out) {
    *out = *left;
}
void pxm_move_from_grip(const PxmPose* grip, PxmVec3, PxmMoveState* out) {
    *out = {.sphere = *grip};
}
}

namespace Core::Xr {

struct Runtime::Impl {
    std::vector<const char*> no_extensions;
};

Runtime& Runtime::Instance() {
    static Runtime runtime;
    return runtime;
}

Runtime::Runtime() : impl{std::make_unique<Impl>()} {}

Runtime::~Runtime() = default;

bool Runtime::IsRequested() const {
    return false;
}

bool Runtime::CreateXrInstance() {
    return false;
}

bool Runtime::HasXrInstance() const {
    return false;
}

const std::vector<const char*>& Runtime::GetVulkanInstanceExtensions() const {
    return impl->no_extensions;
}

const std::vector<const char*>& Runtime::GetVulkanDeviceExtensions() const {
    return impl->no_extensions;
}

std::optional<VkPhysicalDevice_T*> Runtime::GetVulkanPhysicalDevice(VkInstance_T*) {
    return std::nullopt;
}

bool Runtime::CreateSession(VkInstance_T*, VkPhysicalDevice_T*, VkDevice_T*, u32, u32) {
    return false;
}

void Runtime::Shutdown() {}

bool Runtime::IsHeadsetActive() const {
    return false;
}

std::optional<TrackingSnapshot> Runtime::GetTrackingSnapshot() const {
    return std::nullopt;
}

void Runtime::Recenter() {}

void Runtime::RecenterOrientation() {}

void Runtime::SetVibration(Hand, float) {}

float Runtime::GetVirtualCameraHeight() const {
    return 0.0f;
}

void Runtime::SetFrameCallback(std::function<void()>) {}

s64 Runtime::GetSwapchainFormat() const {
    return 0;
}

bool Runtime::EyeSwapchainsMatch(const u32*, const u32*) const {
    return false;
}

bool Runtime::EnsureEyeSwapchains(const u32*, const u32*) {
    return false;
}

std::vector<VkImage_T*> Runtime::GetEyeSwapchainImages(u32) const {
    return {};
}

std::optional<u32> Runtime::AcquireEyeImage(u32) {
    return std::nullopt;
}

void Runtime::PresentEyes(const EyeSubmission&) {}

} // namespace Core::Xr
