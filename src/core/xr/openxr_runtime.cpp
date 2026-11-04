// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

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
    enum class RecenterMode : u32 { None, Full, Orientation };
    std::atomic<RecenterMode> recenter_requested{RecenterMode::Full};
    std::jthread frame_thread;

    struct InputActions {
        XrActionSet set{XR_NULL_HANDLE};
        XrAction grip_pose{XR_NULL_HANDLE};
        XrAction trigger{XR_NULL_HANDLE};
        XrAction haptic{XR_NULL_HANDLE};
        std::array<XrAction, 7> buttons{};
        XrPath hand_paths[2]{XR_NULL_PATH, XR_NULL_PATH};
        XrSpace grip_spaces[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    } input;
    std::atomic<float> vibration[2]{0.0f, 0.0f};
    float applied_vibration[2]{0.0f, 0.0f};
    std::chrono::steady_clock::time_point vibration_refresh[2]{};

    struct EyeSwapchain {
        XrSwapchain handle{XR_NULL_HANDLE};
        u32 width{};
        u32 height{};
        std::vector<VkImage> images;
        std::optional<u32> held_index;
        bool held_ready{false};
    };
    s64 swapchain_format{};
    mutable std::mutex layer_mutex;
    std::array<EyeSwapchain, 2> eye_swapchains{};
    bool layer_ready{false};
    XrCompositionLayerProjectionView layer_views[2]{};

    std::mutex callback_mutex;
    std::function<void()> frame_callback;

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
                recenter_requested = RecenterMode::Full;
                break;
            default:
                break;
            }
            event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    XrPath ToPath(const char* path) const {
        XrPath out = XR_NULL_PATH;
        Check(xrStringToPath(instance, path, &out), path);
        return out;
    }

    bool CreateAction(const char* name, const char* localized, XrActionType type,
                      XrAction& action) {
        XrActionCreateInfo info{
            .type = XR_TYPE_ACTION_CREATE_INFO,
            .actionType = type,
            .countSubactionPaths = 2,
            .subactionPaths = input.hand_paths,
        };
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        return Check(xrCreateAction(input.set, &info, &action), name);
    }

    struct Binding {
        XrAction InputActions::*action;
        s32 button;
        const char* path;
    };

    void SuggestBindings(const char* profile, std::initializer_list<Binding> bindings) {
        std::vector<XrActionSuggestedBinding> suggested;
        for (const auto& binding : bindings) {
            const XrAction action =
                binding.button >= 0 ? input.buttons[binding.button] : input.*binding.action;
            suggested.push_back({action, ToPath(binding.path)});
        }
        const XrInteractionProfileSuggestedBinding info{
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = ToPath(profile),
            .countSuggestedBindings = static_cast<u32>(suggested.size()),
            .suggestedBindings = suggested.data(),
        };
        Check(xrSuggestInteractionProfileBindings(instance, &info), profile);
    }

    bool CreateInput() {
        input.hand_paths[0] = ToPath("/user/hand/left");
        input.hand_paths[1] = ToPath("/user/hand/right");

        XrActionSetCreateInfo set_info{.type = XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(set_info.actionSetName, "psvr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(set_info.localizedActionSetName, "PlayStation VR",
                     XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        if (!Check(xrCreateActionSet(instance, &set_info, &input.set), "xrCreateActionSet")) {
            return false;
        }

        static constexpr std::array<std::pair<const char*, const char*>, 7> button_names{{
            {"cross", "Cross"},
            {"circle", "Circle"},
            {"square", "Square"},
            {"triangle", "Triangle"},
            {"move", "Move button"},
            {"start", "Start"},
            {"select", "Select"},
        }};
        bool ok = CreateAction("grip_pose", "Move controller pose", XR_ACTION_TYPE_POSE_INPUT,
                               input.grip_pose) &&
                  CreateAction("trigger", "T trigger", XR_ACTION_TYPE_FLOAT_INPUT, input.trigger) &&
                  CreateAction("rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, input.haptic);
        for (size_t i = 0; ok && i < button_names.size(); ++i) {
            ok = CreateAction(button_names[i].first, button_names[i].second,
                              XR_ACTION_TYPE_BOOLEAN_INPUT, input.buttons[i]);
        }
        if (!ok) {
            return false;
        }

        enum : s32 { Cross, Circle, Square, Triangle, MoveButton, Start, Select, None = -1 };
        constexpr auto pose = &InputActions::grip_pose;
        constexpr auto trigger = &InputActions::trigger;
        constexpr auto haptic = &InputActions::haptic;
        SuggestBindings("/interaction_profiles/khr/simple_controller",
                        {
                            {pose, None, "/user/hand/left/input/grip/pose"},
                            {pose, None, "/user/hand/right/input/grip/pose"},
                            {trigger, None, "/user/hand/left/input/select/click"},
                            {trigger, None, "/user/hand/right/input/select/click"},
                            {nullptr, Start, "/user/hand/left/input/menu/click"},
                            {nullptr, Start, "/user/hand/right/input/menu/click"},
                            {haptic, None, "/user/hand/left/output/haptic"},
                            {haptic, None, "/user/hand/right/output/haptic"},
                        });
        SuggestBindings("/interaction_profiles/valve/index_controller",
                        {
                            {pose, None, "/user/hand/left/input/grip/pose"},
                            {pose, None, "/user/hand/right/input/grip/pose"},
                            {trigger, None, "/user/hand/left/input/trigger/value"},
                            {trigger, None, "/user/hand/right/input/trigger/value"},
                            {nullptr, Cross, "/user/hand/left/input/a/click"},
                            {nullptr, Cross, "/user/hand/right/input/a/click"},
                            {nullptr, Circle, "/user/hand/left/input/b/click"},
                            {nullptr, Circle, "/user/hand/right/input/b/click"},
                            {nullptr, Square, "/user/hand/left/input/trackpad/force"},
                            {nullptr, Square, "/user/hand/right/input/trackpad/force"},
                            {nullptr, Triangle, "/user/hand/left/input/squeeze/force"},
                            {nullptr, Triangle, "/user/hand/right/input/squeeze/force"},
                            {nullptr, MoveButton, "/user/hand/left/input/thumbstick/click"},
                            {nullptr, MoveButton, "/user/hand/right/input/thumbstick/click"},
                            {haptic, None, "/user/hand/left/output/haptic"},
                            {haptic, None, "/user/hand/right/output/haptic"},
                        });
        SuggestBindings("/interaction_profiles/oculus/touch_controller",
                        {
                            {pose, None, "/user/hand/left/input/grip/pose"},
                            {pose, None, "/user/hand/right/input/grip/pose"},
                            {trigger, None, "/user/hand/left/input/trigger/value"},
                            {trigger, None, "/user/hand/right/input/trigger/value"},
                            {nullptr, Cross, "/user/hand/left/input/x/click"},
                            {nullptr, Cross, "/user/hand/right/input/a/click"},
                            {nullptr, Circle, "/user/hand/left/input/y/click"},
                            {nullptr, Circle, "/user/hand/right/input/b/click"},
                            {nullptr, Square, "/user/hand/left/input/thumbstick/click"},
                            {nullptr, Square, "/user/hand/right/input/thumbstick/click"},
                            {nullptr, Triangle, "/user/hand/left/input/squeeze/value"},
                            {nullptr, Triangle, "/user/hand/right/input/squeeze/value"},
                            {nullptr, Start, "/user/hand/left/input/menu/click"},
                            {haptic, None, "/user/hand/left/output/haptic"},
                            {haptic, None, "/user/hand/right/output/haptic"},
                        });
        SuggestBindings("/interaction_profiles/htc/vive_controller",
                        {
                            {pose, None, "/user/hand/left/input/grip/pose"},
                            {pose, None, "/user/hand/right/input/grip/pose"},
                            {trigger, None, "/user/hand/left/input/trigger/value"},
                            {trigger, None, "/user/hand/right/input/trigger/value"},
                            {nullptr, MoveButton, "/user/hand/left/input/trackpad/click"},
                            {nullptr, MoveButton, "/user/hand/right/input/trackpad/click"},
                            {nullptr, Triangle, "/user/hand/left/input/squeeze/click"},
                            {nullptr, Triangle, "/user/hand/right/input/squeeze/click"},
                            {nullptr, Start, "/user/hand/left/input/menu/click"},
                            {nullptr, Select, "/user/hand/right/input/menu/click"},
                            {haptic, None, "/user/hand/left/output/haptic"},
                            {haptic, None, "/user/hand/right/output/haptic"},
                        });

        for (int hand = 0; hand < 2; ++hand) {
            const XrActionSpaceCreateInfo space_info{
                .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
                .action = input.grip_pose,
                .subactionPath = input.hand_paths[hand],
                .poseInActionSpace = IdentityPose,
            };
            if (!Check(xrCreateActionSpace(session, &space_info, &input.grip_spaces[hand]),
                       "xrCreateActionSpace")) {
                return false;
            }
        }
        const XrSessionActionSetsAttachInfo attach_info{
            .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
            .countActionSets = 1,
            .actionSets = &input.set,
        };
        return Check(xrAttachSessionActionSets(session, &attach_info), "xrAttachSessionActionSets");
    }

    void DestroyInput() {
        for (auto& space : input.grip_spaces) {
            if (space != XR_NULL_HANDLE) {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }
        if (input.set != XR_NULL_HANDLE) {
            xrDestroyActionSet(input.set);
        }
        input = {};
    }

    bool SyncInput() {
        if (input.set == XR_NULL_HANDLE || session_state != XR_SESSION_STATE_FOCUSED) {
            return false;
        }
        const XrActiveActionSet active{input.set, XR_NULL_PATH};
        const XrActionsSyncInfo sync_info{
            .type = XR_TYPE_ACTIONS_SYNC_INFO,
            .countActiveActionSets = 1,
            .activeActionSets = &active,
        };
        return XR_SUCCEEDED(xrSyncActions(session, &sync_info));
    }

    void ReadController(int hand, XrTime display_time, ControllerSnapshot& out,
                        XrSpaceLocation& location, XrSpaceVelocity& velocity) {
        XrActionStateGetInfo get_info{
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .subactionPath = input.hand_paths[hand],
        };
        for (size_t i = 0; i < input.buttons.size(); ++i) {
            get_info.action = input.buttons[i];
            XrActionStateBoolean state{.type = XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &get_info, &state)) &&
                state.isActive && state.currentState) {
                out.buttons |= 1u << i;
            }
        }
        get_info.action = input.trigger;
        XrActionStateFloat trigger{.type = XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(session, &get_info, &trigger)) && trigger.isActive) {
            out.trigger = trigger.currentState;
        }

        velocity = {.type = XR_TYPE_SPACE_VELOCITY};
        location = {.type = XR_TYPE_SPACE_LOCATION, .next = &velocity};
        xrLocateSpace(input.grip_spaces[hand], local_space, display_time, &location);
    }

    void ApplyVibration() {
        if (input.haptic == XR_NULL_HANDLE) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        for (int hand = 0; hand < 2; ++hand) {
            const float amplitude = vibration[hand].load();
            const XrHapticActionInfo info{
                .type = XR_TYPE_HAPTIC_ACTION_INFO,
                .action = input.haptic,
                .subactionPath = input.hand_paths[hand],
            };
            if (amplitude <= 0.0f) {
                if (applied_vibration[hand] > 0.0f) {
                    xrStopHapticFeedback(session, &info);
                    applied_vibration[hand] = 0.0f;
                }
                continue;
            }
            if (amplitude == applied_vibration[hand] && now < vibration_refresh[hand]) {
                continue;
            }
            const XrHapticVibration haptic{
                .type = XR_TYPE_HAPTIC_VIBRATION,
                .duration = 500'000'000,
                .frequency = XR_FREQUENCY_UNSPECIFIED,
                .amplitude = amplitude,
            };
            xrApplyHapticFeedback(session, &info,
                                  reinterpret_cast<const XrHapticBaseHeader*>(&haptic));
            applied_vibration[hand] = amplitude;
            vibration_refresh[hand] = now + std::chrono::milliseconds(400);
        }
    }

    void ChooseSwapchainFormat() {
        u32 count = 0;
        if (!Check(xrEnumerateSwapchainFormats(session, 0, &count, nullptr),
                   "xrEnumerateSwapchainFormats") ||
            count == 0) {
            return;
        }
        std::vector<s64> formats(count);
        if (!Check(xrEnumerateSwapchainFormats(session, count, &count, formats.data()),
                   "xrEnumerateSwapchainFormats")) {
            return;
        }
        constexpr std::array preferred = {static_cast<s64>(VK_FORMAT_R8G8B8A8_SRGB),
                                          static_cast<s64>(VK_FORMAT_B8G8R8A8_SRGB)};
        for (const s64 format : preferred) {
            if (std::ranges::find(formats, format) != formats.end()) {
                swapchain_format = format;
                return;
            }
        }
        swapchain_format = formats.front();
        LOG_WARNING(Lib_Hmd, "OpenXR runtime offers no sRGB swapchain, using format {}",
                    swapchain_format);
    }

    void DestroySwapchains() {
        std::scoped_lock lock{layer_mutex, Vulkan::Scheduler::submit_mutex};
        for (auto& swapchain : eye_swapchains) {
            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
            }
            swapchain = {};
        }
        layer_ready = false;
    }

    std::optional<XrCompositionLayerProjection> BuildProjectionLayer(
        XrCompositionLayerProjectionView (&views)[2]) {
        if (!layer_ready) {
            return std::nullopt;
        }
        std::memcpy(views, layer_views, sizeof(views));
        return XrCompositionLayerProjection{
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .space = local_space,
            .viewCount = 2,
            .views = views,
        };
    }

    static constexpr XrSpaceLocationFlags PoseValid =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    static constexpr XrSpaceVelocityFlags VelocityValid =
        XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;

    bool LocateHead(XrTime display_time, XrSpaceLocation& head, XrView (&views)[2]) {
        if (!Check(xrLocateSpace(view_space, local_space, display_time, &head), "xrLocateSpace") ||
            (head.locationFlags & PoseValid) != PoseValid) {
            return false;
        }
        const XrViewLocateInfo locate_info{
            .type = XR_TYPE_VIEW_LOCATE_INFO,
            .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            .displayTime = display_time,
            .space = local_space,
        };
        XrViewState view_state{.type = XR_TYPE_VIEW_STATE};
        u32 view_count = 0;
        return Check(xrLocateViews(session, &locate_info, &view_state, 2, &view_count, views),
                     "xrLocateViews") &&
               view_count == 2 && (view_state.viewStateFlags & PoseValid) == PoseValid;
    }

    void SampleTracking(XrTime display_time) {
        XrSpaceVelocity head_velocity{.type = XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation head_location{.type = XR_TYPE_SPACE_LOCATION, .next = &head_velocity};
        XrView views[2]{{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
        const bool head_valid = LocateHead(display_time, head_location, views);

        XrSpaceLocation hand_locations[2]{};
        XrSpaceVelocity hand_velocities[2]{};
        TrackingSnapshot next{};
        if (SyncInput()) {
            for (int hand = 0; hand < 2; ++hand) {
                ReadController(hand, display_time, next.hands[hand], hand_locations[hand],
                               hand_velocities[hand]);
            }
            ApplyVibration();
        }

        const PxmPose xr_head = ToPxm(head_location.pose);
        const PxmPose xr_eyes[2] = {ToPxm(views[0].pose), ToPxm(views[1].pose)};
        if (head_valid) {
            const PxmFov left_fov = ToPxm(views[0].fov);
            const PxmFov right_fov = ToPxm(views[1].fov);
            for (int eye = 0; eye < 2; ++eye) {
                pxm_eye_offset(&xr_head, &xr_eyes[eye], &next.eye_offsets[eye]);
            }
            pxm_fov_to_orbis(&left_fov, &right_fov, &next.fov);
        }
        next.head_valid = head_valid;
        next.timestamp_us = Libraries::Kernel::sceKernelGetProcessTime();

        std::scoped_lock lock{state_mutex};
        next.sequence = snapshot ? snapshot->sequence + 1 : 1;
        if (head_valid) {
            switch (recenter_requested.exchange(RecenterMode::None)) {
            case RecenterMode::Full:
                pxm_tracking_space_recenter(&tracking_space, &xr_head);
                break;
            case RecenterMode::Orientation:
                pxm_tracking_space_recenter_orientation(&tracking_space, &xr_head);
                break;
            case RecenterMode::None:
                break;
            }
            pxm_xr_pose_to_orbis(&tracking_space, &xr_head, &next.head);
            for (int eye = 0; eye < 2; ++eye) {
                pxm_xr_pose_to_orbis(&tracking_space, &xr_eyes[eye], &next.eyes[eye]);
            }
            next.has_velocity = (head_velocity.velocityFlags & VelocityValid) == VelocityValid;
            if (next.has_velocity) {
                pxm_xr_vector_to_orbis(&tracking_space, ToPxm(head_velocity.linearVelocity),
                                       &next.linear_velocity);
                pxm_xr_vector_to_orbis(&tracking_space, ToPxm(head_velocity.angularVelocity),
                                       &next.angular_velocity);
            }
        } else if (snapshot) {
            next.head = snapshot->head;
            std::memcpy(next.eyes, snapshot->eyes, sizeof(next.eyes));
            std::memcpy(next.eye_offsets, snapshot->eye_offsets, sizeof(next.eye_offsets));
            next.fov = snapshot->fov;
        } else {
            return;
        }
        for (int hand = 0; hand < 2; ++hand) {
            auto& controller = next.hands[hand];
            const auto& location = hand_locations[hand];
            const auto& hand_velocity = hand_velocities[hand];
            controller.pose_valid = (location.locationFlags & PoseValid) == PoseValid;
            if (!controller.pose_valid) {
                continue;
            }
            const PxmPose grip = ToPxm(location.pose);
            pxm_xr_pose_to_orbis(&tracking_space, &grip, &controller.grip);
            controller.has_velocity =
                (hand_velocity.velocityFlags & VelocityValid) == VelocityValid;
            if (controller.has_velocity) {
                pxm_xr_vector_to_orbis(&tracking_space, ToPxm(hand_velocity.linearVelocity),
                                       &controller.linear_velocity);
                pxm_xr_vector_to_orbis(&tracking_space, ToPxm(hand_velocity.angularVelocity),
                                       &controller.angular_velocity);
            }
        }
        snapshot = next;
    }

    // Ends every headset frame with the most recent eye images, or no layers before the title
    // submits any, which keeps the session focused and the runtime predicting poses. The
    // runtime may touch the graphics queue inside these calls, so they share the scheduler's
    // submit lock.
    void FrameLoop(std::stop_token stop) {
        while (!stop.stop_requested() && !stop_requested) {
            PollEvents();
            if (!session_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            XrFrameState frame_state{.type = XR_TYPE_FRAME_STATE};
            if (!Check(xrWaitFrame(session, nullptr, &frame_state), "xrWaitFrame")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            {
                std::scoped_lock lock{callback_mutex};
                if (frame_callback) {
                    frame_callback();
                }
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
            std::scoped_lock lock{layer_mutex, Vulkan::Scheduler::submit_mutex};
            XrCompositionLayerProjectionView views[2]{};
            std::optional<XrCompositionLayerProjection> projection;
            if (frame_state.shouldRender) {
                projection = BuildProjectionLayer(views);
            }
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(projection ? &*projection
                                                                                 : nullptr),
            };
            const XrFrameEndInfo end_info{
                .type = XR_TYPE_FRAME_END_INFO,
                .displayTime = frame_state.predictedDisplayTime,
                .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                .layerCount = projection ? 1u : 0u,
                .layers = projection ? layers : nullptr,
            };
            Check(xrEndFrame(session, &end_info), "xrEndFrame");
        }
        session_running = false;
    }

    void DestroySession() {
        if (frame_thread.joinable()) {
            frame_thread.request_stop();
            frame_thread.join();
        }
        DestroyInput();
        DestroySwapchains();
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

// Never destroyed: teardown happens in Shutdown() while the Vulkan device is still alive, and
// by the time static destructors run the OpenXR loader may already be gone.
Runtime& Runtime::Instance() {
    static Runtime* runtime = new Runtime;
    return *runtime;
}

Runtime::Runtime() : impl{std::make_unique<Impl>()} {
    pxm_tracking_space_init(&impl->tracking_space, PxmVec3{0.0f, 0.0f, 0.0f});
}

Runtime::~Runtime() = default;

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

    impl->ChooseSwapchainFormat();
    if (!impl->CreateInput()) {
        LOG_ERROR(Lib_Hmd, "OpenXR controller input unavailable");
        impl->DestroyInput();
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
    if (!impl->session_running) {
        return std::nullopt;
    }
    std::scoped_lock lock{impl->state_mutex};
    return impl->snapshot;
}

void Runtime::Recenter() {
    impl->recenter_requested = Impl::RecenterMode::Full;
}

void Runtime::RecenterOrientation() {
    auto expected = Impl::RecenterMode::None;
    impl->recenter_requested.compare_exchange_strong(expected, Impl::RecenterMode::Orientation);
}

void Runtime::SetVibration(Hand hand, float amplitude) {
    impl->vibration[static_cast<u32>(hand)] = std::clamp(amplitude, 0.0f, 1.0f);
}

void Runtime::SetFrameCallback(std::function<void()> callback) {
    std::scoped_lock lock{impl->callback_mutex};
    impl->frame_callback = std::move(callback);
}

s64 Runtime::GetSwapchainFormat() const {
    return impl->session != XR_NULL_HANDLE ? impl->swapchain_format : 0;
}

bool Runtime::EyeSwapchainsMatch(const u32 width[2], const u32 height[2]) const {
    std::scoped_lock lock{impl->layer_mutex};
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto& swapchain = impl->eye_swapchains[eye];
        if (swapchain.handle == XR_NULL_HANDLE || swapchain.width != width[eye] ||
            swapchain.height != height[eye]) {
            return false;
        }
    }
    return true;
}

// The caller must make sure the GPU no longer uses the previous swapchain images.
bool Runtime::EnsureEyeSwapchains(const u32 width[2], const u32 height[2]) {
    if (impl->session == XR_NULL_HANDLE || impl->swapchain_format == 0 ||
        EyeSwapchainsMatch(width, height)) {
        return false;
    }
    impl->DestroySwapchains();

    std::scoped_lock lock{impl->layer_mutex, Vulkan::Scheduler::submit_mutex};
    for (u32 eye = 0; eye < 2; ++eye) {
        auto& swapchain = impl->eye_swapchains[eye];
        const XrSwapchainCreateInfo info{
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
            .format = impl->swapchain_format,
            .sampleCount = 1,
            .width = width[eye],
            .height = height[eye],
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };
        if (!impl->Check(xrCreateSwapchain(impl->session, &info, &swapchain.handle),
                         "xrCreateSwapchain")) {
            for (auto& created : impl->eye_swapchains) {
                if (created.handle != XR_NULL_HANDLE) {
                    xrDestroySwapchain(created.handle);
                }
                created = {};
            }
            return false;
        }
        u32 count = 0;
        std::vector<XrSwapchainImageVulkanKHR> images;
        bool enumerated =
            impl->Check(xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr),
                        "xrEnumerateSwapchainImages");
        if (enumerated && count > 0) {
            images.assign(count, {.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            enumerated =
                impl->Check(xrEnumerateSwapchainImages(
                                swapchain.handle, count, &count,
                                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                            "xrEnumerateSwapchainImages");
        }
        enumerated &= count > 0 && std::ranges::none_of(images, [](const auto& image) {
                          return image.image == VK_NULL_HANDLE;
                      });
        if (!enumerated) {
            for (auto& created : impl->eye_swapchains) {
                if (created.handle != XR_NULL_HANDLE) {
                    xrDestroySwapchain(created.handle);
                }
                created = {};
            }
            return false;
        }
        swapchain.width = width[eye];
        swapchain.height = height[eye];
        for (const auto& image : images) {
            swapchain.images.push_back(image.image);
        }
    }
    LOG_INFO(Lib_Hmd, "OpenXR eye swapchains: {}x{} and {}x{}", width[0], height[0], width[1],
             height[1]);
    return true;
}

std::vector<VkImage_T*> Runtime::GetEyeSwapchainImages(u32 eye) const {
    std::scoped_lock lock{impl->layer_mutex};
    return impl->eye_swapchains[eye].images;
}

// An acquired image stays held until PresentEyes, also across submissions that could not get
// both eyes, so the two eyes always change together.
std::optional<u32> Runtime::AcquireEyeImage(u32 eye) {
    XrSwapchain swapchain;
    std::optional<u32> index;
    {
        std::scoped_lock lock{impl->layer_mutex};
        auto& eye_swapchain = impl->eye_swapchains[eye];
        if (eye_swapchain.held_index && eye_swapchain.held_ready) {
            return eye_swapchain.held_index;
        }
        swapchain = eye_swapchain.handle;
        index = eye_swapchain.held_index;
    }
    if (swapchain == XR_NULL_HANDLE) {
        return std::nullopt;
    }
    if (!index) {
        u32 acquired = 0;
        std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
        if (!impl->Check(xrAcquireSwapchainImage(swapchain, nullptr, &acquired),
                         "xrAcquireSwapchainImage")) {
            return std::nullopt;
        }
        index = acquired;
    }
    const XrSwapchainImageWaitInfo wait_info{
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = 100'000'000,
    };
    const XrResult result = xrWaitSwapchainImage(swapchain, &wait_info);
    const bool ready = result == XR_SUCCESS;
    if (!ready && result != XR_TIMEOUT_EXPIRED) {
        impl->Check(result, "xrWaitSwapchainImage");
    }
    std::scoped_lock lock{impl->layer_mutex};
    auto& eye_swapchain = impl->eye_swapchains[eye];
    if (eye_swapchain.handle != swapchain) {
        return std::nullopt;
    }
    eye_swapchain.held_index = index;
    eye_swapchain.held_ready = ready;
    return ready ? index : std::nullopt;
}

void Runtime::PresentEyes(const EyeSubmission& submission) {
    const auto valid_fov = [](const PxmFov& fov) {
        return fov.angle_left < fov.angle_right && fov.angle_down < fov.angle_up;
    };
    PxmPose head_xr{};
    PxmVec3 eye_offsets[2]{};
    {
        std::scoped_lock lock{impl->state_mutex};
        pxm_orbis_pose_to_xr(&impl->tracking_space, &submission.head, &head_xr);
        if (impl->snapshot) {
            std::memcpy(eye_offsets, impl->snapshot->eye_offsets, sizeof(eye_offsets));
        }
    }

    std::scoped_lock lock{impl->layer_mutex, Vulkan::Scheduler::submit_mutex};
    auto& swapchains = impl->eye_swapchains;
    if (!swapchains[0].held_ready || !swapchains[1].held_ready) {
        return;
    }
    for (auto& swapchain : swapchains) {
        impl->Check(xrReleaseSwapchainImage(swapchain.handle, nullptr), "xrReleaseSwapchainImage");
        swapchain.held_index.reset();
        swapchain.held_ready = false;
    }
    if (!valid_fov(submission.fov[0]) || !valid_fov(submission.fov[1])) {
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            LOG_ERROR(Lib_Hmd, "Ignoring an eye submission with an empty field of view");
        }
        return;
    }
    for (u32 eye = 0; eye < 2; ++eye) {
        PxmPose eye_pose{};
        pxm_pose_apply_local_offset(&head_xr, eye_offsets[eye], &eye_pose);
        auto& view = impl->layer_views[eye];
        view = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        std::memcpy(&view.pose, &eye_pose, sizeof(view.pose));
        view.fov = {submission.fov[eye].angle_left, submission.fov[eye].angle_right,
                    submission.fov[eye].angle_up, submission.fov[eye].angle_down};
        view.subImage.swapchain = swapchains[eye].handle;
        view.subImage.imageRect = {
            {0, 0},
            {static_cast<s32>(submission.width[eye]), static_cast<s32>(submission.height[eye])}};
    }
    impl->layer_ready = true;
}

} // namespace Core::Xr
