// The files a translated DOS program sees, behind DOS-style paths and handles.
#include "runtime/files.h"

#include <algorithm>
#include <cctype>

namespace nax {

namespace {

constexpr wchar_t kTemporarySuffix[] = L".nax-tmp";
constexpr int kFirstHandle = 5;
constexpr int kLastHandle = 254;

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
    return text;
}

std::filesystem::path under(const std::filesystem::path& folder, const std::string& relative) {
    std::filesystem::path result = folder;
    size_t start = 0;
    while (start <= relative.size() && !relative.empty()) {
        const size_t end = relative.find('\\', start);
        const std::string part = relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) result /= std::filesystem::path(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

// DOS wildcard match of an 8.3 name against a pattern such as "*.SAV" or "GAME?.DAT".
bool wildcard(const std::string& name, const std::string& pattern) {
    auto split = [](const std::string& s, std::string* base, std::string* ext) {
        const size_t dot = s.rfind('.');
        *base = dot == std::string::npos ? s : s.substr(0, dot);
        *ext = dot == std::string::npos ? "" : s.substr(dot + 1);
    };
    auto part = [](const std::string& text, const std::string& mask) {
        size_t i = 0;
        for (size_t k = 0; k < mask.size(); k++) {
            if (mask[k] == '*') return true;
            if (mask[k] == '?') { if (i < text.size()) i++; continue; }
            if (i >= text.size() || text[i] != mask[k]) return false;
            i++;
        }
        return i == text.size();
    };
    std::string nb, ne, pb, pe;
    split(name, &nb, &ne);
    split(pattern, &pb, &pe);
    if (pattern == "*.*" || pattern == "*") return true;
    return part(nb, pb) && part(ne, pe);
}

void dos_time(const FILETIME& utc, uint16_t* time, uint16_t* date) {
    FILETIME local;
    FileTimeToLocalFileTime(&utc, &local);
    WORD d = 0, t = 0;
    FileTimeToDosDateTime(&local, &d, &t);
    *time = t;
    *date = d;
}

uint16_t last_error() {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: return kFileNotFound;
    case ERROR_PATH_NOT_FOUND: return kPathNotFound;
    case ERROR_TOO_MANY_OPEN_FILES: return kTooManyFiles;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return kFileExists;
    default: return kAccessDenied;
    }
}

}  // namespace

GameFiles::GameFiles() = default;

GameFiles::~GameFiles() { close_all(false); }

void GameFiles::mount_folder(char letter, const std::filesystem::path& user, std::vector<std::filesystem::path> layers) {
    Drive d;
    d.user = user;
    d.layers = std::move(layers);
    drives_[char(std::toupper(letter))] = std::move(d);
}

bool GameFiles::mount_image(char letter, const std::filesystem::path& image) {
    Drive d;
    d.image = true;
    d.iso = std::make_unique<IsoImage>();
    if (!d.iso->open(image)) return false;
    drives_[char(std::toupper(letter))] = std::move(d);
    return true;
}

bool GameFiles::has_drive(char letter) const { return drive(letter) != nullptr; }

GameFiles::Drive* GameFiles::drive(char letter) {
    const auto it = drives_.find(char(std::toupper(letter)));
    return it == drives_.end() ? nullptr : &it->second;
}

const GameFiles::Drive* GameFiles::drive(char letter) const {
    const auto it = drives_.find(char(std::toupper(letter)));
    return it == drives_.end() ? nullptr : &it->second;
}

std::string GameFiles::current_directory(char letter) const {
    const Drive* d = drive(letter);
    return d ? d->cwd : std::string();
}

bool GameFiles::normalise(const std::string& path, char* letter, std::string* relative) const {
    std::string p = upper(path);
    std::replace(p.begin(), p.end(), '/', '\\');
    char drv = current_;
    if (p.size() >= 2 && p[1] == ':') {
        drv = p[0];
        p = p.substr(2);
    }
    const Drive* d = drive(drv);
    if (!d) return false;
    std::vector<std::string> parts;
    if (p.empty() || p[0] != '\\') {
        size_t start = 0;
        const std::string& cwd = d->cwd;
        while (start < cwd.size()) {
            const size_t end = cwd.find('\\', start);
            parts.push_back(cwd.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find('\\', start);
        std::string part = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        // Trailing spaces and dots are not part of a DOS name.
        while (!part.empty() && (part.back() == ' ')) part.pop_back();
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();  // never above the root of the drive
        } else if (!part.empty() && part != ".") {
            if (part.find_first_of(":*?\"<>|") != std::string::npos) return false;
            parts.push_back(part);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string joined;
    for (const std::string& part : parts) joined += (joined.empty() ? "" : "\\") + part;
    *letter = drv;
    *relative = joined;
    return true;
}

std::filesystem::path GameFiles::existing(const Drive& d, const std::string& relative) const {
    std::error_code ec;
    const std::filesystem::path mine = under(d.user, relative);
    if (std::filesystem::exists(mine, ec)) return mine;
    for (const auto& layer : d.layers) {
        const std::filesystem::path candidate = under(layer, relative);
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

uint16_t GameFiles::change_directory(const std::string& path) {
    char letter;
    std::string relative;
    if (!normalise(path, &letter, &relative)) return kPathNotFound;
    Found found;
    bool read_only;
    if (!relative.empty() && (!stat(std::string(1, letter) + ":\\" + relative, &found, &read_only) || !found.directory))
        return kPathNotFound;
    drive(letter)->cwd = relative;
    return 0;
}

int GameFiles::open(const std::string& path, int access, bool create, bool truncate, bool exclusive, uint16_t* error) {
    char letter;
    std::string relative;
    if (!normalise(path, &letter, &relative) || relative.empty()) { *error = kPathNotFound; return -1; }
    Drive& d = *drive(letter);
    int handle = kFirstHandle;
    while (open_.count(handle)) handle++;
    if (handle > kLastHandle) { *error = kTooManyFiles; return -1; }
    Open file;
    if (d.image) {
        const IsoImage::Entry* entry = d.iso->find(relative);
        if (!entry || entry->directory) { *error = entry ? kAccessDenied : kFileNotFound; return -1; }
        if (access != 0 || truncate) { *error = kAccessDenied; return -1; }
        file.image = true;
        file.entry = entry;
        file.iso = d.iso.get();
        open_[handle] = file;
        return handle;
    }
    const std::filesystem::path source = existing(d, relative);
    std::error_code ec;
    if (!source.empty() && std::filesystem::is_directory(source, ec)) { *error = kAccessDenied; return -1; }
    if (source.empty() && !create) { *error = kFileNotFound; return -1; }
    if (!source.empty() && create && exclusive) { *error = kFileExists; return -1; }
    if (access == 0 && !source.empty()) {
        file.file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.file == INVALID_HANDLE_VALUE) { *error = last_error(); return -1; }
        open_[handle] = file;
        return handle;
    }
    // Writing: work on a temporary copy in the user layer.
    file.final_path = under(d.user, relative);
    file.temp_path = file.final_path;
    file.temp_path += kTemporarySuffix;
    std::filesystem::create_directories(file.final_path.parent_path(), ec);
    if (!source.empty() && !truncate) {
        if (!CopyFileW(source.c_str(), file.temp_path.c_str(), FALSE)) { *error = last_error(); return -1; }
        SetFileAttributesW(file.temp_path.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    file.file = CreateFileW(file.temp_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            (!source.empty() && !truncate) ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.file == INVALID_HANDLE_VALUE) { *error = last_error(); return -1; }
    open_[handle] = file;
    return handle;
}

int64_t GameFiles::read(int handle, void* buffer, uint32_t size, uint16_t* error) {
    const auto it = open_.find(handle);
    if (it == open_.end()) { *error = kBadHandle; return -1; }
    Open& f = it->second;
    if (f.image) {
        const uint32_t got = f.iso->read(*f.entry, f.position, buffer, size);
        f.position += got;
        return got;
    }
    DWORD got = 0;
    if (!ReadFile(f.file, buffer, size, &got, nullptr)) { *error = last_error(); return -1; }
    return got;
}

int64_t GameFiles::write(int handle, const void* buffer, uint32_t size, uint16_t* error) {
    const auto it = open_.find(handle);
    if (it == open_.end()) { *error = kBadHandle; return -1; }
    Open& f = it->second;
    if (f.image || f.temp_path.empty()) { *error = kAccessDenied; return -1; }
    DWORD done = 0;
    if (size == 0) {
        // A DOS write of zero bytes truncates the file at the current position.
        if (!SetEndOfFile(f.file)) { *error = last_error(); return -1; }
        return 0;
    }
    if (!WriteFile(f.file, buffer, size, &done, nullptr)) { *error = last_error(); return -1; }
    return done;
}

int64_t GameFiles::seek(int handle, int64_t offset, int whence, uint16_t* error) {
    const auto it = open_.find(handle);
    if (it == open_.end()) { *error = kBadHandle; return -1; }
    Open& f = it->second;
    if (f.image) {
        int64_t base = whence == 0 ? 0 : whence == 1 ? f.position : f.entry->size;
        int64_t target = base + offset;
        if (target < 0 || target > 0xFFFFFFFFll) { *error = kAccessDenied; return -1; }
        f.position = uint32_t(target);
        return target;
    }
    LARGE_INTEGER move, now;
    move.QuadPart = offset;
    if (!SetFilePointerEx(f.file, move, &now, whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END)) {
        *error = last_error();
        return -1;
    }
    return now.QuadPart;
}

bool GameFiles::close(int handle, bool commit) {
    const auto it = open_.find(handle);
    if (it == open_.end()) return false;
    Open f = it->second;
    open_.erase(it);
    if (f.image) return true;
    bool ok = true;
    if (!f.temp_path.empty()) ok = FlushFileBuffers(f.file) != 0;
    CloseHandle(f.file);
    if (f.temp_path.empty()) return true;
    if (commit && ok) {
        if (MoveFileExW(f.temp_path.c_str(), f.final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
    }
    DeleteFileW(f.temp_path.c_str());
    return commit ? false : true;
}

void GameFiles::close_all(bool commit) {
    while (!open_.empty()) close(open_.begin()->first, commit);
}

bool GameFiles::stat(const std::string& path, Found* found, bool* read_only) {
    char letter;
    std::string relative;
    if (!normalise(path, &letter, &relative)) return false;
    Drive& d = *drive(letter);
    const size_t slash = relative.rfind('\\');
    found->name = slash == std::string::npos ? relative : relative.substr(slash + 1);
    if (d.image) {
        const IsoImage::Entry* e = d.iso->find(relative);
        if (!e) return false;
        found->size = e->size;
        found->directory = e->directory;
        found->time = 0;
        found->date = (16 << 9) | (6 << 5) | 1;  // 1 June 1996, the image keeps no time the game needs
        *read_only = true;
        return true;
    }
    if (relative.empty()) {
        found->directory = true;
        *read_only = false;
        return true;
    }
    const std::filesystem::path p = existing(d, relative);
    if (p.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return false;
    found->directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    found->size = data.nFileSizeHigh ? 0xFFFFFFFFu : data.nFileSizeLow;
    dos_time(data.ftLastWriteTime, &found->time, &found->date);
    *read_only = false;  // every file can be written, because writes go to the user layer
    return true;
}

std::vector<GameFiles::Found> GameFiles::find(const std::string& pattern) {
    std::vector<Found> result;
    // Wildcards are only allowed in the last part, so the folder is normalised on its own.
    std::string p = pattern;
    std::replace(p.begin(), p.end(), '/', '\\');
    const size_t cut = p.find_last_of("\\:");
    std::string where = cut == std::string::npos ? "." : p.substr(0, cut + 1);
    const std::string mask = upper(cut == std::string::npos ? p : p.substr(cut + 1));
    if (where.size() > 1 && where.back() == '\\' && where[where.size() - 2] != ':') where.pop_back();
    char letter;
    std::string folder;
    if (mask.empty() || !normalise(where, &letter, &folder)) return result;
    Drive& d = *drive(letter);
    if (d.image) {
        for (const std::string& name : d.iso->list(folder)) {
            if (!wildcard(name, mask)) continue;
            const IsoImage::Entry* e = d.iso->find(folder.empty() ? name : folder + "\\" + name);
            Found f;
            f.name = name;
            f.size = e->size;
            f.directory = e->directory;
            f.date = (16 << 9) | (6 << 5) | 1;
            result.push_back(f);
        }
        return result;
    }
    std::map<std::string, Found> merged;  // the user layer hides the same name in the original layers
    std::vector<std::filesystem::path> folders = {under(d.user, folder)};
    for (const auto& layer : d.layers) folders.push_back(under(layer, folder));
    for (const auto& dir : folders) {
        std::error_code ec;
        for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
            std::string name = upper(item.path().filename().string());
            if (name.size() > 12 || item.path().extension() == kTemporarySuffix) continue;
            if (!wildcard(name, mask) || merged.count(name)) continue;
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(item.path().c_str(), GetFileExInfoStandard, &data)) continue;
            Found f;
            f.name = name;
            f.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            f.size = data.nFileSizeLow;
            dos_time(data.ftLastWriteTime, &f.time, &f.date);
            merged[name] = f;
        }
    }
    for (auto& [name, f] : merged) result.push_back(f);
    return result;
}

uint16_t GameFiles::remove(const std::string& path) {
    char letter;
    std::string relative;
    if (!normalise(path, &letter, &relative)) return kPathNotFound;
    Drive& d = *drive(letter);
    if (d.image) return kAccessDenied;
    // Only files the program wrote itself can be removed; originals stay untouched.
    const std::filesystem::path mine = under(d.user, relative);
    if (DeleteFileW(mine.c_str())) return 0;
    return existing(d, relative).empty() ? kFileNotFound : kAccessDenied;
}

uint16_t GameFiles::rename(const std::string& from, const std::string& to) {
    char l1, l2;
    std::string r1, r2;
    if (!normalise(from, &l1, &r1) || !normalise(to, &l2, &r2)) return kPathNotFound;
    if (l1 != l2) return 17;  // not the same device
    Drive& d = *drive(l1);
    if (d.image) return kAccessDenied;
    const std::filesystem::path source = under(d.user, r1), target = under(d.user, r2);
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) return existing(d, r1).empty() ? kFileNotFound : kAccessDenied;
    if (!existing(d, r2).empty()) return kAccessDenied;
    std::filesystem::create_directories(target.parent_path(), ec);
    return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) ? 0 : last_error();
}

void GameFiles::clean_temporary_files() {
    for (auto& [letter, d] : drives_) {
        if (d.image || d.user.empty()) continue;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(d.user, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().extension() == kTemporarySuffix) std::filesystem::remove(it->path(), ec);
        }
    }
}

}  // namespace nax
