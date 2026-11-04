// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/xr/openxr_runtime.h"

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

} // namespace Core::Xr
