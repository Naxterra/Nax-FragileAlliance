// Read-only access to the files of an ISO 9660 CD image.
#include "runtime/iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace nax {

namespace {
constexpr uint32_t kSector = 2048;

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}  // namespace

IsoImage::~IsoImage() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool IsoImage::read_sectors(uint32_t sector, uint32_t bytes, std::vector<uint8_t>* out) const {
    out->assign(bytes, 0);
    OVERLAPPED at{};
    const uint64_t offset = uint64_t(sector) * kSector;
    at.Offset = DWORD(offset);
    at.OffsetHigh = DWORD(offset >> 32);
    DWORD got = 0;
    return ReadFile(file_, out->data(), bytes, &got, &at) && got == bytes;
}

bool IsoImage::open(const std::filesystem::path& file) {
    file_ = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> pvd;
    if (!read_sectors(16, kSector, &pvd) || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        return false;
    }
    const uint8_t* root = &pvd[156];
    entries_[""] = Entry{le32(root + 2), le32(root + 10), true};
    if (!walk("", le32(root + 2), le32(root + 10), 0)) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

bool IsoImage::walk(const std::string& prefix, uint32_t extent, uint32_t size, int depth) {
    if (depth > 16 || size > 16 * 1024 * 1024) return false;
    std::vector<uint8_t> data;
    if (!read_sectors(extent, (size + kSector - 1) / kSector * kSector, &data)) return false;
    uint32_t at = 0;
    while (at < size) {
        const uint8_t length = data[at];
        if (length == 0) {
            at = (at / kSector + 1) * kSector;  // records never cross a sector boundary
            continue;
        }
        if (at + length > data.size() || length < 34) return false;
        const uint8_t* record = &data[at];
        const uint8_t name_length = record[32];
        if (33u + name_length > length) return false;
        const bool directory = (record[25] & 2) != 0;
        std::string name(reinterpret_cast<const char*>(record + 33), name_length);
        at += length;
        if (name_length == 1 && (name[0] == 0 || name[0] == 1)) continue;  // "." and ".."
        if (const size_t semicolon = name.find(';'); semicolon != std::string::npos) name.resize(semicolon);
        if (!name.empty() && name.back() == '.') name.pop_back();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
        const std::string path = prefix.empty() ? name : prefix + "\\" + name;
        const Entry entry{le32(record + 2), le32(record + 10), directory};
        entries_[path] = entry;
        children_[prefix].push_back(name);
        if (directory && !walk(path, entry.extent, entry.size, depth + 1)) return false;
    }
    return true;
}

const IsoImage::Entry* IsoImage::find(const std::string& path) const {
    const auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<std::string> IsoImage::list(const std::string& directory) const {
    const auto it = children_.find(directory);
    return it == children_.end() ? std::vector<std::string>{} : it->second;
}

uint32_t IsoImage::read(const Entry& entry, uint32_t offset, void* buffer, uint32_t size) const {
    if (offset >= entry.size) return 0;
    size = std::min(size, entry.size - offset);
    OVERLAPPED at{};
    const uint64_t position = uint64_t(entry.extent) * kSector + offset;
    at.Offset = DWORD(position);
    at.OffsetHigh = DWORD(position >> 32);
    DWORD got = 0;
    if (!ReadFile(file_, buffer, size, &got, &at)) return 0;
    return got;
}

}  // namespace nax
