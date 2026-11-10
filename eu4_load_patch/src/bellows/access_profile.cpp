// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/access_profile.cpp; see ../../LICENSE.bellows.txt.
#include "access_profile.hpp"

#include <windows.h>
#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <unordered_set>

namespace sb::profile {

std::wstring Lower(std::wstring text) {
    for (auto& c : text) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
    return text;
}

std::wstring Normalize(std::wstring path) {
    for (auto& c : path) {
        if (c == L'/') c = L'\\';
    }
    return path;
}

std::wstring ToStored(const std::wstring& path, const std::wstring& gameDir) {
    const std::wstring normal = Normalize(path);
    const std::wstring prefix = Lower(gameDir) + L"\\";
    if (normal.size() > prefix.size() && Lower(normal.substr(0, prefix.size())) == prefix) {
        return L"@\\" + normal.substr(prefix.size());
    }
    return normal;
}

std::wstring FromStored(const std::wstring& line, const std::wstring& gameDir) {
    if (line.size() > 2 && line[0] == L'@' && line[1] == L'\\') return gameDir + line.substr(1);
    return line;
}

std::uint64_t Fnv1a(const std::string& bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : bytes) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide <= 0) return {};
    std::wstring out(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), wide);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

bool ReadWhole(const std::wstring& path, std::string& out, std::uint64_t limit) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= limit;
    if (ok) {
        out.assign(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(file);
    return ok;
}

std::vector<std::wstring> ParseList(const std::string& text) {
    std::vector<std::wstring> lines;
    std::size_t position = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (position < text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string::npos) end = text.size();
        std::size_t stop = end;
        if (stop > position && text[stop - 1] == '\r') --stop;
        if (stop > position) lines.push_back(Widen(text.substr(position, stop - position)));
        position = end + 1;
    }
    return lines;
}

std::vector<std::string> ParseEnabledMods(const std::string& json) {
    std::vector<std::string> mods;
    const std::size_t key = json.find("\"enabled_mods\"");
    if (key == std::string::npos) return mods;
    const std::size_t open = json.find('[', key);
    const std::size_t close = open == std::string::npos ? std::string::npos : json.find(']', open);
    if (close == std::string::npos) return mods;
    std::size_t position = open + 1;
    while (position < close) {
        const std::size_t a = json.find('"', position);
        if (a == std::string::npos || a >= close) break;
        const std::size_t b = json.find('"', a + 1);
        if (b == std::string::npos || b > close) break;
        mods.push_back(json.substr(a + 1, b - a - 1));
        position = b + 1;
    }
    return mods;
}

std::string DescriptorValue(const std::string& text, const std::string& key) {
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string::npos) end = text.size();
        std::size_t start = position;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
        if (text.compare(start, key.size(), key) == 0) {
            std::size_t cursor = start + key.size();
            while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
            if (cursor < end && text[cursor] == '=') {
                const std::size_t a = text.find('"', cursor);
                const std::size_t b = a == std::string::npos ? std::string::npos : text.find('"', a + 1);
                if (b != std::string::npos && b <= end) return text.substr(a + 1, b - a - 1);
            }
        }
        position = end + 1;
    }
    return {};
}

bool IsAbsolute(const std::wstring& path) {
    return (path.size() > 2 && path[1] == L':') || (path.size() > 1 && path[0] == L'\\' && path[1] == L'\\');
}

int Rank(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    const std::size_t slash = path.find_last_of(L'\\');
    const std::wstring ext =
        (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) ? L"" : Lower(path.substr(dot));
    if (ext == L".zip" || ext == L".dlc" || ext == L".mod") return 0;
    if (ext == L".txt" || ext == L".yml" || ext == L".gui" || ext == L".gfx" || ext == L".csv" || ext == L".lua" ||
        ext == L".asset" || ext == L".json") {
        return 1;
    }
    if (ext == L".ogg" || ext == L".wav" || ext == L".mp3" || ext == L".bik" || ext == L".bank" || ext == L".fsb") return 3;
    return 2;
}

bool AllListWanted(const std::wstring& relativeLower) {
    if (Rank(relativeLower) <= 1) return true;  // archives, descriptors, definitions
    static const wchar_t* const kLazy[] = {
        L"sound\\", L"music\\", L"soundtrack\\", L"gfx\\interface\\", L"gfx\\event_pictures\\",
        L"gfx\\dlc_pictures\\", L"gfx\\cursors\\", L"gfx\\loadingscreens\\", L"tutorial\\", L"tests\\",
        L"ebook\\", L"tools\\", L"legal_notes\\", L"licenses\\", L"patchnotes\\", L"crash_reporter\\",
        L"emptysteamdepot\\", L"tweakergui_assets\\", L"thumbnail", L"anvil\\",
    };
    for (const wchar_t* prefix : kLazy) {
        if (relativeLower.compare(0, std::wcslen(prefix), prefix) == 0) return false;
    }
    return true;
}

namespace {

void Enumerate(const std::wstring& directory, std::vector<std::wstring>& out, int depth) {
    if (depth > 24) return;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((directory + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = directory + L"\\" + name;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Junctions are followed; symbolic links are not.
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && data.dwReserved0 != IO_REPARSE_TAG_MOUNT_POINT) {
                continue;
            }
            Enumerate(full, out, depth + 1);
        } else {
            out.push_back(full);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

}  // namespace

std::vector<std::wstring> BuildAllList(const std::wstring& gameDir, const std::wstring& userDir,
                                       const std::vector<std::string>& enabledMods, bool skipGame) {
    std::vector<std::wstring> files;
    auto addTree = [&files](const std::wstring& base) {
        std::vector<std::wstring> found;
        Enumerate(base, found, 0);
        for (auto& f : found) {
            if (AllListWanted(Lower(f.substr(base.size() + 1)))) files.push_back(std::move(f));
        }
    };
    if (!skipGame) addTree(gameDir);
    for (const std::string& descriptor : enabledMods) {
        std::string text;
        const std::wstring descriptorPath = userDir + L"\\" + Normalize(Widen(descriptor));
        if (!ReadWhole(descriptorPath, text, 1 << 20)) continue;
        files.push_back(descriptorPath);
        std::wstring path = Normalize(Widen(DescriptorValue(text, "path")));
        if (!path.empty()) {
            if (!IsAbsolute(path)) path = userDir + L"\\" + path;
            while (!path.empty() && path.back() == L'\\') path.pop_back();
            addTree(path);
        }
        std::wstring archive = Normalize(Widen(DescriptorValue(text, "archive")));
        if (!archive.empty()) {
            if (!IsAbsolute(archive)) archive = userDir + L"\\" + archive;
            files.push_back(archive);
        }
    }
    std::stable_sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) { return Rank(a) < Rank(b); });
    std::vector<std::wstring> stored;
    stored.reserve(files.size());
    for (const auto& f : files) {
        const std::wstring lower = Lower(f);
        if (lower.size() >= 4 &&
            (lower.compare(lower.size() - 4, 4, L".exe") == 0 || lower.compare(lower.size() - 4, 4, L".dll") == 0)) {
            continue;
        }
        stored.push_back(ToStored(f, gameDir));
    }
    return stored;
}

std::wstring ProfileKey(const char* buildId, const std::string& dlcLoadJson) {
    char key[96] = {};
    std::snprintf(key, sizeof(key), "%s-%016llx", buildId, static_cast<unsigned long long>(Fnv1a(dlcLoadJson)));
    return Widen(key);
}

std::vector<std::wstring> ModFolders(const std::wstring& userDir, const std::vector<std::string>& enabledMods) {
    std::vector<std::wstring> folders;
    for (const std::string& descriptor : enabledMods) {
        std::string text;
        if (!ReadWhole(userDir + L"\\" + Normalize(Widen(descriptor)), text, 1 << 20)) continue;
        std::wstring path = Normalize(Widen(DescriptorValue(text, "path")));
        if (path.empty()) {
            path = Normalize(Widen(DescriptorValue(text, "archive")));
            const std::size_t slash = path.find_last_of(L'\\');
            path = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }
        if (path.empty()) continue;
        if (!IsAbsolute(path)) path = userDir + L"\\" + path;
        while (!path.empty() && path.back() == L'\\') path.pop_back();
        folders.push_back(std::move(path));
    }
    return folders;
}

bool FileGone(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

std::vector<std::wstring> UniqueStored(const std::vector<std::wstring>& paths, const std::wstring& gameDir,
                                       const std::vector<std::wstring>& previous) {
    constexpr std::size_t kMaxEntries = 60000;
    std::vector<std::wstring> out;
    std::unordered_set<std::wstring> seen;
    for (const auto& p : paths) {
        const std::wstring stored = ToStored(p, gameDir);
        if (seen.insert(Lower(stored)).second) out.push_back(stored);
    }
    for (const auto& stored : previous) {
        if (out.size() >= kMaxEntries) break;
        if (stored.empty() || !seen.insert(Lower(stored)).second) continue;
        if (!FileGone(FromStored(stored, gameDir))) out.push_back(stored);
    }
    return out;
}

bool WriteList(const std::wstring& target, const std::vector<std::wstring>& lines) {
    std::string text;
    for (const auto& l : lines) {
        text += Narrow(l);
        text += '\n';
    }
    const std::wstring temp = target + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::wstring UserFolderName(const std::wstring& gameDir) {
    const DWORD marker = GetFileAttributesW((gameDir + L"\\goggame-2057001589.info").c_str());
    return marker != INVALID_FILE_ATTRIBUTES ? L"Europa Universalis IV GOG" : L"Europa Universalis IV";
}

std::wstring UserDirArgument(std::wstring_view line) {
    // Past the program's name, whose path may hold the same text.
    std::size_t at = line.starts_with(L'"') ? line.find(L'"', 1) : line.find(L' ');
    if (at == std::wstring_view::npos) return {};
    constexpr std::wstring_view key = L"-userdir=";
    for (at = line.find(key, at + 1); at != std::wstring_view::npos; at = line.find(key, at + 1)) {
        const wchar_t before = line[at - 1];
        if (before != L' ' && before != L'\t' && before != L'"') continue;
        std::wstring_view value = line.substr(at + key.size());
        std::size_t end = std::wstring_view::npos;
        if (before == L'"') {
            end = value.find(L'"');
        } else if (value.starts_with(L'"')) {  // -userdir="C:/a b/": the C runtime takes the quotes off for the game
            value.remove_prefix(1);
            end = value.find(L'"');
        } else {
            for (const std::wstring_view stop : {L" -", L" +", L" /", L" continue="}) end = (std::min)(end, value.find(stop));
        }
        value = value.substr(0, end);
        while (!value.empty() && value.back() == L' ') value.remove_suffix(1);
        return std::wstring(value);
    }
    return {};
}

namespace {

// Backslashes, and no slash at the end of anything but a drive's root.
std::wstring Tidy(std::wstring path) {
    path = Normalize(std::move(path));
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool IsFolder(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}  // namespace

std::wstring UserDirectoryFor(const std::wstring& commandLine, const std::wstring& gameDir, const std::wstring& documents) {
    const std::wstring named = UserDirArgument(commandLine);
    if (!named.empty() && IsFolder(named)) return Tidy(named);
    std::string text;
    if (ReadWhole(gameDir + L"\\userdir.txt", text, 1 << 16)) {
        const auto lines = ParseList(text);
        if (!lines.empty()) {
            std::wstring path = lines.front();
            while (!path.empty() && (path.back() == L' ' || path.back() == L'\t')) path.pop_back();
            path.erase(0, path.find_first_not_of(L" \t"));
            if (!path.empty()) return Tidy(path);
        }
    }
    if (documents.empty()) return {};
    return documents + L"\\Paradox Interactive\\" + UserFolderName(gameDir);
}

std::wstring UserDirectory() {
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe)) return {};
    std::wstring gameDir(exe, length);
    gameDir.resize(gameDir.find_last_of(L'\\'));
    PWSTR documents = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents != nullptr) folder = documents;
    if (documents != nullptr) CoTaskMemFree(documents);
    return UserDirectoryFor(GetCommandLineW(), gameDir, folder);
}

bool OnRotatingDisk(const std::wstring& path) {
    wchar_t volume[MAX_PATH] = {};
    if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH) || volume[0] == L'\0' || volume[1] != L':') return false;
    const wchar_t device[] = {L'\\', L'\\', L'.', L'\\', volume[0], L':', L'\0'};
    // No access rights: the property needs none, and a player has no rights on the volume.
    HANDLE disk = CreateFileW(device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (disk == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR answer{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &answer, sizeof(answer),
                                    &bytes, nullptr);
    CloseHandle(disk);
    return ok && bytes >= sizeof(answer) && answer.IncursSeekPenalty;
}

unsigned WorkersFor(const std::wstring& gameDir) { return OnRotatingDisk(gameDir) ? kWorkersRotating : kWorkers; }

}  // namespace sb::profile
