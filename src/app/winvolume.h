// SPDX-License-Identifier: GPL-3.0-or-later
// The volume of Windows' default playback device - what the keyboard's volume keys, the taskbar and the volume
// flyout change. Read on the GUI thread by polling (COM in the GUI thread's apartment); the default device is looked
// up again every second, so a new default device is followed.
#pragma once

#include <string>

struct IMMDeviceEnumerator;
struct IAudioEndpointVolume;

namespace ap {

class WindowsVolume {
public:
    WindowsVolume() = default;
    ~WindowsVolume();
    WindowsVolume(const WindowsVolume&) = delete;
    WindowsVolume& operator=(const WindowsVolume&) = delete;

    // Reads the volume (call from the GUI timer). False when there is no default playback device.
    bool poll();
    // Releases the COM objects (before the GUI thread leaves its apartment); a later poll opens them again.
    void close();
    bool valid() const { return volume_ != nullptr; }
    // The volume as a gain (what Windows applies; 0 when muted or at 0 %), never above 1.
    double gain() const { return gain_; }
    double percent() const { return 100.0 * scalar_; } // as the volume slider shows it
    bool muted() const { return muted_; }
    // Windows applies the volume to the signal itself (the device's driver has no volume of its own).
    bool software() const { return software_; }
    const std::string& device() const { return name_; } // friendly name, UTF-8

private:
    void release();
    bool open();

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IAudioEndpointVolume* volume_ = nullptr;
    std::wstring id_;
    std::string name_;
    double gain_ = 1.0, scalar_ = 1.0;
    bool muted_ = false, software_ = false;
    int ticks_ = 0;
};

} // namespace ap
