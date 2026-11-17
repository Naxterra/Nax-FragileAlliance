// Cases for tests/test_textures.py. Each case runs in its own folder (the second argument), returns 0 when
// it passes and prints what went wrong otherwise.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "runtime/image.h"
#include "runtime/textures.h"

using namespace nax;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("failed: %s\n", what);
        failures++;
    }
}

// A 4x3 picture: indices 1 and 2 in the left two columns, the right two columns transparent.
GameImage sample_image() {
    GameImage image;
    image.width = 4;
    image.height = 3;
    image.index.assign(12, 0);
    image.opaque.assign(12, 0);
    for (int y = 0; y < 3; y++) {
        image.index[y * 4 + 0] = 1;
        image.index[y * 4 + 1] = 2;
        image.opaque[y * 4 + 0] = image.opaque[y * 4 + 1] = 1;
    }
    return image;
}

struct Palette {
    uint8_t c[256][3]{};
    Palette() {
        c[1][0] = 63;  // index 1 full red
        c[2][2] = 63;  // index 2 full blue
    }
};

uint64_t key_of(int n) { return 0x1234567800000000ull + uint64_t(n); }

int dumped_picture_keeps_its_colours_and_palette(const fs::path& folder) {
    TexturePack pack;
    pack.open(folder / "pack", folder / "dump", true);
    Palette palette;
    int decodes = 0;
    auto decode = [&](GameImage* image) { decodes++; *image = sample_image(); return true; };
    check(pack.get(key_of(1), decode, palette.c, true) == nullptr, "a picture that is only dumped has no replacement");
    const fs::path png = folder / "dump" / (texture_name(key_of(1)) + L".png");
    std::vector<uint32_t> rgba;
    int w = 0, h = 0;
    check(load_png(png, &rgba, &w, &h), "the dumped picture is a readable PNG");
    const int m = kTextureMargin;
    if (w == 4 + 2 * m && h == 3 + 2 * m) {
        auto at = [&](int x, int y) { return rgba[size_t(y + m) * w + x + m]; };
        check(at(0, 0) == 0xFF0000FFu, "an index 1 pixel is opaque red");
        check(at(1, 0) == 0xFFFF0000u, "an index 2 pixel is opaque blue");
        check((at(2, 0) >> 24) == 0, "a transparent pixel has alpha 0");
        check((at(2, 0) & 0xFFFFFF) == 0xFF0000u, "a transparent pixel takes the colour of its drawn neighbour");
        check((at(-1, 0) >> 24) == 0 && (at(-1, 0) & 0xFFFFFF) == 0x0000FFu, "the margin is transparent in the edge colour");
    } else {
        check(false, "the dumped picture has the original size plus the margin");
    }
    check(fs::file_size(folder / "dump" / (texture_name(key_of(1)) + L".pal")) == 768, "the palette is saved beside it");
    pack.get(key_of(1), decode, palette.c, true);
    check(decodes == 1, "a picture is decoded and dumped only once");
    return failures;
}

int picture_is_not_dumped_while_the_palette_changes(const fs::path& folder) {
    TexturePack pack;
    pack.open(folder / "pack", folder / "dump", true);
    Palette palette;
    auto decode = [&](GameImage* image) { *image = sample_image(); return true; };
    const fs::path png = folder / "dump" / (texture_name(key_of(2)) + L".png");
    pack.get(key_of(2), decode, palette.c, false);
    check(!fs::exists(png), "nothing is dumped during a fade");
    pack.get(key_of(2), decode, palette.c, true);
    check(fs::exists(png), "the picture is dumped once the palette has settled");
    return failures;
}

int pack_picture_replaces_the_original(const fs::path& folder) {
    fs::create_directories(folder / "pack");
    // Four times the 4x3 original with its margin: 80x76, of which the inner 16x12 is the picture.
    const int m = kTextureMargin * 4, bw = 16 + 2 * m, bh = 12 + 2 * m;
    std::vector<uint32_t> big(size_t(bw) * bh, 0);
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 16; x++) big[size_t(y + m) * bw + x + m] = 0xFF00FF00u;
    check(save_png(folder / "pack" / (texture_name(key_of(3)) + L".png"), big.data(), bw, bh), "the pack picture is written");
    uint8_t reference[256][3]{};
    reference[1][0] = 200;
    if (FILE* f = _wfopen((folder / "pack" / (texture_name(key_of(3)) + L".pal")).c_str(), L"wb")) {
        std::fwrite(reference, 1, sizeof(reference), f);
        std::fclose(f);
    }
    TexturePack pack;
    pack.open(folder / "pack", folder / "dump", false);
    Palette palette;
    auto decode = [&](GameImage* image) { *image = sample_image(); return true; };
    check(pack.get(key_of(3), decode, palette.c, true) == nullptr, "the replacement is not ready before it has been read");
    pack.wait();
    auto t = pack.get(key_of(3), decode, palette.c, true);
    check(t != nullptr, "a picture in the pack is found by its key once read");
    if (!t) return failures;
    check(t->width == 4 && t->height == 3, "the replacement keeps the size of the original");
    check(t->hd_width == 16 && t->hd_height == 12, "the margin is cropped off the enhanced picture");
    check(t->hd[0] == 0xFF00FF00u && t->hd.back() == 0xFF00FF00u, "the enhanced picture keeps its pixels");
    check(t->reference[1][0] == 200, "the palette file of the pack is used as the reference");
    check(t->used.size() == 2 && t->used[0].first == 1 && t->used[0].second == 3, "the indices of the original are counted");
    check(!fs::exists(folder / "dump"), "without dumping nothing is written to the dump folder");
    return failures;
}

int missing_picture_is_never_decoded_while_absent(const fs::path& folder) {
    fs::create_directories(folder / "pack");
    TexturePack pack;
    pack.open(folder / "pack", folder / "dump", false);
    Palette palette;
    int decodes = 0;
    auto decode = [&](GameImage* image) { decodes++; *image = sample_image(); return true; };
    for (int i = 0; i < 5; i++) check(pack.get(key_of(4), decode, palette.c, true) == nullptr, "an unknown picture has no replacement");
    check(decodes == 0, "an unknown picture is never decoded when it is not dumped");
    return failures;
}

int broken_pack_picture_leaves_the_original(const fs::path& folder) {
    fs::create_directories(folder / "pack");
    if (FILE* f = _wfopen((folder / "pack" / (texture_name(key_of(5)) + L".png")).c_str(), L"wb")) {
        std::fwrite("\x89PNG\r\n\x1a\n broken", 1, 16, f);  // a PNG cut off after its signature
        std::fclose(f);
    }
    TexturePack pack;
    pack.open(folder / "pack", folder / "dump", false);
    Palette palette;
    auto decode = [&](GameImage* image) { *image = sample_image(); return true; };
    pack.get(key_of(5), decode, palette.c, true);
    pack.wait();
    check(pack.get(key_of(5), decode, palette.c, true) == nullptr, "a damaged picture gives no replacement");
    check(pack.get(key_of(5), decode, palette.c, true) == nullptr, "and asking again is harmless");
    return failures;
}

int picture_added_to_the_pack_later_is_picked_up(const fs::path& folder) {
    fs::create_directories(folder / "pack");  // made by the pack tools
    TexturePack pack;
    pack.open(folder / "pack", folder / "dump", false);
    pack.set_search_interval(std::chrono::milliseconds(0));
    Palette palette;
    auto decode = [&](GameImage* image) { *image = sample_image(); return true; };
    check(pack.get(key_of(6), decode, palette.c, true) == nullptr, "a picture the pack lacks has no replacement");
    // The generator adds the picture while the game runs.
    const int m = kTextureMargin * 4, bw = 16 + 2 * m, bh = 12 + 2 * m;
    std::vector<uint32_t> big(size_t(bw) * bh, 0xFF0000FFu);
    check(save_png(folder / "pack" / (texture_name(key_of(6)) + L".png"), big.data(), bw, bh), "the new pack picture is written");
    pack.get(key_of(6), decode, palette.c, true);
    pack.wait();
    auto first = pack.get(key_of(6), decode, palette.c, true);
    check(first != nullptr, "the new picture replaces the original without a restart");
    // The redraw tool then replaces the pack picture with a repainted one.
    std::fill(big.begin(), big.end(), 0xFF00FF00u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // a newer file time than the first picture
    check(save_png(folder / "pack" / (texture_name(key_of(6)) + L".png"), big.data(), bw, bh), "the repainted picture is written");
    check(pack.get(key_of(6), decode, palette.c, true) == first, "the first picture stays while the new one is read");
    pack.wait();
    auto second = pack.get(key_of(6), decode, palette.c, true);
    check(second && second != first && second->version > first->version && second->hd[0] == 0xFF00FF00u,
          "the changed file replaces the picture as a newer version");
    return failures;
}

int without_a_pack_folder_nothing_is_created_or_done(const fs::path& folder) {
    TexturePack pack;
    pack.open(folder / "pack", folder / "dump", false);
    check(!pack.active(), "without a pack folder and without dumping the texture work is off");
    check(!fs::exists(folder / "pack") && !fs::exists(folder / "dump"), "and no folder is created");
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    const fs::path folder = argc > 2 ? fs::path(argv[2]) : fs::path(".");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder, ec);
    const struct {
        const char* name;
        int (*run)(const fs::path&);
    } cases[] = {
        {"dumped_picture_keeps_its_colours_and_palette", dumped_picture_keeps_its_colours_and_palette},
        {"picture_is_not_dumped_while_the_palette_changes", picture_is_not_dumped_while_the_palette_changes},
        {"pack_picture_replaces_the_original", pack_picture_replaces_the_original},
        {"missing_picture_is_never_decoded_while_absent", missing_picture_is_never_decoded_while_absent},
        {"broken_pack_picture_leaves_the_original", broken_pack_picture_leaves_the_original},
        {"picture_added_to_the_pack_later_is_picked_up", picture_added_to_the_pack_later_is_picked_up},
        {"without_a_pack_folder_nothing_is_created_or_done", without_a_pack_folder_nothing_is_created_or_done},
    };
    for (const auto& c : cases)
        if (name == c.name) return c.run(folder);
    std::printf("unknown case %s\n", name.c_str());
    return 2;
}
