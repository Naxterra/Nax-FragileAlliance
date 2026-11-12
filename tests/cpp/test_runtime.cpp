// Tests of the runtime's file layer and CD image reader. Usage: test_runtime <case> <work folder> [<image>]
// Each case prints nothing and returns 0 when it passes, or prints the reason and returns 1.
#include <cstdio>
#include <fstream>
#include <string>

#include "runtime/files.h"
#include "runtime/iso9660.h"

namespace fs = std::filesystem;
using nax::GameFiles;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("%s\n", what);
        failures++;
    }
}

void put(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string get(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Setup {
    fs::path original, user;
    GameFiles files;
    explicit Setup(const fs::path& work) : original(work / "original"), user(work / "user") {
        fs::remove_all(work);
        put(original / "CONFIG.INI", "old");
        put(original / "_SAVE" / "GAME1.SAV", "save one");
        fs::create_directories(user);
        files.mount_folder('C', user, {original});
    }
};

int write(GameFiles& files, int handle, const std::string& text) {
    uint16_t error = 0;
    return int(files.write(handle, text.data(), uint32_t(text.size()), &error));
}

std::string read_all(GameFiles& files, const std::string& path) {
    uint16_t error = 0;
    const int h = files.open(path, 0, false, false, false, &error);
    if (h < 0) return "<missing>";
    char buffer[256];
    const int64_t n = files.read(h, buffer, sizeof buffer, &error);
    files.close(h, true);
    return std::string(buffer, n > 0 ? size_t(n) : 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string name = argv[1];
    const fs::path work = argv[2];

    if (name == "parent_directories_never_leave_the_drive") {
        Setup s(work);
        char drive = 0;
        std::string relative;
        check(s.files.normalise("C:\\..\\..\\WINDOWS\\SYSTEM.INI", &drive, &relative), "normalise failed");
        check(drive == 'C' && relative == "WINDOWS\\SYSTEM.INI", "the path climbed above the drive root");
        uint16_t error = 0;
        const int h = s.files.open("..\\..\\..\\outside.txt", 1, true, true, false, &error);
        check(h >= 5, "creating the file failed");
        write(s.files, h, "x");
        s.files.close(h, true);
        check(fs::exists(s.user / "OUTSIDE.TXT") || fs::exists(s.user / "outside.txt"), "the file is not in the user folder");
        check(!fs::exists(work.parent_path() / "outside.txt"), "the file was written outside the user folder");
    } else if (name == "write_goes_to_user_folder_and_original_stays") {
        Setup s(work);
        uint16_t error = 0;
        const int h = s.files.open("C:\\CONFIG.INI", 2, false, false, false, &error);
        check(h >= 5, "opening for writing failed");
        s.files.seek(h, 0, 2, &error);
        write(s.files, h, " new");
        check(s.files.close(h, true), "closing failed");
        check(get(s.original / "CONFIG.INI") == "old", "the original file changed");
        check(get(s.user / "CONFIG.INI") == "old new", "the user copy does not hold the written text");
        check(read_all(s.files, "CONFIG.INI") == "old new", "reading does not prefer the user copy");
    } else if (name == "interrupted_write_keeps_previous_version") {
        Setup s(work);
        put(s.user / "_SAVE" / "GAME1.SAV", "previous");
        uint16_t error = 0;
        const int h = s.files.open("C:\\_SAVE\\GAME1.SAV", 1, true, true, false, &error);
        check(h >= 5, "opening for writing failed");
        write(s.files, h, "half");
        s.files.close_all(false);  // the program failed before it closed the file
        check(get(s.user / "_SAVE" / "GAME1.SAV") == "previous", "the previous version was replaced");
        check(!fs::exists(s.user / "_SAVE" / "GAME1.SAV.nax-tmp"), "a temporary file was left behind");
    } else if (name == "write_is_invisible_until_close") {
        Setup s(work);
        uint16_t error = 0;
        const int h = s.files.open("C:\\NEW.DAT", 1, true, true, false, &error);
        write(s.files, h, "data");
        check(!fs::exists(s.user / "NEW.DAT"), "the final file exists before the close");
        s.files.close(h, true);
        check(get(s.user / "NEW.DAT") == "data", "the final file is missing after the close");
    } else if (name == "leftover_temporary_files_are_removed") {
        Setup s(work);
        put(s.user / "_SAVE" / "GAME2.SAV.nax-tmp", "junk");
        s.files.clean_temporary_files();
        check(!fs::exists(s.user / "_SAVE" / "GAME2.SAV.nax-tmp"), "the temporary file is still there");
    } else if (name == "missing_file_reports_dos_error_2") {
        Setup s(work);
        uint16_t error = 0;
        check(s.files.open("C:\\NOPE.TXT", 0, false, false, false, &error) == -1 && error == nax::kFileNotFound,
              "the error is not file not found");
    } else if (name == "find_lists_both_layers_without_temporary_files") {
        Setup s(work);
        put(s.user / "_SAVE" / "GAME2.SAV", "two");
        put(s.user / "_SAVE" / "GAME3.SAV.nax-tmp", "junk");
        const auto found = s.files.find("C:\\_SAVE\\*.SAV");
        std::string names;
        for (const auto& f : found) names += f.name + " ";
        check(names == "GAME1.SAV GAME2.SAV ", ("unexpected list: " + names).c_str());
    } else if (name == "removing_an_original_file_is_refused") {
        Setup s(work);
        check(s.files.remove("C:\\CONFIG.INI") == nax::kAccessDenied, "removing the original was not refused");
        check(fs::exists(s.original / "CONFIG.INI"), "the original file was removed");
    } else if (name == "cd_image_files_are_read_and_listed") {
        GameFiles files;
        check(files.mount_image('D', argv[3]), "the image did not open");
        uint16_t error = 0;
        const int h = files.open("D:\\DATA\\README.TXT", 0, false, false, false, &error);
        check(h >= 5, "the file in the image was not found");
        char buffer[64] = {};
        const int64_t n = files.read(h, buffer, sizeof buffer, &error);
        check(n == 11 && std::string(buffer, 11) == "hello world", "the file content is wrong");
        files.seek(h, 6, 0, &error);
        const int64_t m = files.read(h, buffer, 5, &error);
        check(m == 5 && std::string(buffer, 5) == "world", "reading after a seek is wrong");
        check(files.open("D:\\DATA\\README.TXT", 1, false, false, false, &error) == -1, "writing to the image was allowed");
        const auto found = files.find("D:\\DATA\\*.*");
        check(found.size() == 1 && found[0].name == "README.TXT", "listing the image directory is wrong");
    } else {
        std::printf("unknown case %s\n", name.c_str());
        return 2;
    }
    return failures ? 1 : 0;
}
