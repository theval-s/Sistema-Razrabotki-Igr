#pragma once

// Contract between the launcher (sri_runtime.exe) and the game library it loads.
namespace runtime_entry {

inline constexpr wchar_t LibraryDirectory[] = L"bin";
inline constexpr wchar_t GameLibrary[] = L"sri_game.dll";
inline constexpr char RunGameExport[] = "SriRunGame";
using RunGameFunction = int (*)();

} // namespace runtime_entry
