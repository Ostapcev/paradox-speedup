#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace modifierpatch {

inline constexpr std::size_t maximum_entry_size = 16;
using EntryBytes = std::array<std::uint8_t, maximum_entry_size>;

enum class CallerArgument {
    third,   // R8: suitable when the original function has at most two arguments.
    fourth,  // R9: preserves the original third argument in R8.
};

struct FunctionPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* allocation = nullptr;
    void* trampoline = nullptr;
    EntryBytes original{};
    EntryBytes replacement{};
    std::size_t entry_size = 0;
    bool committed = false;
};

bool prepare_function(std::uint8_t* site, const EntryBytes& expected, std::size_t entry_size,
    const void* hook, FunctionPatch& patch, char* error, std::size_t error_size,
    CallerArgument caller_argument = CallerArgument::third) noexcept;
bool commit_function(FunctionPatch& patch, char* error, std::size_t error_size) noexcept;
void rollback_function(FunctionPatch& patch) noexcept;
void discard_function(FunctionPatch& patch) noexcept;

inline constexpr std::size_t maximum_search_size = 20;
using SearchBytes = std::array<std::uint8_t, maximum_search_size>;

struct SearchPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* allocation = nullptr;
    SearchBytes original{};
    SearchBytes replacement{};
    std::size_t entry_size = 0;
    bool committed = false;
};

bool prepare_search(std::uint8_t* site, const SearchBytes& expected, std::size_t entry_size,
    const void* relay, SearchPatch& patch, char* error, std::size_t error_size) noexcept;
bool commit_search(SearchPatch& patch, char* error, std::size_t error_size) noexcept;
void rollback_search(SearchPatch& patch) noexcept;
void discard_search(SearchPatch& patch) noexcept;

}  // namespace modifierpatch
