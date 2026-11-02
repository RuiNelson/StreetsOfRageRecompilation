#include "SoRDamageCircles.hpp"

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

constexpr int    kHardestDifficulty = 3;
constexpr m_byte kDeathState        = 0x06u;

// Radii in eighths of a pixel: the ring is 8 pixels across and 1 wide. Alive, the disc stays inside the ring with
// a gap that still shows at 0 health; dead, it is as wide as the ring.
constexpr int kDiameter        = 8;
constexpr int kRingOuter       = kDiameter * 4;
constexpr int kRingInner       = kRingOuter - 8;
constexpr int kDiscFirst       = 4;
constexpr int kDiscLast        = 18;
constexpr int kSamples         = 4; // per pixel, per axis
constexpr int kOpacity         = 11; // of 16: the picture shows through

struct Colour {
    m_byte b = 0;
    m_byte g = 0;
    m_byte r = 0;
};

struct Circle {
    std::int16_t x    = 0; // the object's X and the sprite's top, in biased hardware coordinates
    std::int16_t top  = 0;
    std::int16_t disc = 0;
    Colour       colour;
};

struct Circles {
    std::array<Circle, kObjectCount> circles;
    int                              count = 0;
};

std::mutex circlesMutex;
Circles    shownCircles;

// $9350: $28 and $29 are helpers in the ordinary range, not fighters.
bool isRegularEnemy(m_byte type) {
    return (type >= 0x20u && type <= 0x27u) || type == 0x2Au;
}

// By family, the types of ai-analysis/enemy-ai.md's table.
Colour familyColour(m_byte type) {
    switch (type) {
        case 0x24u: // Signal: yellow
            return {0, 7, 7};
        case 0x25u: // Haku-Ro, the ninja: purple
        case 0x2Au:
            return {7, 1, 5};
        case 0x26u: // Nora: red
            return {1, 1, 7};
        case 0x27u: // Jack: dark grey
            return {3, 3, 3};
        default: // $20-$23, Garcia: blue
            return {7, 3, 1};
    }
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

// How much of the pixel at (x, y) of the circle's box the ring and the disc cover, of 16.
int coverage(int x, int y, int disc) {
    int covered = 0;
    for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
            const int dx       = x * 8 + sx * 2 + 1 - kRingOuter;
            const int dy       = y * 8 + sy * 2 + 1 - kRingOuter;
            const int distance = dx * dx + dy * dy;
            const bool inRing  = distance <= kRingOuter * kRingOuter && distance >= kRingInner * kRingInner;
            if (inRing || distance <= disc * disc)
                ++covered;
        }
    }
    return covered;
}

m_byte blend(m_byte picture, m_byte colour, int weight) {
    return static_cast<m_byte>((picture * (256 - weight) + colour * weight + 128) >> 8);
}

} // namespace

namespace SoRDamageCircles {

void capture(SystemMemory &memory) {
    Circles seen;
    if (SoRQolConfig::current().enemyDamageCircles && inGameplay(memory)) {
        const int bonus = memory.readWord(kDifficulty) == kHardestDifficulty ? 4 : 0;
        for (int i = 0; i < kObjectCount; ++i) {
            const m_long object = kObjectTable + static_cast<m_long>(i) * kObjectSize;
            const m_byte type = memory.readByte(object);
            if (!isRegularEnemy(type))
                continue;
            const m_byte flags   = memory.readByte(object + kFlags);
            const m_byte initial = memory.readByte(object + kInitialHealth);
            const auto   health  = static_cast<std::int16_t>(memory.readWord(object + kHealth));
            const int    full    = initial + bonus;
            const m_byte state   = memory.readByte(object + kState);
            // State 0 or no initial health: not activated yet. Death ($9D16) blinks the sprite with the render
            // flag, and the circle stays on through it.
            if (((flags & 0x01u) != 0u && state != kDeathState) || state == 0u || initial == 0u || health >= full)
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

            // Only a negative health word kills: 0 is alive, the last step before the ring is full.
            const int disc = health < 0 ? kRingOuter : kDiscFirst + (kDiscLast - kDiscFirst) * (full - health) / full;
            seen.circles[static_cast<std::size_t>(seen.count++)] = {static_cast<std::int16_t>(x),
                                                                    static_cast<std::int16_t>(y + topOffset),
                                                                    static_cast<std::int16_t>(disc),
                                                                    familyColour(type)};
        }
    }
    const std::lock_guard lock(circlesMutex);
    shownCircles = seen;
}

void draw(Framebuffer &framebuffer, int width, int height) {
    Circles seen;
    {
        const std::lock_guard lock(circlesMutex);
        seen = shownCircles;
    }
    // The hardware's left edge is $80, widescreen's $60; widescreen hides the picture's first 8 lines.
    const int originX = width > 320 ? 0x60 : 0x80;
    const int originY = 0x80 + (height < 224 ? 8 : 0);
    for (int i = 0; i < seen.count; ++i) {
        const Circle &circle = seen.circles[static_cast<std::size_t>(i)];
        const int     left   = circle.x - originX - kDiameter / 2;
        const int     top    = std::max(0, circle.top - originY - kDiameter); // on screen when the head is not
        for (int y = 0; y < kDiameter; ++y) {
            for (int x = 0; x < kDiameter; ++x) {
                const int px = left + x;
                const int py = top + y;
                if (px < 0 || px >= width || py >= height)
                    continue;
                const int weight = coverage(x, y, circle.disc) * kOpacity;
                if (weight == 0)
                    continue;
                m_byte b = 0;
                m_byte g = 0;
                m_byte r = 0;
                framebuffer.getPixel(px, py, b, g, r);
                framebuffer.setPixel(px,
                                     py,
                                     blend(b, circle.colour.b, weight),
                                     blend(g, circle.colour.g, weight),
                                     blend(r, circle.colour.r, weight));
            }
        }
    }
}

} // namespace SoRDamageCircles
