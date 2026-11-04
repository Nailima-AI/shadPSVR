// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/xr/openxr_runtime.h"
#include "move.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Libraries::Move {

static bool g_library_initialized = false;

static constexpr s32 MaxControllers = 2;
static constexpr float SphereRadius = 0.0225f;
static std::array<s32, MaxControllers> g_handles{-1, -1};

static constexpr std::array<std::pair<u32, u16>, 7> ButtonMap{{
    {Core::Xr::CONTROLLER_BUTTON_CROSS, ORBIS_MOVE_BUTTON_CROSS},
    {Core::Xr::CONTROLLER_BUTTON_CIRCLE, ORBIS_MOVE_BUTTON_CIRCLE},
    {Core::Xr::CONTROLLER_BUTTON_SQUARE, ORBIS_MOVE_BUTTON_SQUARE},
    {Core::Xr::CONTROLLER_BUTTON_TRIANGLE, ORBIS_MOVE_BUTTON_TRIANGLE},
    {Core::Xr::CONTROLLER_BUTTON_MOVE, ORBIS_MOVE_BUTTON_MOVE},
    {Core::Xr::CONTROLLER_BUTTON_START, ORBIS_MOVE_BUTTON_START},
    {Core::Xr::CONTROLLER_BUTTON_SELECT, ORBIS_MOVE_BUTTON_SELECT},
}};

// The first Move a title opens is treated as the right hand.
static s32 HandForIndex(s32 index) {
    return index == 0 ? static_cast<s32>(Core::Xr::Hand::Right)
                      : static_cast<s32>(Core::Xr::Hand::Left);
}

s32 GetMoveHandIndex(s32 handle) {
    for (s32 index = 0; index < MaxControllers; ++index) {
        if (g_handles[index] != -1 && g_handles[index] == handle) {
            return HandForIndex(index);
        }
    }
    return -1;
}

static const Core::Xr::ControllerSnapshot* FindController(
    s32 handle, const std::optional<Core::Xr::TrackingSnapshot>& snapshot) {
    const s32 hand = GetMoveHandIndex(handle);
    if (hand < 0 || !snapshot || !snapshot->hands[hand].connected) {
        return nullptr;
    }
    return &snapshot->hands[hand];
}

static void FillMoveData(const Core::Xr::ControllerSnapshot& controller,
                         const Core::Xr::TrackingSnapshot& snapshot, OrbisMoveData* data) {
    std::memset(data, 0, sizeof(OrbisMoveData));
    PxmMoveState state{};
    pxm_move_from_grip(&controller.grip,
                       controller.has_velocity ? controller.angular_velocity : PxmVec3{}, &state);
    data->accelerometer[0] = state.accelerometer.x;
    data->accelerometer[1] = state.accelerometer.y;
    data->accelerometer[2] = state.accelerometer.z;
    data->gyro[0] = state.gyro.x;
    data->gyro[1] = state.gyro.y;
    data->gyro[2] = state.gyro.z;
    u16 buttons = 0;
    for (const auto [xr_button, move_button] : ButtonMap) {
        if (controller.buttons & xr_button) {
            buttons |= move_button;
        }
    }
    const u16 trigger = static_cast<u16>(std::clamp(controller.trigger, 0.0f, 1.0f) * 255.0f);
    if (trigger >= 128) {
        buttons |= ORBIS_MOVE_BUTTON_T;
    }
    data->button_data.button_data = buttons;
    data->button_data.trigger_data = trigger;
    data->timestamp = static_cast<s64>(snapshot.timestamp_us);
    data->count = static_cast<s32>(snapshot.sequence);
    data->temperature = 25.0f;
}

s32 PS4_SYSV_ABI sceMoveInit() {
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    if (!Core::Xr::Runtime::Instance().IsRequested()) {
        LOG_WARNING(Lib_Move, "Move controllers are not supported yet");
    }
    g_library_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Even when no controllers are connected, this returns a proper handle.
    // Internal libSceVrTracker logic requires this handle to be different from other devices.
    static s32 handle = 0x30b0000;
    handle += 0x100;
    if (index >= 0 && index < MaxControllers) {
        g_handles[index] = handle;
    }
    return handle;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (!FindController(handle, Core::Xr::Runtime::Instance().GetTrackingSnapshot())) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    info->sphere_radius = SphereRadius;
    info->accelerometer_offset[0] = 0.0f;
    info->accelerometer_offset[1] = 0.0f;
    info->accelerometer_offset[2] = 0.0f;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    LOG_TRACE(Lib_Move, "(called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const auto snapshot = Core::Xr::Runtime::Instance().GetTrackingSnapshot();
    const auto* controller = FindController(handle, snapshot);
    if (!controller) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    FillMoveData(*controller, *snapshot, data);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const auto snapshot = Core::Xr::Runtime::Instance().GetTrackingSnapshot();
    const auto* controller = FindController(handle, snapshot);
    if (!controller) {
        *out_count = 0;
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    // Only the latest OpenXR sample is kept, so at most one state is newer than the caller's.
    if (snapshot->timestamp_us <= static_cast<u64>(timestamp)) {
        *out_count = 0;
        return ORBIS_OK;
    }
    FillMoveData(*controller, *snapshot, data);
    *out_count = 1;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, void* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const s32 hand = GetMoveHandIndex(handle);
    if (hand >= 0) {
        // Always accepted, so a stop request is never lost to a tracking or connection gap.
        Core::Xr::Runtime::Instance().SetVibration(static_cast<Core::Xr::Hand>(hand),
                                                   static_cast<float>(intensity) / 255.0f);
    }
    if (!FindController(handle, Core::Xr::Runtime::Instance().GetTrackingSnapshot())) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (!FindController(handle, Core::Xr::Runtime::Instance().GetTrackingSnapshot())) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Returns success, even if no controllers are connected.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (const s32 hand = GetMoveHandIndex(handle); hand >= 0) {
        Core::Xr::Runtime::Instance().SetVibration(static_cast<Core::Xr::Hand>(hand), 0.0f);
    }
    for (auto& registered : g_handles) {
        if (registered == handle) {
            registered = -1;
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    Core::Xr::Runtime::Instance().SetVibration(Core::Xr::Hand::Left, 0.0f);
    Core::Xr::Runtime::Instance().SetVibration(Core::Xr::Hand::Right, 0.0f);
    g_handles.fill(-1);
    g_library_initialized = false;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move