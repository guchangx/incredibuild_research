#pragma once

#include <cstdint>

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\write_redirect_demo_pipe";
constexpr wchar_t kRedirectTargetEnv[] = L"WRD_REDIRECT_TARGET";
constexpr unsigned short kProcessCPort = 39017;
constexpr std::uint32_t kProtocolMagic = 0x57445244; // "WDRD"

enum class PipeCommand : std::uint32_t {
    Open = 1,
    Write = 2,
    Close = 3,
};

struct PipeMessageHeader {
    std::uint32_t magic;
    std::uint32_t command;
    std::uint32_t pathBytes;
    std::uint32_t payloadBytes;
};

struct PipeResponse {
    std::uint32_t magic;
    std::uint32_t ok;
    std::uint32_t win32Error;
};
