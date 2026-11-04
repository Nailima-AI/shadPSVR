// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "video_core/renderer_vulkan/vk_common.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "common/logging/log.h"
#include "core/libraries/kernel/time.h"
#include "core/xr/openxr_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Core::Xr {

namespace {

std::vector<std::string> SplitExtensionList(const std::string& list) {
    std::vector<std::string> out;
    std::istringstream stream{list};
    std::string name;
    while (stream >> name) {
        out.push_back(name);
    }
    return out;
}

PxmPose ToPxm(const XrPosef& pose) {
    PxmPose out;
    std::memcpy(&out, &pose, sizeof(out));
    return out;
}

PxmVec3 ToPxm(const XrVector3f& v) {
    return {v.x, v.y, v.z};
}

PxmFov ToPxm(const XrFovf& fov) {
    return {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
}

constexpr XrPosef IdentityPose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

} // Anonymous namespace

static_assert(sizeof(PxmPose) == sizeof(XrPosef));

struct Runtime::Impl {
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system_id{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace local_space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE};
    XrSessionState session_state{XR_SESSION_STATE_UNKNOWN};

    PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions{};
    PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions{};
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device{};
    PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements{};

    std::vector<std::string> instance_extension_storage;
    std::vector<std::string> device_extension_storage;
    std::vector<const char*> instance_extensions;
    std::vector<const char*> device_extensions;

    std::atomic<bool> session_running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> recenter_requested{true};
    std::jthread frame_thread;

    mutable std::mutex state_mutex;
    PxmTrackingSpace tracking_space{};
    std::optional<TrackingSnapshot> snapshot;

    bool Check(XrResult result, const char* what) const {
        if (XR_SUCCEEDED(result)) {
            return true;
        }
        char name[XR_MAX_RESULT_STRING_SIZE]{};
        if (instance != XR_NULL_HANDLE) {
            xrResultToString(instance, result, name);
        }
        LOG_ERROR(Lib_Hmd, "OpenXR {} failed: {} ({})", what, name, static_cast<s32>(result));
        return false;
    }

    template <typename Fn>
    bool LoadFunction(const char* name, Fn& fn) {
        return Check(
            xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn)),
            name);
    }

    bool QueryVulkanExtensions() {
        const auto query = [this](auto fn, std::vector<std::string>& storage,
                                  std::vector<const char*>& names, const char* what) {
            u32 size = 0;
            if (!Check(fn(instance, system_id, 0, &size, nullptr), what)) {
                return false;
            }
            std::string list(size, '\0');
            if (!Check(fn(instance, system_id, size, &size, list.data()), what)) {
                return false;
            }
            list.resize(std::strlen(list.c_str()));
            storage = SplitExtensionList(list);
            names.clear();
            for (const auto& ext : storage) {
                names.push_back(ext.c_str());
            }
            LOG_INFO(Lib_Hmd, "OpenXR requires Vulkan {}: {}", what, list);
            return true;
        };
        return query(get_instance_extensions, instance_extension_storage, instance_extensions,
                     "instance extensions") &&
               query(get_device_extensions, device_extension_storage, device_extensions,
                     "device extensions");
    }

    void HandleSessionStateChange(XrSessionState state) {
        session_state = state;
        switch (state) {
        case XR_SESSION_STATE_READY: {
            const XrSessionBeginInfo begin_info{
                .type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            };
            if (Check(xrBeginSession(session, &begin_info), "xrBeginSession")) {
                session_running = true;
                LOG_INFO(Lib_Hmd, "OpenXR session running");
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            session_running = false;
            Check(xrEndSession(session), "xrEndSession");
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            session_running = false;
            stop_requested = true;
            break;
        default:
            break;
        }
    }

    void PollEvents() {
        XrEventDataBuffer event{.type = XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                HandleSessionStateChange(
                    reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOG_ERROR(Lib_Hmd, "OpenXR instance loss pending");
                session_running = false;
                stop_requested = true;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                recenter_requested = true;
                break;
            default:
                break;
            }
            event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void SampleTracking(XrTime display_time) {
        XrSpaceVelocity velocity{.type = XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation head_location{.type = XR_TYPE_SPACE_LOCATION, .next = &velocity};
        if (!Check(xrLocateSpace(view_space, local_space, display_time, &head_location),
                   "xrLocateSpace")) {
            return;
        }
        constexpr XrSpaceLocationFlags pose_valid =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((head_location.locationFlags & pose_valid) != pose_valid) {
            return;
        }

        const XrViewLocateInfo locate_info{
            .type = XR_TYPE_VIEW_LOCATE_INFO,
            .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            .displayTime = display_time,
            .space = local_space,
        };
        XrViewState view_state{.type = XR_TYPE_VIEW_STATE};
        XrView views[2]{{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
        u32 view_count = 0;
        if (!Check(xrLocateViews(session, &locate_info, &view_state, 2, &view_count, views),
                   "xrLocateViews") ||
            view_count != 2) {
            return;
        }

        const PxmPose xr_head = ToPxm(head_location.pose);
        const PxmFov left_fov = ToPxm(views[0].fov);
        const PxmFov right_fov = ToPxm(views[1].fov);
        const PxmPose xr_eyes[2] = {ToPxm(views[0].pose), ToPxm(views[1].pose)};

        TrackingSnapshot next{};
        for (int eye = 0; eye < 2; ++eye) {
            pxm_eye_offset(&xr_head, &xr_eyes[eye], &next.eye_offsets[eye]);
        }
        pxm_fov_to_orbis(&left_fov, &right_fov, &next.fov);
        next.timestamp_us = Libraries::Kernel::sceKernelGetProcessTime();

        std::scoped_lock lock{state_mutex};
        if (recenter_requested.exchange(false)) {
            pxm_tracking_space_recenter(&tracking_space, &xr_head);
        }
        pxm_xr_pose_to_orbis(&tracking_space, &xr_head, &next.head);
        for (int eye = 0; eye < 2; ++eye) {
            pxm_xr_pose_to_orbis(&tracking_space, &xr_eyes[eye], &next.eyes[eye]);
        }
        constexpr XrSpaceVelocityFlags velocity_valid =
            XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        next.has_velocity = (velocity.velocityFlags & velocity_valid) == velocity_valid;
        if (next.has_velocity) {
            pxm_xr_vector_to_orbis(&tracking_space, ToPxm(velocity.linearVelocity),
                                   &next.linear_velocity);
            pxm_xr_vector_to_orbis(&tracking_space, ToPxm(velocity.angularVelocity),
                                   &next.angular_velocity);
        }
        snapshot = next;
    }

    // Until eye buffers are submitted this loop ends every frame with no layers, which keeps
    // the session focused and the runtime predicting poses. The runtime may touch the
    // graphics queue inside these calls, so they share the scheduler's submit lock.
    void FrameLoop(std::stop_token stop) {
        while (!stop.stop_requested() && !stop_requested) {
            PollEvents();
            if (!session_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            XrFrameState frame_state{.type = XR_TYPE_FRAME_STATE};
            if (!Check(xrWaitFrame(session, nullptr, &frame_state), "xrWaitFrame")) {
                continue;
            }
            {
                std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
                if (!Check(xrBeginFrame(session, nullptr), "xrBeginFrame")) {
                    continue;
                }
            }
            if (frame_state.shouldRender) {
                SampleTracking(frame_state.predictedDisplayTime);
            }
            const XrFrameEndInfo end_info{
                .type = XR_TYPE_FRAME_END_INFO,
                .displayTime = frame_state.predictedDisplayTime,
                .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                .layerCount = 0,
                .layers = nullptr,
            };
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            Check(xrEndFrame(session, &end_info), "xrEndFrame");
        }
        session_running = false;
    }

    void DestroySession() {
        if (frame_thread.joinable()) {
            frame_thread.request_stop();
            frame_thread.join();
        }
        if (view_space != XR_NULL_HANDLE) {
            xrDestroySpace(view_space);
            view_space = XR_NULL_HANDLE;
        }
        if (local_space != XR_NULL_HANDLE) {
            xrDestroySpace(local_space);
            local_space = XR_NULL_HANDLE;
        }
        if (session != XR_NULL_HANDLE) {
            xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        std::scoped_lock lock{state_mutex};
        snapshot.reset();
    }
};

Runtime& Runtime::Instance() {
    static Runtime runtime;
    return runtime;
}

Runtime::Runtime() : impl{std::make_unique<Impl>()} {
    pxm_tracking_space_init(&impl->tracking_space, PxmVec3{0.0f, 0.0f, 0.0f});
}

// Teardown happens in Shutdown() while the Vulkan device is still alive. By the time static
// destructors run the OpenXR loader may already be gone, so the state is left to the OS.
Runtime::~Runtime() {
    static_cast<void>(impl.release());
}

bool Runtime::IsRequested() const {
    const char* value = std::getenv("SHADPS4_OPENXR");
    return value != nullptr && std::strcmp(value, "0") != 0;
}

bool Runtime::HasXrInstance() const {
    return impl->instance != XR_NULL_HANDLE && impl->system_id != XR_NULL_SYSTEM_ID;
}

bool Runtime::CreateXrInstance() {
    if (HasXrInstance()) {
        return true;
    }
    if (!IsRequested()) {
        return false;
    }

    const char* extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo create_info{
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo =
            {
                .applicationVersion = 1,
                .engineVersion = 1,
                .apiVersion = XR_API_VERSION_1_0,
            },
        .enabledExtensionCount = 1,
        .enabledExtensionNames = extensions,
    };
    std::strncpy(create_info.applicationInfo.applicationName, "shadPS4",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create_info.applicationInfo.engineName, "shadPS4", XR_MAX_ENGINE_NAME_SIZE - 1);

    if (!impl->Check(xrCreateInstance(&create_info, &impl->instance), "xrCreateInstance")) {
        impl->instance = XR_NULL_HANDLE;
        return false;
    }

    const XrSystemGetInfo system_info{
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    if (!impl->Check(xrGetSystem(impl->instance, &system_info, &impl->system_id), "xrGetSystem") ||
        !impl->LoadFunction("xrGetVulkanInstanceExtensionsKHR", impl->get_instance_extensions) ||
        !impl->LoadFunction("xrGetVulkanDeviceExtensionsKHR", impl->get_device_extensions) ||
        !impl->LoadFunction("xrGetVulkanGraphicsDeviceKHR", impl->get_graphics_device) ||
        !impl->LoadFunction("xrGetVulkanGraphicsRequirementsKHR",
                            impl->get_graphics_requirements) ||
        !impl->QueryVulkanExtensions()) {
        Shutdown();
        return false;
    }

    XrSystemProperties properties{.type = XR_TYPE_SYSTEM_PROPERTIES};
    if (impl->Check(xrGetSystemProperties(impl->instance, impl->system_id, &properties),
                    "xrGetSystemProperties")) {
        LOG_INFO(Lib_Hmd, "OpenXR headset: {} (vendor {:#x}), position tracking: {}",
                 properties.systemName, properties.vendorId,
                 properties.trackingProperties.positionTracking == XR_TRUE);
    }
    return true;
}

const std::vector<const char*>& Runtime::GetVulkanInstanceExtensions() const {
    return impl->instance_extensions;
}

const std::vector<const char*>& Runtime::GetVulkanDeviceExtensions() const {
    return impl->device_extensions;
}

std::optional<VkPhysicalDevice> Runtime::GetVulkanPhysicalDevice(VkInstance instance) {
    if (!HasXrInstance()) {
        return std::nullopt;
    }
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (!impl->Check(impl->get_graphics_device(impl->instance, impl->system_id, instance, &device),
                     "xrGetVulkanGraphicsDeviceKHR") ||
        device == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    return device;
}

bool Runtime::CreateSession(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                            u32 queue_family_index, u32 queue_index) {
    if (!HasXrInstance() || impl->session != XR_NULL_HANDLE) {
        return impl->session != XR_NULL_HANDLE;
    }

    XrGraphicsRequirementsVulkanKHR requirements{.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!impl->Check(
            impl->get_graphics_requirements(impl->instance, impl->system_id, &requirements),
            "xrGetVulkanGraphicsRequirementsKHR")) {
        return false;
    }

    const XrGraphicsBindingVulkanKHR binding{
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
        .instance = instance,
        .physicalDevice = physical_device,
        .device = device,
        .queueFamilyIndex = queue_family_index,
        .queueIndex = queue_index,
    };
    const XrSessionCreateInfo session_info{
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .systemId = impl->system_id,
    };
    if (!impl->Check(xrCreateSession(impl->instance, &session_info, &impl->session),
                     "xrCreateSession")) {
        impl->session = XR_NULL_HANDLE;
        return false;
    }

    XrReferenceSpaceCreateInfo space_info{
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = IdentityPose,
    };
    if (!impl->Check(xrCreateReferenceSpace(impl->session, &space_info, &impl->local_space),
                     "xrCreateReferenceSpace(LOCAL)")) {
        impl->DestroySession();
        return false;
    }
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!impl->Check(xrCreateReferenceSpace(impl->session, &space_info, &impl->view_space),
                     "xrCreateReferenceSpace(VIEW)")) {
        impl->DestroySession();
        return false;
    }

    impl->stop_requested = false;
    impl->frame_thread = std::jthread{[this](std::stop_token stop) { impl->FrameLoop(stop); }};
    LOG_INFO(Lib_Hmd, "OpenXR session created");
    return true;
}

void Runtime::Shutdown() {
    impl->DestroySession();
    if (impl->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(impl->instance);
        impl->instance = XR_NULL_HANDLE;
    }
    impl->system_id = XR_NULL_SYSTEM_ID;
}

bool Runtime::IsHeadsetActive() const {
    return impl->session_running;
}

std::optional<TrackingSnapshot> Runtime::GetTrackingSnapshot() const {
    std::scoped_lock lock{impl->state_mutex};
    return impl->snapshot;
}

void Runtime::Recenter() {
    impl->recenter_requested = true;
}

} // namespace Core::Xr
