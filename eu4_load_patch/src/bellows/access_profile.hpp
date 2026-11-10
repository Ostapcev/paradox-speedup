// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/access_profile.hpp; see ../../LICENSE.bellows.txt.
#pragma once

// Access profiles: lists of files the game opens during start-up, in first-use
// order (ported from the lab's startup_open_warmup.inc list helpers).
//
// Format: UTF-8, one path per line. Paths inside the game folder are stored as
// "@\relative"; everything else (Workshop mods, the user folder) is absolute,
// so each entry's source is visible from the path itself (game, mod id from
// "...\workshop\content\236850\<id>\...", user folder).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sb::profile {

std::wstring Lower(std::wstring text);
std::wstring Normalize(std::wstring path);
std::wstring ToStored(const std::wstring& path, const std::wstring& gameDir);
std::wstring FromStored(const std::wstring& line, const std::wstring& gameDir);
std::uint64_t Fnv1a(const std::string& bytes);
std::wstring Widen(const std::string& text);
std::string Narrow(const std::wstring& text);
bool ReadWhole(const std::wstring& path, std::string& out, std::uint64_t limit = 64ULL << 20);
std::vector<std::wstring> ParseList(const std::string& text);
std::vector<std::string> ParseEnabledMods(const std::string& json);
std::string DescriptorValue(const std::string& text, const std::string& key);
bool IsAbsolute(const std::wstring& path);
int Rank(const std::wstring& path);
bool AllListWanted(const std::wstring& relativeLower);

// "all" list: game folder (unless skipGame) + enabled mods' folders/archives,
// definitions everywhere, media only outside folders loaded during play;
// ordered archives/descriptors, definitions, other files.
std::vector<std::wstring> BuildAllList(const std::wstring& gameDir, const std::wstring& userDir,
                                       const std::vector<std::string>& enabledMods, bool skipGame);

// Profile key for a mod set: "<build>-<16 hex digits of FNV-1a(dlc_load.json)>".
std::wstring ProfileKey(const char* buildId, const std::string& dlcLoadJson);

// Folders of the enabled mods (descriptor "path", or the folder holding "archive").
std::vector<std::wstring> ModFolders(const std::wstring& userDir, const std::vector<std::string>& enabledMods);

// True when Windows answers that the file or a folder on its path does not exist. Any other failure
// (no access, a name it refuses) says nothing about the file.
bool FileGone(const std::wstring& path);

// Unique stored paths in first-use order, then previous entries not seen this time whose file is
// not gone (max 60000): a file this start did not open may be one the next start opens.
std::vector<std::wstring> UniqueStored(const std::vector<std::wstring>& paths, const std::wstring& gameDir,
                                       const std::vector<std::wstring>& previous = {});

// Writes via a temporary file and an atomic replace.
bool WriteList(const std::wstring& target, const std::vector<std::wstring>& lines);

// The name of the game's folder in Documents\Paradox Interactive for the game
// in `gameDir`: "Europa Universalis IV", or "Europa Universalis IV GOG" in a GOG
// install, whose eu4.exe adds " GOG" (GOG leaves goggame-2057001589.info there).
std::wstring UserFolderName(const std::wstring& gameDir);

// The folder `-userdir=<path>` names on eu4.exe's command line, empty without
// one. The whole argument may be in quotes, as Node's spawn passes a path with
// a space, or only the path, as a player writes it in Steam's launch options.
// Without quotes it ends where the game ends an argument, at a space
// before -, + or /, or at a bare ` continue=`. The same reading as Anvil's
// user_dir_argument (anvil#207), which no Anvil exposes to plugins yet.
std::wstring UserDirArgument(std::wstring_view commandLine);

// The game's user folder as the game chooses it: the folder of -userdir when it
// exists, then the path in <gameDir>\userdir.txt, then UserFolderName in
// `documents` (`documents` empty: empty). With backslashes, no slash at the end.
std::wstring UserDirectoryFor(const std::wstring& commandLine, const std::wstring& gameDir, const std::wstring& documents);

// That folder for the running game (empty when unknown).
std::wstring UserDirectory();

// Threads that read or open ahead of the game. A rotating disk has one head: more threads only make
// it seek, and they take it from the game (not measured, the lab has no rotating disk).
inline constexpr unsigned kWorkers = 8;
inline constexpr unsigned kWorkersRotating = 2;

// False too when the disk cannot be asked (a network drive, a folder mounted without a letter).
bool OnRotatingDisk(const std::wstring& path);
unsigned WorkersFor(const std::wstring& gameDir);

}  // namespace sb::profile
