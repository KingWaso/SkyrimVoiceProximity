#pragma once

// Tiny loopback TCP server exposing VoiceProximityAPI.h to the external
// VoiceCompanion.exe window as a plain-text line protocol. Call Start()
// once (from the SKSE kDataLoaded handler) and Stop() on shutdown.
namespace IpcServer
{
    void Start();
    void Stop();
}
