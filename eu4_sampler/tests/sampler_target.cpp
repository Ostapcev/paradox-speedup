#include <windows.h>

#include <cstdint>

volatile std::uint64_t target_sink = 0;

__attribute__((noinline)) void busy_leaf(std::uint64_t rounds) {
    auto value = target_sink + 1;
    for (std::uint64_t index = 0; index < rounds; ++index) {
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
        value ^= value >> 29;
    }
    target_sink = value;
}

__attribute__((noinline)) void busy_middle() {
    busy_leaf(3000000);
}

__attribute__((noinline)) void busy_outer() {
    busy_middle();
}

DWORD WINAPI sleeping_thread(void*) {
    while (true) Sleep(1000);
    return 0;
}

int main() {
    CreateThread(nullptr, 0, sleeping_thread, nullptr, 0, nullptr);
    const auto finish = GetTickCount64() + 60000;
    while (GetTickCount64() < finish) busy_outer();
    return static_cast<int>(target_sink & 1);
}
