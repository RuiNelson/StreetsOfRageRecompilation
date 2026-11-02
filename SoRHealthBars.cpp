#include "SoRHealthBars.hpp"

#include "SoRQolConfig.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace {

constexpr m_long kObjectTable = 0xFFFFB900u;
constexpr int    kObjectCount = 66;
constexpr m_long kObjectSize  = 0x80u;
constexpr m_long kCamX        = 0xFFFFE002u;
constexpr m_long kCameraY     = 0xFFFFE00Eu;
constexpr m_long kGameState   = 0xFFFFFF00u;
constexpr m_long kDifficulty  = 0xFFFFFFC6u;

constexpr m_long kFlags         = 0x01u; // bit 0 not rendered, bit 1 screen coordinates
constexpr m_long kAnimationSet  = 0x04u;
constexpr m_long kAnimation     = 0x08u;
constexpr m_long kFrame         = 0x0Au;
constexpr m_long kX             = 0x10u;
constexpr m_long kLane          = 0x14u;
constexpr m_long kHeight        = 0x18u;
constexpr m_long kState         = 0x30u;
constexpr m_long kHealth        = 0x32u;
constexpr m_long kInitialHealth = 0x38u; // set once, by $93CE, without the hardest difficulty's +4

constexpr int kBarWidth  = 16;
constexpr int kBarHeight = 4;

constexpr int kHardestDifficulty = 3;

struct Bar {
    std::int16_t x      = 0; // the object's X and the sprite's top, in biased hardware coordinates
    std::int16_t top    = 0;
    std::int16_t filled = 0;
};

struct Bars {
    std::array<Bar, kObjectCount> bars;
    int                           count = 0;
};

std::mutex barsMutex;
Bars       shownBars;

// $9350: $28 and $29 are helpers in the ordinary range, not fighters.
bool isRegularEnemy(m_byte type) {
    return (type >= 0x20u && type <= 0x27u) || type == 0x2Au;
}

bool inGameplay(SystemMemory &memory) {
    const m_word state = memory.readWord(kGameState);
    return state == 0x28u || state == 0x2Au || (state >= 0x14u && state < 0x1Cu);
}

// The topmost piece of the object's current frame, relative to its Y, walking the mapping as
// $AF46 does (both of its headers are five bytes, and a piece starts with its Y). False with no pieces.
bool spriteTopOffset(SystemMemory &memory, m_long object, int &topOffset) {
    const m_long set = memory.readLong(object + kAnimationSet);
    if (set == 0u)
        return false;
    const auto animation = static_cast<std::int16_t>(memory.readWord(object + kAnimation));
    const auto offset    = static_cast<std::int16_t>(memory.readWord(set + static_cast<std::int32_t>(animation)));
    const m_long frames  = static_cast<m_long>(static_cast<std::int64_t>(set) + offset) + 2u;
    const m_long entry   = memory.readWord(frames + memory.readByte(object + kFrame) * 2u);
    const m_long frame   = frames + ((entry - 2u) & 0x7FFFu);

    const m_byte count = memory.readByte(frame);
    if ((count & 0x80u) != 0u)
        return false;
    int top = 0x7FFF;
    for (int piece = 0; piece <= count; ++piece)
        top = std::min<int>(top, static_cast<std::int8_t>(memory.readByte(frame + 5u + static_cast<m_long>(piece) * 5u)));
    topOffset = top;
    return true;
}

} // namespace

namespace SoRHealthBars {

void capture(SystemMemory &memory) {
    Bars seen;
    if (SoRQolConfig::current().enemyHealthBars && inGameplay(memory)) {
        const int bonus = memory.readWord(kDifficulty) == kHardestDifficulty ? 4 : 0;
        for (int i = 0; i < kObjectCount; ++i) {
            const m_long object = kObjectTable + static_cast<m_long>(i) * kObjectSize;
            if (!isRegularEnemy(memory.readByte(object)))
                continue;
            const m_byte flags  = memory.readByte(object + kFlags);
            const m_byte initial = memory.readByte(object + kInitialHealth);
            const auto   health = static_cast<std::int16_t>(memory.readWord(object + kHealth));
            // State 0 or no initial health: not activated yet. Negative health or state $0600: dead, or being swept.
            const m_byte state = memory.readByte(object + kState);
            if ((flags & 0x01u) != 0u || state == 0u || state == 0x06u || initial == 0u || health < 0)
                continue;
            int topOffset = 0;
            if (!spriteTopOffset(memory, object, topOffset))
                continue;

            // As $AF46: lane/2 + height, both biased by $80, in the camera unless the object is in screen coordinates.
            int x = static_cast<std::int16_t>(memory.readWord(object + kX)) + 0x80;
            int y = (static_cast<std::int16_t>(memory.readWord(object + kLane)) >> 1) +
                    static_cast<std::int16_t>(memory.readWord(object + kHeight)) + 0x80;
            if ((flags & 0x02u) == 0u) {
                x -= static_cast<std::int16_t>(memory.readWord(kCamX));
                y -= static_cast<std::int16_t>(memory.readWord(kCameraY));
            }

            const int full   = initial + bonus;
            const int filled = std::min<int>(kBarWidth, (std::min<int>(health, full) * kBarWidth + full - 1) / full);
            seen.bars[static_cast<std::size_t>(seen.count++)] = {static_cast<std::int16_t>(x),
                                                                 static_cast<std::int16_t>(y + topOffset),
                                                                 static_cast<std::int16_t>(filled)};
        }
    }
    const std::lock_guard lock(barsMutex);
    shownBars = seen;
}

void draw(Framebuffer &framebuffer, int width, int height) {
    Bars seen;
    {
        const std::lock_guard lock(barsMutex);
        seen = shownBars;
    }
    // The hardware's left edge is $80, widescreen's $60; widescreen hides the picture's first 8 lines.
    const int originX = width > 320 ? 0x60 : 0x80;
    const int originY = 0x80 + (height < 224 ? 8 : 0);
    for (int i = 0; i < seen.count; ++i) {
        const Bar &bar  = seen.bars[static_cast<std::size_t>(i)];
        const int  left = bar.x - originX - kBarWidth / 2;
        const int  top  = std::max(0, bar.top - originY - kBarHeight); // on screen when the head is not
        for (int y = top; y < std::min(height, top + kBarHeight); ++y) {
            for (int x = std::max(0, left); x < std::min(width, left + kBarWidth); ++x) {
                m_byte b = 0;
                m_byte g = 0;
                m_byte r = 0;
                framebuffer.getPixel(x, y, b, g, r);
                // The picture shows through: a quarter under the yellow, half where the health is gone.
                if (x - left < bar.filled)
                    framebuffer.setPixel(x, y, b >> 2, (g + 3 * 7 + 2) >> 2, (r + 3 * 7 + 2) >> 2);
                else
                    framebuffer.setPixel(x, y, b >> 1, g >> 1, r >> 1);
            }
        }
    }
}

} // namespace SoRHealthBars
