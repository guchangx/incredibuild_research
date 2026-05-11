#include "common.hpp"
#include "tcp_common.hpp"

namespace {

void handle_client(SOCKET client) {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring current_path;

    for (;;) {
        PipeMessageHeader header{};
        socket_recv_all(client, &header, sizeof(header));

        if (header.magic != kProtocolMagic) {
            throw std::runtime_error("invalid TCP protocol magic");
        }
        if (header.pathBytes > 32 * 1024) {
            throw std::runtime_error("invalid TCP path length");
        }
        if (header.payloadBytes > 16 * 1024 * 1024) {
            throw std::runtime_error("TCP payload too large for demo");
        }

        std::string path_utf8(header.pathBytes, '\0');
        std::string payload(header.payloadBytes, '\0');
        if (!path_utf8.empty()) {
            socket_recv_all(client, path_utf8.data(), path_utf8.size());
        }
        if (!payload.empty()) {
            socket_recv_all(client, payload.data(), payload.size());
        }

        PipeResponse response{kProtocolMagic, 1, ERROR_SUCCESS};
        const auto command = static_cast<PipeCommand>(header.command);

        if (command == PipeCommand::Open) {
            if (file != INVALID_HANDLE_VALUE) {
                response.ok = 0;
                response.win32Error = ERROR_ALREADY_EXISTS;
            } else if (path_utf8.empty()) {
                response.ok = 0;
                response.win32Error = ERROR_INVALID_PARAMETER;
            } else {
                current_path = widen(path_utf8);
                ensure_parent_directory(current_path);
                file = CreateFileW(current_path.c_str(), GENERIC_WRITE,
                                   FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    response.ok = 0;
                    response.win32Error = GetLastError();
                } else {
                    std::wcout << L"process_c: opened TCP target "
                               << current_path << L"\n";
                }
            }
        } else if (command == PipeCommand::Write) {
            if (file == INVALID_HANDLE_VALUE) {
                response.ok = 0;
                response.win32Error = ERROR_INVALID_HANDLE;
            } else {
                try {
                    write_all(file, payload.data(), payload.size());
                    std::wcout << L"process_c: wrote " << payload.size()
                               << L" bytes\n";
                } catch (...) {
                    response.ok = 0;
                    response.win32Error = GetLastError();
                }
            }
        } else if (command == PipeCommand::Close) {
            if (file != INVALID_HANDLE_VALUE) {
                if (!CloseHandle(file)) {
                    response.ok = 0;
                    response.win32Error = GetLastError();
                } else {
                    std::wcout << L"process_c: closed TCP target "
                               << current_path << L"\n";
                }
                file = INVALID_HANDLE_VALUE;
            }
            socket_send_all(client, &response, sizeof(response));
            break;
        } else {
            response.ok = 0;
            response.win32Error = ERROR_INVALID_PARAMETER;
        }

        socket_send_all(client, &response, sizeof(response));
        if (!response.ok) {
            break;
        }
    }

    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

void usage() {
    std::cout
        << "Usage:\n"
        << "  process_c.exe [--once]\n\n"
        << "TCP server on 127.0.0.1:" << kProcessCPort
        << ". Receives file streams from process_b and writes target files.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        bool once = false;
        if (argc >= 2) {
            const std::wstring arg = argv[1];
            if (arg == L"--once") {
                once = true;
            } else {
                usage();
                return 2;
            }
        }

        WinsockRuntime winsock;

        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            throw std::runtime_error(wsa_error_message("socket"));
        }

        BOOL reuse = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(kProcessCPort);

        if (bind(listener, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR) {
            const std::string error = wsa_error_message("bind");
            closesocket(listener);
            throw std::runtime_error(error);
        }

        if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
            const std::string error = wsa_error_message("listen");
            closesocket(listener);
            throw std::runtime_error(error);
        }

        std::cout << "process_c: listening on 127.0.0.1:" << kProcessCPort
                  << "\n";
        do {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                const std::string error = wsa_error_message("accept");
                closesocket(listener);
                throw std::runtime_error(error);
            }

            try {
                handle_client(client);
            } catch (...) {
                closesocket(client);
                throw;
            }
            closesocket(client);
        } while (!once);

        closesocket(listener);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "process_c error: " << ex.what() << "\n";
        return 1;
    }
}

