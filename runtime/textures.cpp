// Texture packs and the dump of original pictures.
#include "runtime/textures.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "runtime/image.h"

namespace nax {

namespace {

uint8_t expand(uint8_t six) { return uint8_t((six & 63) << 2 | (six & 63) >> 4); }

bool read_palette(const std::filesystem::path& file, uint8_t out[256][3]) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    const bool ok = std::fread(out, 1, 768, f) == 768;
    std::fclose(f);
    return ok;
}

bool write_file(const std::filesystem::path& file, const void* data, size_t bytes) {
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    FILE* f = _wfopen(temporary.c_str(), L"wb");
    if (!f) return false;
    const bool written = std::fwrite(data, 1, bytes, f) == bytes;
    const bool closed = std::fclose(f) == 0;
    std::error_code ec;
    if (written && closed) std::filesystem::rename(temporary, file, ec);
    if (!written || !closed || ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace

std::wstring texture_name(uint64_t key) {
    wchar_t text[17];
    swprintf(text, 17, L"%016llx", static_cast<unsigned long long>(key));
    return text;
}

void TexturePack::open(const std::filesystem::path& pack, const std::filesystem::path& dump, bool dumping) {
    pack_ = pack;
    dump_ = dump;
    dumping_ = dumping;
    std::error_code ec;
    has_pack_ = std::filesystem::is_directory(pack_, ec);
    next_folder_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (dumping_) std::filesystem::create_directories(dump_, ec);
    entries_.clear();
}

bool TexturePack::active() {
    if (!has_pack_ && !pack_.empty() && std::chrono::steady_clock::now() >= next_folder_check_) {
        std::error_code ec;
        has_pack_ = std::filesystem::is_directory(pack_, ec);
        next_folder_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    return has_pack_ || dumping_;
}

TexturePack::~TexturePack() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void TexturePack::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        auto texture = load(job.key, job.image, job.palette);
        lock.lock();
        finished_[job.key] = std::move(texture);
        busy_ = false;
        if (jobs_.empty()) idle_.notify_all();
    }
}

void TexturePack::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

std::shared_ptr<const OverlayTexture> TexturePack::get(uint64_t key, const std::function<bool(GameImage*)>& decode,
                                                       const uint8_t palette[256][3], bool settled) {
    Entry& e = entries_[key];
    if (e.loading) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto done = finished_.find(key);
        if (done == finished_.end()) return e.texture;  // the previous version, if any, stays until then
        if (done->second) e.texture = std::move(done->second);
        e.loading = false;
        finished_.erase(done);
    }
    std::error_code ec;
    const auto now = std::chrono::steady_clock::now();
    if (!e.looked || now >= e.next_search) {
        if (!e.looked && dumping_ && std::filesystem::exists(dump_ / (texture_name(key) + L".png"), ec)) e.dumped = true;
        e.looked = true;
        e.next_search = now + search_interval_;
        // The pack file is read when it is new to this key or has changed since it was last read, for example
        // when the redraw tool replaced the enlarged picture with a repainted one.
        const std::filesystem::path file = pack_ / (texture_name(key) + L".png");
        const auto stamp = std::filesystem::last_write_time(file, ec);
        if (has_pack_ && !ec && stamp != e.tried) {
            Job job;
            job.key = key;
            if (decode(&job.image)) {
                // A decode that fails (the game was changing the sprite) is tried again at the next search.
                e.tried = stamp;
                if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
                std::memcpy(job.palette, palette, sizeof(job.palette));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    jobs_.push_back(std::move(job));
                }
                wake_.notify_one();
                e.loading = true;
                return e.texture;  // a picture in the pack is never dumped
            }
        }
    }
    if (dumping_ && !e.texture && !e.dumped && settled) {
        GameImage image;
        if (decode(&image)) dump(key, image, palette);
        e.dumped = true;
    }
    return e.texture;
}

std::shared_ptr<const OverlayTexture> TexturePack::load(uint64_t key, const GameImage& image, const uint8_t palette[256][3]) {
    auto t = std::make_shared<OverlayTexture>();
    std::vector<uint32_t> full;
    int fw = 0, fh = 0;
    if (!load_png(pack_ / (texture_name(key) + L".png"), &full, &fw, &fh)) return nullptr;
    // The margin is cropped off in proportion to the enlargement.
    const int mx = int(double(kTextureMargin) * fw / (image.width + 2 * kTextureMargin) + 0.5);
    const int my = int(double(kTextureMargin) * fh / (image.height + 2 * kTextureMargin) + 0.5);
    t->hd_width = fw - 2 * mx;
    t->hd_height = fh - 2 * my;
    if (t->hd_width <= 0 || t->hd_height <= 0) return nullptr;
    t->hd.resize(size_t(t->hd_width) * t->hd_height);
    for (int y = 0; y < t->hd_height; y++)
        std::copy_n(&full[size_t(y + my) * fw + mx], t->hd_width, &t->hd[size_t(y) * t->hd_width]);
    t->key = key;
    static std::atomic<uint64_t> versions{0};
    t->version = ++versions;
    t->width = image.width;
    t->height = image.height;
    t->index = image.index;
    t->opaque = image.opaque;
    // Without a palette file the picture counts as made under the palette shown now.
    if (!read_palette(pack_ / (texture_name(key) + L".pal"), t->reference))
        for (int i = 0; i < 256; i++)
            for (int k = 0; k < 3; k++) t->reference[i][k] = expand(palette[i][k]);
    uint32_t counts[256]{};
    for (size_t i = 0; i < t->index.size(); i++)
        if (t->opaque[i]) counts[t->index[i]]++;
    for (int i = 0; i < 256; i++)
        if (counts[i]) t->used.emplace_back(uint8_t(i), counts[i]);
    return t;
}

void TexturePack::dump(uint64_t key, const GameImage& image, const uint8_t palette[256][3]) {
    const int m = kTextureMargin, w = image.width + 2 * m, h = image.height + 2 * m;
    std::vector<uint32_t> rgba(size_t(w) * h, 0);
    std::vector<uint8_t> filled(size_t(w) * h, 0);
    for (int y = 0; y < image.height; y++)
        for (int x = 0; x < image.width; x++) {
            const size_t from = size_t(y) * image.width + x, to = size_t(y + m) * w + x + m;
            if (!image.opaque[from]) continue;
            const uint8_t* c = palette[image.index[from]];
            rgba[to] = expand(c[0]) | uint32_t(expand(c[1])) << 8 | uint32_t(expand(c[2])) << 16 | 0xFF000000u;
            filled[to] = 1;
        }
    // Transparent pixels, the margin included, take the average colour of their drawn neighbours with alpha 0.
    // An upscaler then blends the edges of the picture towards its own colours instead of towards black.
    for (int pass = 0; pass < m; pass++) {
        std::vector<uint8_t> next = filled;
        std::vector<uint32_t> out = rgba;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t i = size_t(y) * w + x;
                if (filled[i]) continue;
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t j = size_t(ny) * w + nx;
                        if (!filled[j]) continue;
                        r += rgba[j] & 0xFF;
                        g += (rgba[j] >> 8) & 0xFF;
                        b += (rgba[j] >> 16) & 0xFF;
                        n++;
                    }
                if (!n) continue;
                out[i] = (r / n) | (g / n) << 8 | (b / n) << 16;
                next[i] = 1;
            }
        rgba.swap(out);
        filled.swap(next);
    }
    uint8_t full[256][3];
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++) full[i][k] = expand(palette[i][k]);
    const std::wstring name = texture_name(key);
    // The PNG is written last: its presence marks the picture as dumped.
    if (write_file(dump_ / (name + L".pal"), full, sizeof(full)) && save_png(dump_ / (name + L".png"), rgba.data(), w, h))
        dumped_count_++;
}

}  // namespace nax
