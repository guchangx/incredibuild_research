#include "common.hpp"
#include "tcp_common.hpp"

namespace {

// Handles one named-pipe client and writes redirected data locally.
// 处理一个命名管道客户端，并在本地写入重定向数据。
void handle_client_local(HANDLE pipe) {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring current_path;

    for (;;) {
        PipeMessageHeader header{};
        read_all(pipe, &header, sizeof(header));

        if (header.magic != kProtocolMagic) {
            throw std::runtime_error("invalid protocol magic");
        }
        if (header.pathBytes > 32 * 1024) {
            throw std::runtime_error("invalid path length");
        }
        if (header.payloadBytes > 16 * 1024 * 1024) {
            throw std::runtime_error("payload too large for demo");
        }

        std::string path_utf8(header.pathBytes, '\0');
        std::string payload(header.payloadBytes, '\0');
        if (!path_utf8.empty()) {
            read_all(pipe, path_utf8.data(), path_utf8.size());
        }
        if (!payload.empty()) {
            read_all(pipe, payload.data(), payload.size());
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
                    std::wcout << L"process_b: opened redirected target "
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
                    std::wcout << L"process_b: wrote " << payload.size()
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
                    std::wcout << L"process_b: closed redirected target "
                               << current_path << L"\n";
                }
                file = INVALID_HANDLE_VALUE;
            }
            write_all(pipe, &response, sizeof(response));
            break;
        } else {
            response.ok = 0;
            response.win32Error = ERROR_INVALID_PARAMETER;
        }

        write_all(pipe, &response, sizeof(response));
        if (!response.ok) {
            break;
        }
    }

    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

// Handles one named-pipe client by forwarding redirected data to process_c.
// 通过将重定向数据转发给 process_c 来处理一个命名管道客户端。
void handle_client_forward_to_c(HANDLE pipe) {
    WinsockRuntime winsock;
    SOCKET process_c = INVALID_SOCKET;
    bool tcp_open = false;

    try {
        process_c = connect_to_process_c();
        std::cout << "process_b: connected to process_c on 127.0.0.1:"
                  << kProcessCPort << "\n";

        for (;;) {
            PipeMessageHeader header{};
            read_all(pipe, &header, sizeof(header));

            if (header.magic != kProtocolMagic) {
                throw std::runtime_error("invalid protocol magic");
            }
            if (header.pathBytes > 32 * 1024) {
                throw std::runtime_error("invalid path length");
            }
            if (header.payloadBytes > 16 * 1024 * 1024) {
                throw std::runtime_error("payload too large for demo");
            }

            std::string path_utf8(header.pathBytes, '\0');
            std::string payload(header.payloadBytes, '\0');
            if (!path_utf8.empty()) {
                read_all(pipe, path_utf8.data(), path_utf8.size());
            }
            if (!payload.empty()) {
                read_all(pipe, payload.data(), payload.size());
            }

            PipeResponse response{kProtocolMagic, 1, ERROR_SUCCESS};
            try {
                const auto command = static_cast<PipeCommand>(header.command);
                if (command == PipeCommand::Open) {
                    send_tcp_message(process_c, command, path_utf8, nullptr, 0);
                    tcp_open = true;
                    std::cout << "process_b: forwarded open to process_c\n";
                } else if (command == PipeCommand::Write) {
                    send_tcp_message(process_c, command, {}, payload.data(),
                                     static_cast<std::uint32_t>(payload.size()));
                    std::cout << "process_b: forwarded " << payload.size()
                              << " bytes to process_c\n";
                } else if (command == PipeCommand::Close) {
                    send_tcp_message(process_c, command, {}, nullptr, 0);
                    tcp_open = false;
                    std::cout << "process_b: forwarded close to process_c\n";
                    write_all(pipe, &response, sizeof(response));
                    break;
                } else {
                    response.ok = 0;
                    response.win32Error = ERROR_INVALID_PARAMETER;
                }
            } catch (const std::exception&) {
                response.ok = 0;
                response.win32Error = ERROR_WRITE_FAULT;
            }

            write_all(pipe, &response, sizeof(response));
            if (!response.ok) {
                break;
            }
        }
    } catch (...) {
        if (process_c != INVALID_SOCKET) {
            if (tcp_open) {
                try {
                    send_tcp_message(process_c, PipeCommand::Close, {}, nullptr,
                                     0);
                } catch (...) {
                }
            }
            closesocket(process_c);
        }
        throw;
    }

    closesocket(process_c);
}

// Prints command-line usage for process_b.
// 打印 process_b 的命令行用法。
void usage() {
    std::cout
        << "Usage:\n"
        << "  process_b.exe [--once] [--forward-to-c]\n\n"
        << "Default: writes redirected files locally.\n"
        << "--forward-to-c: forwards redirected file streams to process_c over TCP.\n";
}

} // namespace

// Runs the named-pipe redirect server and optionally forwards streams to process_c.
// 运行命名管道重定向服务器，并可选择将数据流转发给 process_c。
int wmain(int argc, wchar_t** argv) {
    try {
        bool once = false;
        bool forward_to_c = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--once") {
                once = true;
            } else if (arg == L"--forward-to-c") {
                forward_to_c = true;
            } else {
                usage();
                return 2;
            }
        }

        std::wcout << L"process_b: listening on " << kPipeName << L"\n";
        do {
            HANDLE pipe = CreateNamedPipeW(
                kPipeName, PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                throw std::runtime_error(last_error_message("CreateNamedPipeW"));
            }

            const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                       ? TRUE
                                       : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) {
                const std::string error = last_error_message("ConnectNamedPipe");
                CloseHandle(pipe);
                throw std::runtime_error(error);
            }

            try {
                if (forward_to_c) {
                    handle_client_forward_to_c(pipe);
                } else {
                    handle_client_local(pipe);
                }
            } catch (...) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                throw;
            }

            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        } while (!once);

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "process_b error: " << ex.what() << "\n";
        return 1;
    }
}
