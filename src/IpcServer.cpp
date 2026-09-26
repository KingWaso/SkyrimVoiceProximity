// ---------------------------------------------------------------------------
// IpcServer.cpp
//
// Loopback-only TCP server (127.0.0.1, not exposed externally) that lets
// the external VoiceCompanion.exe window query and control the audio
// pipeline running inside SkyrimVoiceProximity.dll, without needing to
// inject anything into the game's process. Deliberately dead simple:
// newline-delimited plain text, one request per line, one response per
// request. No auth beyond "only accepts loopback connections" -- fine for
// a same-machine companion app; don't repurpose this for anything that
// needs to be reachable from other machines.
//
// Protocol (all lines end in \n; multi-line responses end with "END\n"):
//
//   LIST_MICS                -> "<index>|<name>\n" ... "END\n"
//                                index -1 is always "System Default"
//   GET_MIC                  -> "<index>\n"
//   SET_MIC <index>           -> "OK\n"
//   GET_MASTER                -> "<float 0.00-2.00>\n"
//   SET_MASTER <float>         -> "OK\n"
//   LIST_PLAYERS              -> "<id>|<username>|<volume>|<talking 0/1>\n" ... "END\n"
//   SET_PLAYER_VOLUME <id> <float> -> "OK\n" (or "ERR unknown player\n")
//
// Unrecognized commands get "ERR unknown command\n".
// ---------------------------------------------------------------------------

#include "IpcServer.h"
#include "VoiceProximityAPI.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr uint16_t kIpcPort = 28571; // distinct from RELAY_PORT (28570)

SOCKET            g_listenSock = INVALID_SOCKET;
std::atomic<bool> g_running{false};
std::thread       g_acceptThread;

void sendLine(SOCKET s, const std::string& line) {
    send(s, line.c_str(), static_cast<int>(line.size()), 0);
}

void handleCommand(SOCKET client, const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "LIST_MICS") {
        for (auto& d : VoiceProximity::GetCaptureDevices()) {
            char buf[300];
            snprintf(buf, sizeof(buf), "%d|%s\n", d.index, d.name.c_str());
            sendLine(client, buf);
        }
        sendLine(client, "END\n");

    } else if (cmd == "GET_MIC") {
        sendLine(client, std::to_string(VoiceProximity::GetCaptureDeviceIndex()) + "\n");

    } else if (cmd == "SET_MIC") {
        int index;
        if (iss >> index) {
            VoiceProximity::SetCaptureDeviceIndex(index);
            sendLine(client, "OK\n");
        } else {
            sendLine(client, "ERR bad index\n");
        }

    } else if (cmd == "GET_MASTER") {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f\n", VoiceProximity::GetMasterVolume());
        sendLine(client, buf);

    } else if (cmd == "SET_MASTER") {
        float volume;
        if (iss >> volume) {
            VoiceProximity::SetMasterVolume(volume);
            sendLine(client, "OK\n");
        } else {
            sendLine(client, "ERR bad volume\n");
        }

    } else if (cmd == "LIST_PLAYERS") {
        for (auto& p : VoiceProximity::GetKnownPlayers()) {
            char buf[300];
            snprintf(buf, sizeof(buf), "%u|%s|%.2f|%d\n", p.playerId, p.username.c_str(),
                    p.volume, p.talking ? 1 : 0);
            sendLine(client, buf);
        }
        sendLine(client, "END\n");

    } else if (cmd == "SET_PLAYER_VOLUME") {
        uint32_t id;
        float volume;
        if (iss >> id >> volume) {
            VoiceProximity::SetPlayerVolume(id, volume);
            sendLine(client, "OK\n");
        } else {
            sendLine(client, "ERR bad args\n");
        }

    } else {
        sendLine(client, "ERR unknown command\n");
    }
}

// One client at a time is all this needs (a single companion window). If
// you ever want multiple simultaneous IPC clients, swap this for a thread-
// per-connection accept loop -- deliberately not done here to keep this
// file small.
void serverLoop() {
    while (g_running.load()) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET client = accept(g_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (client == INVALID_SOCKET) {
            if (!g_running.load())
                break;
            continue;
        }

        // Read line-by-line until the client disconnects.
        std::string buffer;
        char chunk[512];
        while (g_running.load()) {
            int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0)
                break;

            buffer.append(chunk, n);
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    handleCommand(client, line);
            }
        }

        closesocket(client);
    }
}

} // namespace

namespace IpcServer {

void Start() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); // loopback only, deliberately
    addr.sin_port = htons(kIpcPort);

    if (bind(g_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
        return;
    }

    listen(g_listenSock, 1);

    g_running = true;
    g_acceptThread = std::thread(serverLoop);
}

void Stop() {
    g_running = false;
    if (g_listenSock != INVALID_SOCKET) {
        closesocket(g_listenSock); // unblocks the pending accept()
        g_listenSock = INVALID_SOCKET;
    }
    if (g_acceptThread.joinable())
        g_acceptThread.join();
    WSACleanup();
}

} // namespace IpcServer
