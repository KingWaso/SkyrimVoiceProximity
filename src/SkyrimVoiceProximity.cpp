// ---------------------------------------------------------------------------
// SkyrimVoiceProximity.cpp
//
// SKSE plugin for Skyrim Together Reborn: captures the local player's mic,
// streams it as raw PCM over UDP to the STR server's voice relay (separate
// component, not yet built), and plays back other connected players' audio
// with distance-based volume falloff computed from positions the relay
// forwards alongside each packet.
//
// This plugin runs INSIDE SkyrimTogether.exe (loaded by SKSE), so position
// reads are a direct pointer dereference off the module base -- no
// ReadProcessMemory / cross-process attach needed, unlike the earlier
// external Mumble plugin.
//
// Dependencies:
//   - SKSE64 / CommonLibSSE-NG (for the plugin scaffolding + messaging)
//   - miniaudio.h (https://miniaud.io/ -- single header, drop it next to
//     this file, no separate build step)
//   - Winsock2 (ws2_32.lib, part of the Windows SDK)
//
// NOT yet included: Opus compression. This sends raw 16-bit PCM to keep the
// build simple while you get the pipeline working end to end. Swap in Opus
// later (see the ENCODE/DECODE seams marked below) once you're happy with
// packet flow, and bandwidth becomes a concern.
//
// Build (CMake, CommonLibSSE-NG project layout):
//   target_sources(YourPlugin PRIVATE SkyrimVoiceProximity.cpp)
//   target_link_libraries(YourPlugin PRIVATE ws2_32)
//   (miniaudio.h needs no linking -- it's header-only; define
//    MINIAUDIO_IMPLEMENTATION in exactly one .cpp, done below)
// ---------------------------------------------------------------------------

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "VoiceProximityAPI.h"
#include "IpcServer.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// SKSE / CommonLibSSE-NG headers -- adjust include paths to your project.
#include "REL/Relocation.h"
#include "SKSE/API.h"

#pragma comment(lib, "ws2_32.lib")

// ===========================================================================
// CONFIG
// ===========================================================================

// The STR server IP is already known (15.235.31.125:28569 is the game
// port). The voice relay needs its OWN UDP port on the same box -- pick one
// that isn't already used by STR and open it in the Shockbyte panel's port
// config once the server-side relay exists. Placeholder for now.
static const char    *RELAY_IP   = "15.235.31.125";
static const uint16_t RELAY_PORT = 28570;

// Position offsets confirmed via Cheat Engine (module-relative RVAs).
static const uintptr_t OFF_POS_X = 0x338C8C0;
static const uintptr_t OFF_POS_Y = 0x338C8C4;
static const uintptr_t OFF_POS_Z = 0x202AED4;

// Audio format: keep this simple and fixed for every client so nobody's
// plugin has to negotiate anything.
static const ma_uint32 SAMPLE_RATE     = 48000;
static const ma_uint32 CHANNELS        = 1; // mono capture
static const int       FRAME_MS        = 20;
static const int       FRAME_SAMPLES   = SAMPLE_RATE * FRAME_MS / 1000; // 960
static const int       FRAME_BYTES     = FRAME_SAMPLES * sizeof(int16_t);

// Volume model: full volume inside NEAR units, silence beyond FAR units,
// linear falloff between. Skyrim world units, same scale as your position
// reads (~70 units/metre), so NEAR=700 is ~10m, FAR=3500 is ~50m.
static const float ATTEN_NEAR = 700.0f;
static const float ATTEN_FAR  = 3500.0f;

// Simple voice-activity gate so you're not streaming silence 24/7.
static const int16_t VAD_THRESHOLD = 400; // raw PCM amplitude

// ===========================================================================
// Wire format
//
// Outgoing (client -> relay):   [seq:u16][pcm: FRAME_BYTES]
//   Player identity is implied by source IP:port once the relay has you
//   registered; add an explicit player-id field here if your relay design
//   needs it (e.g. multiple clients behind the same NAT/IP).
//
// Incoming (relay -> client):   [playerId:u32][posX:f32][posY:f32][posZ:f32]
//                                [seq:u16][pcm: FRAME_BYTES]
// ===========================================================================

#pragma pack(push, 1)
struct OutgoingPacket {
    uint16_t seq;
    int16_t  pcm[FRAME_SAMPLES];
};

struct IncomingPacket {
    uint32_t playerId;
    float    posX, posY, posZ;
    uint16_t seq;
    char     username[32];
    int16_t  pcm[FRAME_SAMPLES];
};
#pragma pack(pop)

// ===========================================================================
// Local player position (direct in-process read -- no cross-process API)
// ===========================================================================

static bool readLocalPos(float &x, float &y, float &z) {
    uintptr_t base = reinterpret_cast< uintptr_t >(GetModuleHandleA(nullptr));
    if (!base)
        return false;

    auto *px = reinterpret_cast< float * >(base + OFF_POS_X);
    auto *py = reinterpret_cast< float * >(base + OFF_POS_Y);
    auto *pz = reinterpret_cast< float * >(base + OFF_POS_Z);

    // Guard against reading unmapped memory during loading screens / teardown.
    if (IsBadReadPtr(px, sizeof(float)) || IsBadReadPtr(py, sizeof(float)) ||
        IsBadReadPtr(pz, sizeof(float)))
        return false;

    x = *px;
    y = *py;
    z = *pz;
    return true;
}

// ===========================================================================
// Registration -- tells the relay which STR connection this UDP socket
// belongs to, so it can forward audio using the source address instead of
// requiring an identity field on every single audio packet.
// ===========================================================================

static const uint32_t REGISTER_MAGIC = 0x53565243; // 'SVRC', must match the relay
static const size_t   MAX_USERNAME   = 64;

#pragma pack(push, 1)
struct RegisterPacket {
    uint32_t magic;
    char     username[MAX_USERNAME];
};
#pragma pack(pop)

// TODO: this assumes the in-game character display name matches whatever
// PlayerManager/Player::GetUsername() returns server-side. If STR assigns a
// separate multiplayer username that differs from the Skyrim character
// name, swap this for whatever API exposes that to an SKSE plugin instead.
static std::string getLocalUsername() {
    if (auto *pc = RE::PlayerCharacter::GetSingleton())
        return std::string(pc->GetName());
    return "";
}

static void sendRegister(SOCKET sock, const sockaddr_in &relayAddr) {
    RegisterPacket pkt {};
    pkt.magic = REGISTER_MAGIC;

    std::string name = getLocalUsername();
    strncpy_s(pkt.username, name.c_str(), MAX_USERNAME - 1);

    sendto(sock, reinterpret_cast<const char *>(&pkt), sizeof(pkt), 0,
          reinterpret_cast<const sockaddr *>(&relayAddr), sizeof(relayAddr));
}

// ===========================================================================
// Networking
// ===========================================================================

static SOCKET          g_sock = INVALID_SOCKET;
static sockaddr_in     g_relayAddr {};
static std::atomic<bool> g_running { false };

static bool initNetworking() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET)
        return false;

    // Non-blocking recv so the network thread can also service a shutdown
    // flag instead of hanging forever on recvfrom.
    u_long nonBlocking = 1;
    ioctlsocket(g_sock, FIONBIO, &nonBlocking);

    g_relayAddr.sin_family = AF_INET;
    g_relayAddr.sin_port   = htons(RELAY_PORT);
    inet_pton(AF_INET, RELAY_IP, &g_relayAddr.sin_addr);

    return true;
}

static void shutdownNetworking() {
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

// ===========================================================================
// Remote player audio state -- one jitter-free single-frame slot per player.
// Good enough for proximity chat; swap for a real jitter buffer if you get
// choppy audio under packet reordering.
// ===========================================================================

struct RemotePlayer {
    float    posX = 0, posY = 0, posZ = 0;
    int16_t  pcm[FRAME_SAMPLES] {};
    uint32_t writeCursor = 0; // playback read position within pcm[]
    bool     hasAudio    = false;
    float    gain        = 0.0f; // last computed distance gain, for overlay display only

    std::string username;
    float       userVolume = 1.0f; // 0.0 - 2.0, set via the overlay
    std::chrono::steady_clock::time_point lastPacket{};
};

// ---------------------------------------------------------------------------
// Overlay-facing state. Guarded by g_playersMutex (players) and its own
// small mutex (master volume / device list) since those are touched far
// less often than the per-frame audio path.
// ---------------------------------------------------------------------------
static std::atomic<float> g_masterVolume{1.0f};

static std::mutex                                  g_playersMutex;
static std::unordered_map<uint32_t, RemotePlayer>   g_players;

static float distanceGain(float dx, float dy, float dz) {
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist <= ATTEN_NEAR)
        return 1.0f;
    if (dist >= ATTEN_FAR)
        return 0.0f;
    return 1.0f - (dist - ATTEN_NEAR) / (ATTEN_FAR - ATTEN_NEAR);
}

// ===========================================================================
// Network receive thread: parse incoming packets, decode (currently: raw
// PCM, no decode step needed -- see Opus seam below), stash per-player.
// ===========================================================================

static void networkThreadFn() {
    std::vector<uint8_t> buf(sizeof(IncomingPacket) + 64);

    sendRegister(g_sock, g_relayAddr);
    auto lastRegister = std::chrono::steady_clock::now();

    while (g_running.load()) {
        // Retry registration periodically -- cheap, and covers the relay
        // not being up yet, UDP packet loss, or the relay restarting and
        // losing its client map.
        auto now = std::chrono::steady_clock::now();
        if (now - lastRegister > std::chrono::seconds(5)) {
            sendRegister(g_sock, g_relayAddr);
            lastRegister = now;
        }

        sockaddr_in from {};
        int fromLen = sizeof(from);

        int n = recvfrom(g_sock, reinterpret_cast<char *>(buf.data()),
                         static_cast<int>(buf.size()), 0,
                         reinterpret_cast<sockaddr *>(&from), &fromLen);

        if (n == static_cast<int>(sizeof(IncomingPacket))) {
            auto *pkt = reinterpret_cast<IncomingPacket *>(buf.data());

            // --- DECODE seam: if you add Opus later, decode pkt->pcm's raw
            // bytes here into a local int16_t[FRAME_SAMPLES] buffer instead
            // of using pkt->pcm directly. ---

            std::lock_guard<std::mutex> lock(g_playersMutex);
            RemotePlayer &rp = g_players[pkt->playerId];
            rp.posX = pkt->posX;
            rp.posY = pkt->posY;
            rp.posZ = pkt->posZ;
            if (rp.username.empty())
                rp.username.assign(pkt->username, strnlen(pkt->username, sizeof(pkt->username)));
            rp.lastPacket = std::chrono::steady_clock::now();
            memcpy(rp.pcm, pkt->pcm, FRAME_BYTES);
            rp.writeCursor = 0;
            rp.hasAudio    = true;
        } else if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ===========================================================================
// Playback mixing: miniaudio pulls audio via this callback. We sum every
// remote player's current frame, each scaled by that player's distance gain
// against the local player's live position.
// ===========================================================================

static void playbackCallback(ma_device *device, void *output, const void *,
                             ma_uint32 frameCount) {
    auto *out = reinterpret_cast<float *>(output);
    memset(out, 0, sizeof(float) * frameCount);

    float lx, ly, lz;
    bool haveLocal = readLocalPos(lx, ly, lz);

    std::lock_guard<std::mutex> lock(g_playersMutex);
    for (auto &[id, rp] : g_players) {
        if (!rp.hasAudio)
            continue;

        float gain = 0.0f;
        if (haveLocal)
            gain = distanceGain(rp.posX - lx, rp.posY - ly, rp.posZ - lz);
        rp.gain = gain; // distance-only, kept for overlay display

        gain *= rp.userVolume * g_masterVolume.load();

        if (gain <= 0.001f) {
            rp.writeCursor = FRAME_SAMPLES; // skip -- fully attenuated
            continue;
        }

        for (ma_uint32 i = 0; i < frameCount && rp.writeCursor < FRAME_SAMPLES; ++i) {
            float sample = rp.pcm[rp.writeCursor++] / 32768.0f;
            out[i] += sample * gain;
        }

        if (rp.writeCursor >= FRAME_SAMPLES)
            rp.hasAudio = false; // consumed; wait for next packet
    }

    // Basic clipping guard.
    for (ma_uint32 i = 0; i < frameCount; ++i) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        if (out[i] < -1.0f) out[i] = -1.0f;
    }

    (void) device;
}

// ===========================================================================
// Mic capture: read mic frames, gate on VAD, send over UDP.
// ===========================================================================

static uint16_t g_sendSeq = 0;

static void captureCallback(ma_device *, void *, const void *input, ma_uint32 frameCount) {
    static std::vector<int16_t> pending;
    auto *in = reinterpret_cast<const int16_t *>(input);
    pending.insert(pending.end(), in, in + frameCount);

    while (pending.size() >= FRAME_SAMPLES) {
        // Cheap voice-activity gate: skip frames that are basically silence.
        bool loud = false;
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
            if (abs(pending[i]) > VAD_THRESHOLD) {
                loud = true;
                break;
            }
        }

        if (loud) {
            OutgoingPacket pkt;
            pkt.seq = g_sendSeq++;
            memcpy(pkt.pcm, pending.data(), FRAME_BYTES);

            // --- ENCODE seam: replace the raw memcpy above with an Opus
            // encode step once you add libopus, and shrink OutgoingPacket's
            // payload to the compressed size instead of FRAME_BYTES. ---

            sendto(g_sock, reinterpret_cast<const char *>(&pkt), sizeof(pkt), 0,
                  reinterpret_cast<sockaddr *>(&g_relayAddr), sizeof(g_relayAddr));
        }

        pending.erase(pending.begin(), pending.begin() + FRAME_SAMPLES);
    }
}

// ===========================================================================
// miniaudio device setup
// ===========================================================================

static ma_context g_audioContext;
static ma_device  g_captureDevice;
static ma_device  g_playbackDevice;
static bool       g_captureInitialized = false;

// Guards (re)initialization of the capture device -- the overlay calls
// SetCaptureDeviceIndex() from its own render thread, which must not race
// with startAudio()/stopAudio() or the capture callback's own device.
static std::mutex g_captureDeviceMutex;

static int                                    g_captureDeviceIndex = -1; // -1 = system default
static std::vector<ma_device_id>              g_captureDeviceIds;
static std::vector<std::string>               g_captureDeviceNames;

static void refreshCaptureDeviceList() {
    ma_device_info* pPlaybackInfos;
    ma_uint32 playbackCount;
    ma_device_info* pCaptureInfos;
    ma_uint32 captureCount;

    if (ma_context_get_devices(&g_audioContext, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount) != MA_SUCCESS)
        return;

    g_captureDeviceIds.clear();
    g_captureDeviceNames.clear();
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        g_captureDeviceIds.push_back(pCaptureInfos[i].id);
        g_captureDeviceNames.push_back(pCaptureInfos[i].name);
    }
}

// Starts (or restarts, if already running) the capture device using
// g_captureDeviceIndex. Safe to call while the plugin is already running.
static bool startCaptureDeviceLocked() {
    if (g_captureInitialized) {
        ma_device_uninit(&g_captureDevice);
        g_captureInitialized = false;
    }

    ma_device_config captureCfg = ma_device_config_init(ma_device_type_capture);
    captureCfg.capture.format   = ma_format_s16;
    captureCfg.capture.channels = CHANNELS;
    captureCfg.sampleRate       = SAMPLE_RATE;
    captureCfg.dataCallback     = captureCallback;

    if (g_captureDeviceIndex >= 0 && g_captureDeviceIndex < static_cast<int>(g_captureDeviceIds.size()))
        captureCfg.capture.pDeviceID = &g_captureDeviceIds[g_captureDeviceIndex];
    // else leave pDeviceID null -- miniaudio picks the system default.

    if (ma_device_init(&g_audioContext, &captureCfg, &g_captureDevice) != MA_SUCCESS)
        return false;

    if (ma_device_start(&g_captureDevice) != MA_SUCCESS) {
        ma_device_uninit(&g_captureDevice);
        return false;
    }

    g_captureInitialized = true;
    return true;
}

static bool startAudio() {
    if (ma_context_init(nullptr, 0, nullptr, &g_audioContext) != MA_SUCCESS)
        return false;

    refreshCaptureDeviceList();

    {
        std::lock_guard<std::mutex> lock(g_captureDeviceMutex);
        if (!startCaptureDeviceLocked()) {
            ma_context_uninit(&g_audioContext);
            return false;
        }
    }

    ma_device_config playbackCfg  = ma_device_config_init(ma_device_type_playback);
    playbackCfg.playback.format   = ma_format_f32;
    playbackCfg.playback.channels = 1;
    playbackCfg.sampleRate        = SAMPLE_RATE;
    playbackCfg.dataCallback      = playbackCallback;

    if (ma_device_init(&g_audioContext, &playbackCfg, &g_playbackDevice) != MA_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_captureDeviceMutex);
        ma_device_uninit(&g_captureDevice);
        ma_context_uninit(&g_audioContext);
        return false;
    }

    ma_device_start(&g_playbackDevice);
    return true;
}

static void stopAudio() {
    std::lock_guard<std::mutex> lock(g_captureDeviceMutex);
    if (g_captureInitialized) {
        ma_device_uninit(&g_captureDevice);
        g_captureInitialized = false;
    }
    ma_device_uninit(&g_playbackDevice);
    ma_context_uninit(&g_audioContext);
}

// ===========================================================================
// VoiceProximity API -- implementation. See VoiceProximityAPI.h.
// ===========================================================================
namespace VoiceProximity {

std::vector<CaptureDeviceInfo> GetCaptureDevices() {
    std::lock_guard<std::mutex> lock(g_captureDeviceMutex);
    std::vector<CaptureDeviceInfo> result;
    result.push_back({-1, "System Default"});
    for (size_t i = 0; i < g_captureDeviceNames.size(); ++i)
        result.push_back({static_cast<int>(i), g_captureDeviceNames[i]});
    return result;
}

int GetCaptureDeviceIndex() {
    return g_captureDeviceIndex;
}

void SetCaptureDeviceIndex(int index) {
    std::lock_guard<std::mutex> lock(g_captureDeviceMutex);
    g_captureDeviceIndex = index;
    startCaptureDeviceLocked(); // swaps the live device; logs nothing on failure by design -- overlay can retry
}

float GetMasterVolume() {
    return g_masterVolume.load();
}

void SetMasterVolume(float volume) {
    g_masterVolume.store(volume < 0.0f ? 0.0f : (volume > 2.0f ? 2.0f : volume));
}

std::vector<PlayerVolumeInfo> GetKnownPlayers() {
    std::vector<PlayerVolumeInfo> result;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(g_playersMutex);
    for (auto& [id, rp] : g_players) {
        PlayerVolumeInfo info;
        info.playerId = id;
        info.username = rp.username.empty() ? ("Player " + std::to_string(id)) : rp.username;
        info.volume   = rp.userVolume;
        info.talking  = (now - rp.lastPacket) < std::chrono::milliseconds(500);
        result.push_back(std::move(info));
    }
    return result;
}

void SetPlayerVolume(uint32_t playerId, float volume) {
    volume = volume < 0.0f ? 0.0f : (volume > 2.0f ? 2.0f : volume);

    std::lock_guard<std::mutex> lock(g_playersMutex);
    auto it = g_players.find(playerId);
    if (it != g_players.end())
        it->second.userVolume = volume;
}

} // namespace VoiceProximity

// ===========================================================================
// SKSE plugin entry points (CommonLibSSE-NG style)
// ===========================================================================

static std::thread g_networkThread;
static HINSTANCE   g_hModule = nullptr;

// Spawns VoiceCompanion.exe from the same folder as this DLL. Not fatal if
// it's missing -- the audio pipeline works fine without it, you just don't
// get the settings window. Doesn't relaunch it if it's already running
// (e.g. the player started it manually) -- CreateProcess just adds a second
// window in that case, which is harmless but pointless, so a FindWindow
// check would be a nice future improvement; skipped here for simplicity.
static void launchCompanionApp() {
    char dllPath[MAX_PATH];
    if (!GetModuleFileNameA(g_hModule, dllPath, MAX_PATH))
        return;

    std::string dir(dllPath);
    size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos)
        return;
    dir = dir.substr(0, slash);

    std::string exePath = dir + "\\VoiceCompanion\\VoiceCompanion.exe";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // lpCommandLine must be a mutable buffer, not exePath.c_str() directly.
    std::vector<char> cmdLine(exePath.begin(), exePath.end());
    cmdLine.push_back('\0');

    if (CreateProcessA(exePath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    // Silently do nothing if VoiceCompanion.exe isn't present -- see comment above.
}

static void onSKSEMessage(SKSE::MessagingInterface::Message *msg) {
    if (msg->type != SKSE::MessagingInterface::kDataLoaded)
        return;

    if (!initNetworking())
        return;

    g_running = true;
    g_networkThread = std::thread(networkThreadFn);

    startAudio();

    IpcServer::Start(); // loopback TCP server for the companion app -- see IpcServer.cpp
    launchCompanionApp();
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface *skse) {
    SKSE::Init(skse);

    auto *messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(onSKSEMessage))
        return false;

    return true;
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;
    v.PluginVersion(REL::Version(1, 0, 0, 0));
    v.PluginName("SkyrimVoiceProximity");
    v.AuthorName("KingWaso");
    v.UsesAddressLibrary(true);
    v.CompatibleVersions({ SKSE::RUNTIME_LATEST });
    return v;
}();

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        if (g_networkThread.joinable())
            g_networkThread.join();
        stopAudio();
        shutdownNetworking();
        IpcServer::Stop();
    }
    return TRUE;
}
