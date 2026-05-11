#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "protocol.hpp"

#include <stdexcept>
#include <string>

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
        }
    }

    ~WinsockRuntime() {
        WSACleanup();
    }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

inline std::string wsa_error_message(const char* prefix) {
    return std::string(prefix) + " failed with WSA error " +
           std::to_string(WSAGetLastError());
}

inline void socket_send_all(SOCKET socket, const void* data, std::size_t size) {
    const auto* cursor = static_cast<const char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const int chunk = remaining > 64 * 1024
                              ? 64 * 1024
                              : static_cast<int>(remaining);
        const int sent = send(socket, cursor, chunk, 0);
        if (sent == SOCKET_ERROR) {
            throw std::runtime_error(wsa_error_message("send"));
        }
        if (sent == 0) {
            throw std::runtime_error("send wrote zero bytes");
        }
        cursor += sent;
        remaining -= sent;
    }
}

inline void socket_recv_all(SOCKET socket, void* data, std::size_t size) {
    auto* cursor = static_cast<char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const int chunk = remaining > 64 * 1024
                              ? 64 * 1024
                              : static_cast<int>(remaining);
        const int received = recv(socket, cursor, chunk, 0);
        if (received == SOCKET_ERROR) {
            throw std::runtime_error(wsa_error_message("recv"));
        }
        if (received == 0) {
            throw std::runtime_error("recv reached EOF");
        }
        cursor += received;
        remaining -= received;
    }
}

inline SOCKET connect_to_process_c() {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        throw std::runtime_error(wsa_error_message("socket"));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kProcessCPort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (connect(socket, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
        const std::string error = wsa_error_message("connect");
        closesocket(socket);
        throw std::runtime_error(error);
    }

    return socket;
}

inline void send_tcp_message(SOCKET socket, PipeCommand command,
                             const std::string& path,
                             const void* payload,
                             std::uint32_t payload_bytes) {
    PipeMessageHeader header{kProtocolMagic,
                             static_cast<std::uint32_t>(command),
                             static_cast<std::uint32_t>(path.size()),
                             payload_bytes};
    socket_send_all(socket, &header, sizeof(header));
    if (!path.empty()) {
        socket_send_all(socket, path.data(), path.size());
    }
    if (payload != nullptr && payload_bytes > 0) {
        socket_send_all(socket, payload, payload_bytes);
    }

    PipeResponse response{};
    socket_recv_all(socket, &response, sizeof(response));
    if (response.magic != kProtocolMagic) {
        throw std::runtime_error("invalid TCP response magic");
    }
    if (!response.ok) {
        throw std::runtime_error("process_c returned win32 error " +
                                 std::to_string(response.win32Error));
    }
}

