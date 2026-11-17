// Texture packs: enhanced replacements for the game's own pictures, and the dump of the originals that a
// pack is made from. Both are folders of PNG files named after the content key of the original picture.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime/display.h"

namespace nax {

// A picture as the game draws it: palette indices, and which pixels are drawn at all.
struct GameImage {
    int width = 0, height = 0;
    std::vector<uint8_t> index, opaque;
};

// Dumped pictures carry a transparent margin of this many pixels on every side, and pack pictures carry the
// same margin enlarged with the picture. The margin gives an upscaler room around the edges (without it,
// some very small pictures make the Real-ESRGAN models fail); the game crops it off when loading.
constexpr int kTextureMargin = 8;

class TexturePack {
public:
    TexturePack() = default;
    TexturePack(const TexturePack&) = delete;
    TexturePack& operator=(const TexturePack&) = delete;
    ~TexturePack();
    // pack holds <key>.png (any size, margin included) with an optional <key>.pal (768 bytes, the palette it
    // was made under); dump receives the originals in the same form when dumping is on.
    void open(const std::filesystem::path& pack, const std::filesystem::path& dump, bool dumping);
    // True when there is a pack folder or dumping is on. A pack folder made while the game runs is noticed
    // within a few seconds; without one, the game does no texture work at all.
    bool active();
    // Returns the replacement for the picture with this key, or null. decode fills in the original; it is
    // called only when the key is new or a dump is still due. palette holds 6-bit components, and settled
    // tells whether it has stopped changing, as a picture dumped during a fade would keep the faded colours.
    // A replacement is read from the pack on a background thread, so the game never waits for a PNG to be
    // decoded: until it is ready, get returns null and the original picture shows. A picture without a
    // replacement is searched for again every few seconds, so pictures that a generator adds to the pack
    // while the game runs appear without a restart; a damaged file is tried again only once it changes.
    std::shared_ptr<const OverlayTexture> get(uint64_t key, const std::function<bool(GameImage*)>& decode,
                                              const uint8_t palette[256][3], bool settled);
    size_t dumped() const { return dumped_count_; }
    // Waits until every replacement asked for so far has been read; for tests.
    void wait();
    // How long a picture without a replacement waits before the pack is searched again (3 s by default).
    void set_search_interval(std::chrono::milliseconds interval) { search_interval_ = interval; }

private:
    struct Entry {
        std::shared_ptr<const OverlayTexture> texture;
        bool looked = false;   // the pack was searched for this key
        bool loading = false;  // the background thread is reading the replacement
        bool dumped = false;   // the original is in the dump folder
        std::chrono::steady_clock::time_point next_search{};
        std::filesystem::file_time_type tried{};  // the pack file as it was when it was last read
    };
    struct Job {
        uint64_t key = 0;
        GameImage image;
        uint8_t palette[256][3]{};
    };
    std::shared_ptr<const OverlayTexture> load(uint64_t key, const GameImage& image, const uint8_t palette[256][3]);
    void work();
    void dump(uint64_t key, const GameImage& image, const uint8_t palette[256][3]);

    std::filesystem::path pack_, dump_;
    bool has_pack_ = false, dumping_ = false;
    size_t dumped_count_ = 0;
    std::unordered_map<uint64_t, Entry> entries_;
    std::chrono::milliseconds search_interval_{3000};
    std::chrono::steady_clock::time_point next_folder_check_{};

    // Shared with the background thread, under mutex_.
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<Job> jobs_;
    std::unordered_map<uint64_t, std::shared_ptr<const OverlayTexture>> finished_;  // null when reading failed
    bool busy_ = false, stop_ = false;
    std::thread worker_;
};

// Names a key as its file name without extension: 16 hexadecimal digits.
std::wstring texture_name(uint64_t key);

}  // namespace nax
