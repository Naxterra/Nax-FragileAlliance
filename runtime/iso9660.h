// Read-only access to the files of an ISO 9660 CD image, such as the FRAGILE.DAT image of the GOG release.
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nax {

class IsoImage {
public:
    struct Entry {
        uint32_t extent = 0;  // first sector
        uint32_t size = 0;
        bool directory = false;
    };

    IsoImage() = default;
    ~IsoImage();
    IsoImage(const IsoImage&) = delete;
    IsoImage& operator=(const IsoImage&) = delete;

    // Opens the image and reads its directory tree. Returns false when it is not a readable ISO 9660 image.
    bool open(const std::filesystem::path& file);
    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }

    // Paths use upper case, backslashes and no leading backslash, for example "_INTRO\INTRO.FLC".
    const Entry* find(const std::string& path) const;
    // Names directly inside a directory ("" for the root), in image order.
    std::vector<std::string> list(const std::string& directory) const;
    // Reads up to size bytes at offset inside a file. Returns the number of bytes read.
    uint32_t read(const Entry& entry, uint32_t offset, void* buffer, uint32_t size) const;

private:
    bool read_sectors(uint32_t sector, uint32_t bytes, std::vector<uint8_t>* out) const;
    bool walk(const std::string& prefix, uint32_t extent, uint32_t size, int depth);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::vector<std::string>> children_;
};

}  // namespace nax
