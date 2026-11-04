// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "common/types.h"
#include "psvr_xr_math.h"

struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;

namespace Core::Xr {

// All poses are already in the Orbis tracking space.
struct TrackingSnapshot {
    PxmPose head;
    PxmPose eyes[2];
    PxmVec3 eye_offsets[2];
    PxmOrbisFov fov;
    PxmVec3 linear_velocity;
    PxmVec3 angular_velocity;
    bool has_velocity;
    u64 timestamp_us;
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

    ~Runtime();

private:
    Runtime();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::Xr
