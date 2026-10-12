#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lsm::net {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;

class Runtime {
public:
    Runtime() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Runtime() { if (ready_) WSACleanup(); }
    [[nodiscard]] bool ready() const { return ready_; }
private:
    bool ready_ = false;
};

inline void Close(Socket socket) { if (socket != kInvalidSocket) closesocket(socket); }
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;

class Runtime {
public:
    [[nodiscard]] bool ready() const { return true; }
};

inline void Close(Socket socket) { if (socket != kInvalidSocket) close(socket); }
#endif

inline bool SendAll(Socket socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto remaining = data.size() - sent;
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 64 * 1024));
#ifdef _WIN32
        const int count = send(socket, data.data() + sent, chunk, 0);
#else
        const int count = static_cast<int>(send(socket, data.data() + sent,
                                                static_cast<std::size_t>(chunk), 0));
#endif
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

inline bool ReceiveLine(Socket socket, std::string* line) {
    if (line == nullptr) return false;
    line->clear();
    char byte = 0;
    while (line->size() < 1024 * 1024) {
#ifdef _WIN32
        const int count = recv(socket, &byte, 1, 0);
#else
        const int count = static_cast<int>(recv(socket, &byte, 1, 0));
#endif
        if (count <= 0) return false;
        if (byte == '\n') return true;
        if (byte != '\r') line->push_back(byte);
    }
    return false;
}

} // namespace lsm::net
