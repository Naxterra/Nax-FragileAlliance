// The files a translated DOS program sees, behind DOS-style paths and handles. A drive is either a folder
// with read-only original layers and a writable user layer on top, or a read-only CD image. The original
// files are never changed: every write goes to the user layer, first into a temporary file that replaces
// the previous version only when the program closes the file.
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime/iso9660.h"

namespace nax {

// DOS error codes returned by the functions below.
enum DosError : uint16_t {
    kFileNotFound = 2,
    kPathNotFound = 3,
    kTooManyFiles = 4,
    kAccessDenied = 5,
    kBadHandle = 6,
    kFileExists = 80,
};

class GameFiles {
public:
    struct Found {
        std::string name;  // upper case 8.3 name
        uint32_t size = 0;
        bool directory = false;
        uint16_t time = 0, date = 0;  // DOS time and date
    };

    GameFiles();
    ~GameFiles();

    // Drive C-style folder: writes go to user; reads look in user, then in each read layer in order.
    void mount_folder(char drive, const std::filesystem::path& user, std::vector<std::filesystem::path> read_layers);
    bool mount_image(char drive, const std::filesystem::path& image);
    void set_current_drive(char drive) { current_ = drive; }
    char current_drive() const { return current_; }
    bool has_drive(char drive) const;
    std::string current_directory(char drive) const;  // without drive and leading backslash
    uint16_t change_directory(const std::string& path);

    // access: 0 read, 1 write, 2 read and write. Returns a handle of 5 or more, or -1 with *error set.
    int open(const std::string& path, int access, bool create, bool truncate, bool exclusive, uint16_t* error);
    int64_t read(int handle, void* buffer, uint32_t size, uint16_t* error);
    int64_t write(int handle, const void* buffer, uint32_t size, uint16_t* error);
    // whence: 0 start, 1 current, 2 end. Returns the new position, or -1 with *error set.
    int64_t seek(int handle, int64_t offset, int whence, uint16_t* error);
    bool close(int handle, bool commit);
    // Closes every file. Writes are kept only when commit is true.
    void close_all(bool commit);
    // Finds a file or directory. Returns false when it does not exist.
    bool stat(const std::string& path, Found* found, bool* read_only);
    // Lists the entries of a directory that match a DOS wildcard pattern such as "_SAVE\*.SAV".
    std::vector<Found> find(const std::string& pattern);
    uint16_t remove(const std::string& path);
    uint16_t rename(const std::string& from, const std::string& to);
    // Removes temporary files that an interrupted earlier session left in the user layer.
    void clean_temporary_files();

    // Splits a DOS path into its drive and normalised relative path (upper case, backslashes, no ".").
    bool normalise(const std::string& path, char* drive, std::string* relative) const;

private:
    struct Drive {
        bool image = false;
        std::filesystem::path user;
        std::vector<std::filesystem::path> layers;
        std::unique_ptr<IsoImage> iso;
        std::string cwd;
    };
    struct Open {
        bool image = false;
        const IsoImage::Entry* entry = nullptr;
        const IsoImage* iso = nullptr;
        uint32_t position = 0;
        HANDLE file = INVALID_HANDLE_VALUE;
        std::filesystem::path final_path, temp_path;
    };
    Drive* drive(char letter);
    const Drive* drive(char letter) const;
    std::filesystem::path existing(const Drive& d, const std::string& relative) const;

    std::map<char, Drive> drives_;
    std::map<int, Open> open_;
    char current_ = 'C';
};

}  // namespace nax
