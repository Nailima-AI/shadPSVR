// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "common/types.h"
#include "psvr_xr_math.h"

struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkImage_T;

namespace Core::Xr {

enum ControllerButton : u32 {
    CONTROLLER_BUTTON_CROSS = 1u << 0,
    CONTROLLER_BUTTON_CIRCLE = 1u << 1,
    CONTROLLER_BUTTON_SQUARE = 1u << 2,
    CONTROLLER_BUTTON_TRIANGLE = 1u << 3,
    CONTROLLER_BUTTON_MOVE = 1u << 4,
    CONTROLLER_BUTTON_START = 1u << 5,
    CONTROLLER_BUTTON_SELECT = 1u << 6,
};

enum class Hand : u32 { Left = 0, Right = 1 };

struct ControllerSnapshot {
    bool pose_valid;
    PxmPose grip;
    PxmVec3 linear_velocity;
    PxmVec3 angular_velocity;
    bool has_velocity;
    u32 buttons;
    float trigger;
};

// All poses are already in the Orbis tracking space.
struct TrackingSnapshot {
    bool head_valid;
    u64 sequence;
    PxmPose head;
    PxmPose eyes[2];
    PxmVec3 eye_offsets[2];
    PxmOrbisFov fov;
    PxmVec3 linear_velocity;
    PxmVec3 angular_velocity;
    bool has_velocity;
    ControllerSnapshot hands[2];
    u64 timestamp_us;
};

// One stereo frame handed over by sceHmdReprojectionStart.
struct EyeSubmission {
    PxmPose head;
    PxmFov fov[2];
    u32 width[2];
    u32 height[2];
};

class Runtime {
public:
    static Runtime& Instance();

    // Opt-in through SHADPS4_OPENXR=1 so normal runs never load the OpenXR loader.
    bool IsRequested() const;

    // Must run before the Vulkan instance is created.
    bool CreateXrInstance();
    bool HasXrInstance() const;

    const std::vector<const char*>& GetVulkanInstanceExtensions() const;
    const std::vector<const char*>& GetVulkanDeviceExtensions() const;
    std::optional<VkPhysicalDevice_T*> GetVulkanPhysicalDevice(VkInstance_T* instance);

    bool CreateSession(VkInstance_T* instance, VkPhysicalDevice_T* physical_device,
                       VkDevice_T* device, u32 queue_family_index, u32 queue_index);
    void Shutdown();

    bool IsHeadsetActive() const;
    std::optional<TrackingSnapshot> GetTrackingSnapshot() const;
    void Recenter();
    void RecenterOrientation();
    void SetVibration(Hand hand, float amplitude);

    // Called once per headset frame from the frame loop, right after xrWaitFrame.
    void SetFrameCallback(std::function<void()> callback);

    // Eye swapchains. All of these are called from the GPU thread.
    s64 GetSwapchainFormat() const;
    bool EyeSwapchainsMatch(const u32 width[2], const u32 height[2]) const;
    bool EnsureEyeSwapchains(const u32 width[2], const u32 height[2]);
    std::vector<VkImage_T*> GetEyeSwapchainImages(u32 eye) const;
    std::optional<u32> AcquireEyeImage(u32 eye);
    // Releases both eye images and shows them with the submission's pose and field of view.
    void PresentEyes(const EyeSubmission& submission);

    ~Runtime();

private:
    Runtime();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::Xr
