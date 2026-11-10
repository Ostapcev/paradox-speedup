// From EU4 Menu Patch (https://github.com/YozoraTempest/EU4-Menu-Patch),
// MIT License, Copyright (c) 2026 VulonLok. See LICENSE.
#pragma once

#include <cstdint>

namespace hamweard {

// The world's PotentialCorruption after a return. The game sets it to 1 when
// the player enters single player and keeps it through ResetGame: it is what
// asks for a restart before multiplayer, so 1 stays. ResetGame turns every other
// non-zero reason into 1, and only -nudge's own reason (2) needs putting back.
constexpr std::uint32_t corruption_after_return(std::uint32_t after_reset, std::uint32_t before_return) noexcept {
    return after_reset == 1 && before_return > 1 ? before_return : after_reset;
}

// Stock, a process that has played never reaches the menu: every way back restarts.
// The game's own check does not stop a click either, because the update that
// disables the Multiplayer button then enables every menu button. So after a
// return the click stops here, and the button's tooltip gives the game's reason.
constexpr bool multiplayer_needs_restart(bool returned, std::uint32_t corruption) noexcept {
    return returned && corruption != 0;
}

// GetMultiplayerAvailability's "restart first", the only answer that shows the reason.
constexpr int multiplayer_must_restart = 5;

// The game answers that only with a Paradox session, so without one (GOG without
// an account, lab) the click stopped with no word. Another reason the game
// already gives stays: only "available" turns into "restart first".
constexpr int multiplayer_availability(int stock, bool returned, std::uint32_t corruption) noexcept {
    return stock == 0 && multiplayer_needs_restart(returned, corruption) ? multiplayer_must_restart : stock;
}

// A world reset does not give all its memory back, and a big mod loses tens of MB
// each time (lab: 66 MB with Anbennar). So a return with less than this left of
// the system's commit limit restarts the game: the memory comes back, and a
// reset cannot fail halfway.
constexpr std::uint64_t min_commit_for_return = 1ull << 30;

constexpr bool enough_memory_for_return(std::uint64_t commit_available) noexcept {
    return commit_available >= min_commit_for_return;
}

// Why a return has to be the game's own restart, or null when it does not.
constexpr const char* restart_reason(bool multiplayer, bool map_randomized,
                                     std::uint64_t commit_available) noexcept {
    if (multiplayer) return "in a multiplayer session";
    if (map_randomized) return "after a Random New World map";
    if (!enough_memory_for_return(commit_available)) return "with too little memory left";
    return nullptr;
}

// SetNextIdler hands the new idler over on a later frame. The world is reset
// only after that hand-over has destroyed the outgoing idler: once the menu is
// both the updating and the current idler and nothing is queued after it.
class WorldResetGate {
    void* app_ = nullptr;
    void* menu_ = nullptr;

public:
    void queue(void* app, void* menu) noexcept {
        app_ = app;
        menu_ = menu;
    }
    bool pending() const noexcept { return app_ != nullptr; }

    // Runs reset() once, when the hand-over is complete; disarms itself before
    // the call, so a reset that pumps messages or throws never runs twice.
    template <class Reset> bool run(void* app, void* updating, void* current, void* next, Reset&& reset) {
        if (!app_ || app != app_ || updating != menu_ || current != menu_ || next) return false;
        app_ = nullptr;
        menu_ = nullptr;
        reset();
        return true;
    }
};

}  // namespace hamweard
