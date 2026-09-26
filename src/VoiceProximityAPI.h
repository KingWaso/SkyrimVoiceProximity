// ---------------------------------------------------------------------------
// VoiceProximityAPI.h
//
// Small interface exposed by SkyrimVoiceProximity.cpp for VoiceOverlay.cpp
// to call into. Keeps the DX11/ImGui rendering code from needing to know
// about miniaudio, sockets, or the packet formats directly.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VoiceProximity
{
    struct CaptureDeviceInfo
    {
        int   index; // pass back into SetCaptureDeviceIndex
        std::string name;
    };

    struct PlayerVolumeInfo
    {
        uint32_t    playerId;
        std::string username; // may be empty if no packet received from them yet
        float       volume;   // 0.0 - 2.0, user-set multiplier on top of distance falloff
        bool        talking;  // true if we've received audio from them in the last ~500ms
    };

    // Mic devices --------------------------------------------------------
    std::vector<CaptureDeviceInfo> GetCaptureDevices();
    int  GetCaptureDeviceIndex();          // currently active device, -1 = system default
    void SetCaptureDeviceIndex(int index); // -1 = system default

    // Volume ---------------------------------------------------------------
    float GetMasterVolume();               // 0.0 - 2.0
    void  SetMasterVolume(float volume);

    std::vector<PlayerVolumeInfo> GetKnownPlayers();
    void SetPlayerVolume(uint32_t playerId, float volume);
}
