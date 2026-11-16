// Enhanced textures for the sprites of Fragile Allegiance. The game keeps everything it shows on top of its
// backgrounds (buttons, panels, icons, the pointer) as display objects in one list, and a sprite engine
// (00079E2E to 000809E0) redraws the objects that changed into its picture buffer. After each update the
// native code here reads that list, identifies each sprite frame by its content and hands the replacements
// from the texture pack to the window thread, which draws them over the game picture. The game itself is
// not changed: it still draws every original sprite, which is what the window thread compares against.
#include <windows.h>

#include <cstring>

#include "games/fragile/game.h"

namespace nax::native {

namespace {

constexpr uint32_t kSceneHead = fa::A(0x1242F0);  // display list head; +28h links the objects in drawing order
constexpr uint32_t kScreenBusy = fa::A(0x1242CE);  // re-entry counter of the timer routine at 000807FA
// Drawers the engine stores at +54h of an object. A plain sprite object holds its sheet at +3Ch and its frame
// at +40h. A sprite reference ("IRPS") holds at +3Ch a record with a pointer to the sheet pointer at +14h and
// the frame at +18h, whose bits 16 to 23 give a scale (0 for none).
constexpr uint32_t kDrawSprite = fa::A(0x7E29C);
constexpr uint32_t kDrawReference = fa::A(0x7E270);
constexpr uint32_t kSpriteTag = 0x49525053;       // "SPRI", the first dword of a sprite sheet
constexpr int kMaxObjects = 4096;
constexpr int kMaxSide = 1024;
constexpr int kSettledUpdates = 30;  // updates without a palette change before pictures are dumped

// One sprite frame on screen. data is the guest address of its rows.
struct SpriteRef {
    uint32_t data;
    int width, height, x, y;
    uint64_t key;
};

inline uint16_t r16(const uint8_t* mem, uint32_t a) { uint16_t v; std::memcpy(&v, mem + a, 2); return v; }
inline uint32_t r32(const uint8_t* mem, uint32_t a) { uint32_t v; std::memcpy(&v, mem + a, 4); return v; }

// A frame is stored row by row. Each row is a run of (skip, count) byte pairs, each followed by count pixel
// bytes, and ends with a zero word. The key is an FNV-1a hash of the size and the rows; 0 marks a frame that
// does not fit its size.
uint64_t frame_key(const uint8_t* mem, uint32_t p, int w, int h) {
    uint64_t k = 14695981039346656037ull;
    auto mix = [&k](uint32_t byte) { k = (k ^ byte) * 1099511628211ull; };
    mix(uint32_t(w) & 0xFF); mix(uint32_t(w) >> 8); mix(uint32_t(h) & 0xFF); mix(uint32_t(h) >> 8);
    for (int y = 0; y < h; y++) {
        int x = 0;
        for (;;) {
            const uint16_t word = r16(mem, p);
            p += 2;
            mix(word & 0xFF);
            mix(word >> 8);
            if (word == 0) break;
            const int count = word >> 8;
            x += (word & 0xFF) + count;
            if (x > w) return 0;
            for (int i = 0; i < count; i++) mix(mem[p + i]);
            p += uint32_t(count);
        }
    }
    return k ? k : 1;
}

void decode_frame(const uint8_t* mem, uint32_t p, int w, int h, uint8_t* index, uint8_t* opaque) {
    for (int y = 0; y < h; y++) {
        int x = 0;
        for (;;) {
            const uint16_t word = r16(mem, p);
            p += 2;
            if (word == 0) break;
            const int count = word >> 8;
            x += word & 0xFF;
            for (int i = 0; i < count && x < w; i++, x++) {
                index[size_t(y) * w + x] = mem[p + uint32_t(i)];
                opaque[size_t(y) * w + x] = 1;
            }
            p += uint32_t(count);
        }
    }
}

// The guarded functions below touch guest memory through pointers taken from the game's objects. An object
// that is being changed can hold a stale pointer, which must not end the program, so a fault only ends the
// walk. They hold no objects with destructors, as structured exception handling requires.
int find_sprites(const uint8_t* mem, SpriteRef* out, int capacity) {
    int n = 0;
    __try {
        uint32_t object = r32(mem, kSceneHead + 0x28);
        for (int steps = 0; object != kSceneHead && steps < kMaxObjects && n < capacity; steps++) {
            const uint32_t drawer = r32(mem, object + 0x54);
            uint32_t sheet = 0, frame = 0xFFFF;
            if (drawer == kDrawSprite) {
                sheet = r32(mem, object + 0x3C);
                frame = r16(mem, object + 0x40);
            } else if (drawer == kDrawReference) {
                const uint32_t reference = r32(mem, object + 0x3C);
                const uint32_t selected = r32(mem, reference + 0x18);
                sheet = r32(mem, r32(mem, reference + 0x14));
                if (!(selected & 0xFF0000)) frame = selected & 0xFFFF;
            }
            if (sheet && !(r16(mem, object + 0x24) & 0x80)) {
                if (r32(mem, sheet) == kSpriteTag && frame < r16(mem, sheet + 8)) {
                    const uint32_t entry = sheet + (frame + 1) * 16;
                    const int w = r16(mem, entry + 8), h = r16(mem, entry + 0xA);
                    const uint32_t data = sheet + r32(mem, entry + 0xC);
                    if (w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide) {
                        const uint64_t key = frame_key(mem, data, w, h);
                        if (key)
                            out[n++] = SpriteRef{data, w, h, int(r32(mem, object + 0x30)), int(r32(mem, object + 0x34)), key};
                    }
                }
            }
            object = r32(mem, object + 0x28);
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    }
    return n;
}

bool decode_guarded(const uint8_t* mem, const SpriteRef& s, uint8_t* index, uint8_t* opaque) {
    __try {
        decode_frame(mem, s.data, s.width, s.height, index, opaque);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

}  // namespace

void publish_overlays(Game& g) {
    if (!g.textures.active()) return;
    // A fade changes the brightness of the whole palette, while colour cycling only moves a few entries, so
    // the palette counts as settled when its total brightness has stayed the same for a while.
    uint8_t palette[256][3];
    int brightness = 0;
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++) {
            palette[i][k] = g.display.palette[i][k];
            brightness += palette[i][k];
        }
    if (brightness < g.overlay_brightness - 32 || brightness > g.overlay_brightness + 32) {
        g.overlay_brightness = brightness;
        g.overlay_palette_age = 0;
    } else if (g.overlay_palette_age < kSettledUpdates) {
        g.overlay_palette_age++;
    }
    const bool settled = g.overlay_palette_age >= kSettledUpdates;

    static SpriteRef found[kMaxObjects];
    const int n = find_sprites(g.mem, found, kMaxObjects);
    std::vector<Overlay> list;
    for (int i = 0; i < n; i++) {
        const SpriteRef& s = found[i];
        auto decode = [&](GameImage* image) {
            image->width = s.width;
            image->height = s.height;
            image->index.assign(size_t(s.width) * s.height, 0);
            image->opaque.assign(size_t(s.width) * s.height, 0);
            return decode_guarded(g.mem, s, image->index.data(), image->opaque.data());
        };
        if (auto texture = g.textures.get(s.key, decode, palette, settled)) list.push_back(Overlay{texture, s.x, s.y});
    }
    g.display.set_overlays(std::move(list));
}

// Replaces "dec byte ptr [001242CEh]" at the end of the sprite engine's timer routine (00080986), which runs
// the update and then copies the changed lines of its picture buffer to the screen. Only now does the screen
// match the display list, so the list of enhanced sprites is brought up to date here.
void scene_shown(Machine& m) {
    Game& g = game(m);
    g.wr8(kScreenBusy, uint8_t(g.rd8(kScreenBusy) - 1));
    publish_overlays(g);
}

}  // namespace nax::native
