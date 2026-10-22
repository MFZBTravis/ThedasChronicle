#pragma once
#include <windows.h>

namespace logging {

// Opens ThedasChronicleHook.log next to the .asi, falling back to
// %LOCALAPPDATA%\ThedasChronicleHook\ if the game folder isn't writable.
void Init(HMODULE self);
void Shutdown();

// printf-style; thread-safe, timestamped, flushed every line.
// Also mirrored to OutputDebugString (view with Sysinternals DebugView).
void Write(const char* fmt, ...);

// Hex + ASCII dump, capped at maxBytes.
void Hex(const void* data, size_t len, size_t maxBytes = 256);

} // namespace logging

#define LOG(...) ::logging::Write(__VA_ARGS__)
