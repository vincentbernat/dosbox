/*
 *  Copyright (C) 2022-2022  The DOSBox Staging Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "mouse_manymouse.h"
#include "mouse_common.h"
#include "mouse_config.h"

#include "callback.h"
#include "checks.h"
#include "math_utils.h"
#include "pic.h"
#include "string_utils.h"

CHECK_NARROWING();


void manymouse_tick(uint32_t)
{
#if C_MANYMOUSE
    ManyMouseGlue::GetInstance().Tick();
#endif // C_MANYMOUSE
}

MousePhysical::MousePhysical(const std::string &name) :
    name(name)
{
}

bool MousePhysical::IsMapped() const
{
    return mapped_id != MouseInterfaceId::None;
}

bool MousePhysical::IsDisconnected() const
{
    return disconnected;
}

MouseInterfaceId MousePhysical::GetMappedInterfaceId() const
{
    return mapped_id;
}

const std::string &MousePhysical::GetName() const
{
    return name;
}

ManyMouseGlue &ManyMouseGlue::GetInstance()
{
    static ManyMouseGlue *instance = nullptr;
    if (!instance)
        instance = new ManyMouseGlue();
    return *instance;
}

#if C_MANYMOUSE

void ManyMouseGlue::InitIfNeeded()
{
    if (initialized || malfunction)
        return;

    num_mice = ManyMouse_Init();
    if (num_mice < 0) {
        LOG_ERR("MOUSE: ManyMouse initialization failed");
        ManyMouse_Quit();
        malfunction = true;
        return;
    }
    initialized = true;

    if (num_mice >= max_mice) {
        num_mice = max_mice - 1;
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_ERR("MOUSE: Up to %d simultaneously connected mice supported",
                    max_mice);
        }
    }

    const auto new_driver_name = std::string(ManyMouse_DriverName());
    if (new_driver_name != driver_name) {
        driver_name = new_driver_name;
        LOG_INFO("MOUSE: ManyMouse driver '%s'", driver_name.c_str());
    }

    Rescan();
}

void ManyMouseGlue::ShutdownIfSafe()
{
    if (is_mapping_in_effect || config_api_counter)
        return;

    ShutdownForced();
}

void ManyMouseGlue::ShutdownForced()
{
    if (!initialized)
        return;

    PIC_RemoveEvents(manymouse_tick);
    ManyMouse_Quit();
    ClearPhysicalMice();
    num_mice = 0;
    initialized = false;
}

void ManyMouseGlue::StartConfigAPI()
{
    // Config API object is being created
    ++config_api_counter;
    assert(config_api_counter > 0);
}

void ManyMouseGlue::StopConfigAPI()
{
    // Config API object is being destroyed
    assert(config_api_counter > 0);
    config_api_counter--;
    ShutdownIfSafe();
    if (!config_api_counter)
        rescan_blocked_config = false;   
}

void ManyMouseGlue::ClearPhysicalMice()
{
    mouse_info.physical.clear();
    physical_devices.clear();
    rel_x.clear();
    rel_y.clear();
}

void ManyMouseGlue::Rescan()
{
    if (config_api_counter)
        // Do not allow another rescan until MouseConfigAPI
        // stops being used, it would be unsafe due to possible
        // changes of physical device list size/indices
        rescan_blocked_config = true;

    ClearPhysicalMice();

    for (uint8_t idx = 0; idx < num_mice; idx++) {
        const auto name_utf8 = ManyMouse_DeviceName(idx);
        std::string name;
        UTF8_RenderForDos(name_utf8, name);

        const char character_nbsp  = 0x7f; // non-breaking space
        const char character_space = 0x20;

        for (auto pos = name.size(); pos > 0; pos--) {
            // Replace non-breaking space with a regular space
            if (name[pos - 1] == character_nbsp)
                name[pos - 1] = character_space;
            // Remove non-ASCII and control characters
            if (name[pos - 1] < character_space ||
                name[pos - 1] >= character_nbsp)
                name.erase(pos - 1, 1);
        }

        // Try to rework into something useful names in the forms
        // 'FooBar Corp FooBar Corp Incredible Mouse'
        size_t pos = name.size() / 2 + 1;
        while (--pos > 2) {
            if (name[pos - 1] != ' ')
                continue;
            const std::string tmp = name.substr(0, pos);
            if (name.rfind(tmp + tmp, 0) == std::string::npos)
                continue;
            name = name.substr(pos);
            break;
        }

        // ManyMouse should limit device names to 64 characters,
        // but make sure name is indeed limited in length, and
        // strip trailing spaces
        name.resize(std::min(static_cast<size_t>(64), name.size()));
        while (!name.empty() && name.back() == ' ')
            name.pop_back();

        physical_devices.emplace_back(MousePhysical(name));
        mouse_info.physical.emplace_back(
            MousePhysicalInfoEntry(static_cast<uint8_t>(physical_devices.size() - 1)));
    }
}

void ManyMouseGlue::RescanIfSafe()
{
    if (rescan_blocked_config)
        return;

    ShutdownIfSafe();
    InitIfNeeded();
}

bool ManyMouseGlue::ProbeForMapping(uint8_t &device_id)
{
    // Wait a little to speedup screen update
    const auto pic_ticks_start = PIC_Ticks;
    while (PIC_Ticks >= pic_ticks_start &&
           PIC_Ticks - pic_ticks_start < 50)
           CALLBACK_Idle();

    // Make sure the module is initialized,
    // but suppress default event handling
    InitIfNeeded();
    if (!initialized)
        return false;
    PIC_RemoveEvents(manymouse_tick);

    // Flush events, handle critical ones
    ManyMouseEvent event;
    while (ManyMouse_PollEvent(&event))
        HandleEvent(event, true); // handle critical events

    bool success = false;
    while (true) {
        // Poll mouse events, handle critical ones
        if (!ManyMouse_PollEvent(&event)) {
            CALLBACK_Idle();
            continue;
        }
        if (event.device >= max_mice)
            continue;
        HandleEvent(event, true);

        // Wait for mouse button press
        if (event.type != MANYMOUSE_EVENT_BUTTON || !event.value)
            continue;
        device_id = static_cast<uint8_t>(event.device);

        if (event.item >= 1)
            break; // user cancelled the interactive mouse mapping

        // Do not accept already mapped devices
        bool already_mapped = false;
        for (const auto &interface : mouse_interfaces)
            if (interface->IsMapped(device_id))
                already_mapped = true;
        if (already_mapped)
            continue;

        // Mouse probed successfully
        device_id = static_cast<uint8_t>(event.device);
        success = true;
        break;
    }

    if (is_mapping_in_effect && !mouse_config.no_mouse)
        PIC_AddEvent(manymouse_tick, tick_interval);
    return success;
}

uint8_t ManyMouseGlue::GetIdx(const std::regex &regex)
{
    // Try to match the mouse name which is not mapped yet

    for (size_t i = 0; i < physical_devices.size(); i++) {
        const auto &physical_device = physical_devices[i];

        if (physical_device.IsDisconnected() ||
            !std::regex_match(physical_device.GetName(), regex))
            // mouse disconnected or name does not match
            continue;

        if (physical_device.GetMappedInterfaceId() == MouseInterfaceId::None)
            // name matches, mouse not mapped yet - use it!
            return static_cast<uint8_t>(i);
    }

    return max_mice; // return value which will be considered out of range
}

void ManyMouseGlue::Map(const uint8_t physical_idx,
                        const MouseInterfaceId interface_id)
{
    assert(interface_id != MouseInterfaceId::None);

    if (physical_idx >= physical_devices.size()) {
        UnMap(interface_id);
        return;
    }

    auto &physical_device = physical_devices[physical_idx];
    if (interface_id == physical_device.GetMappedInterfaceId())
        return; // nothing to update
    physical_device.mapped_id = interface_id;

    MapFinalize();
}

void ManyMouseGlue::UnMap(const MouseInterfaceId interface_id)
{
    for (auto &physical_device : physical_devices) {
        if (interface_id != physical_device.GetMappedInterfaceId())
            continue; // not a device to unmap
        physical_device.mapped_id = MouseInterfaceId::None;
        break;
    }

    MapFinalize();
}

void ManyMouseGlue::MapFinalize()
{
    PIC_RemoveEvents(manymouse_tick);
    is_mapping_in_effect = false;
    for (const auto &entry : mouse_info.physical) {
        if (entry.IsMapped())
            continue;

        is_mapping_in_effect = true;
        if (!mouse_config.no_mouse)
            PIC_AddEvent(manymouse_tick, tick_interval);
        break;
    }
}

bool ManyMouseGlue::IsMappingInEffect() const
{
    return is_mapping_in_effect;
}

void ManyMouseGlue::HandleEvent(const ManyMouseEvent &event,
                                const bool critical_only)
{
    if (GCC_UNLIKELY(event.device >= mouse_info.physical.size()))
        return; // device ID out of supported range
    if (GCC_UNLIKELY(mouse_config.no_mouse &&
                     event.type != MANYMOUSE_EVENT_DISCONNECT))
        return; // mouse control disabled in GUI

    const auto device_idx   = static_cast<uint8_t>(event.device);
    const auto interface_id = physical_devices[device_idx].GetMappedInterfaceId();
    const bool no_interface = (interface_id == MouseInterfaceId::None);

    switch (event.type) {

    case MANYMOUSE_EVENT_ABSMOTION:
        // LOG_INFO("MANYMOUSE #%u ABSMOTION axis %d, %d", event.device, event.item, event.value);
        break;

    case MANYMOUSE_EVENT_RELMOTION:
        // LOG_INFO("MANYMOUSE #%u RELMOTION axis %d, %d", event.device, event.item, event.value);
        if (no_interface || critical_only)
            break; // movements not relevant at this moment
        if (event.item != 0 && event.item != 1)
            break; // only movements related to x and y axis are relevant

        if (rel_x.size() <= device_idx) {
            rel_x.resize(static_cast<size_t>(device_idx + 1), 0);
            rel_y.resize(static_cast<size_t>(device_idx + 1), 0);
        }

        if (event.item)
            rel_y[event.device] += event.value; // event.item 1
        else
            rel_x[event.device] += event.value; // event.item 0
        break;

    case MANYMOUSE_EVENT_BUTTON:
        // LOG_INFO("MANYMOUSE #%u BUTTON %u %s", event.device, event.item, event.value ? "press" : "release");
        if (no_interface || (critical_only && !event.value) || (event.item >= max_buttons))
            // TODO: Consider supporting extra mouse buttons
            // in the future. On Linux event items 3-7 are for
            // scroll wheel(s), 8 is for SDL button X1, 9 is
            // for X2, etc. - but I don't know yet if this
            // is consistent across various platforms
            break;
        MOUSE_EventButton(static_cast<uint8_t>(event.item), event.value, interface_id);
        break;

    case MANYMOUSE_EVENT_SCROLL:
        // LOG_INFO("MANYMOUSE #%u WHEEL #%u %d", event.device, event.item, event.value);
        if (no_interface || critical_only || (event.item != 0))
            break; // only the 1st wheel is supported
        MOUSE_EventWheel(clamp_to_int16(-event.value), interface_id);
        break;

    case MANYMOUSE_EVENT_DISCONNECT:
        // LOG_INFO("MANYMOUSE #%u DISCONNECT", event.device);
        physical_devices[event.device].disconnected = true;
        for (uint8_t button = 0; button < max_buttons; button++)
            MOUSE_EventButton(button, false, interface_id);
        MOUSE_NotifyDisconnect(interface_id);
        break;

    default:
        // LOG_INFO("MANYMOUSE #%u (other event)", event.device);
        break;
    }
}

void ManyMouseGlue::Tick()
{
    assert(!mouse_config.no_mouse);

    // Handle all the events from the queue
    ManyMouseEvent event;
    while (ManyMouse_PollEvent(&event))
        HandleEvent(event);

    // Report accumulated mouse movements
    for (uint8_t idx = 0; idx < rel_x.size(); idx++) {
        if (rel_x[idx] == 0 && rel_y[idx] == 0)
            continue;

        const auto interface_id = physical_devices[idx].GetMappedInterfaceId();
        MOUSE_EventMoved(static_cast<float>(rel_x[idx]),
                         static_cast<float>(rel_y[idx]),
                         interface_id);
        rel_x[idx] = 0;
        rel_y[idx] = 0;
    }

    if (is_mapping_in_effect)
        PIC_AddEvent(manymouse_tick, tick_interval);
}

#else

// ManyMouse is not available

void ManyMouseGlue::RescanIfSafe()
{
    static bool already_warned = false;
    if (!already_warned) {
        LOG_ERR("MOUSE: This build has no ManyMouse support");
        already_warned = true;
    }
}

void ManyMouseGlue::ShutdownIfSafe()
{
}

void ManyMouseGlue::StartConfigAPI()
{
}

void ManyMouseGlue::StopConfigAPI()
{
}

bool ManyMouseGlue::ProbeForMapping(uint8_t &)
{
    return false;
}

uint8_t ManyMouseGlue::GetIdx(const std::regex &)
{
    return UINT8_MAX;
}

void ManyMouseGlue::Map(const uint8_t, const MouseInterfaceId)
{
}

bool ManyMouseGlue::IsMappingInEffect() const
{
    return false;
}

#endif // C_MANYMOUSE