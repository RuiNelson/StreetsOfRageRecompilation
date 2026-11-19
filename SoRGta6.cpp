#include "SoRGta6.hpp"

#include "SoRDecompress.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

namespace SoRGta6 {
namespace {

constexpr m_long kGameState  = 0xFFFFFF00u;
constexpr m_long kLevel      = 0xFFFFFF02u;
constexpr m_long kPlayerMode = 0xFFFFFF18u;
constexpr m_long kScoreLong  = 0xFFFFFF08u; // score_p1; score_p2 8 further on
constexpr m_long kCamX       = 0xFFFFE002u;
constexpr m_long kCameraY    = 0xFFFFE00Eu;
constexpr m_long kP1Object   = 0xFFFFB800u;

constexpr m_word kStateModeMenu        = 0x10u;
constexpr m_word kStateCharacterSelect = 0x20u;
constexpr m_byte kModeOnePlayer        = 1;

constexpr m_long kType    = 0x00u;
constexpr m_long kFlags   = 0x01u; // bit 0: not shown (the respawn blink)
constexpr m_long kX       = 0x10u;
constexpr m_long kLane    = 0x14u;
constexpr m_long kHeight  = 0x18u;
constexpr m_long kAction  = 0x30u; // bit 0: facing left
constexpr m_long kHealth  = 0x32u;
constexpr m_long kAttack  = 0x64u;
constexpr m_long kShadowOwner = 0x3Eu; // a type-$4A shadow's source ($B132)

constexpr m_byte kPlayerType = 0x01u;
constexpr m_byte kShadowType = 0x4Au;

// The car's touch: its body less the tapered ends, as deep in the street as a fighter's reach.
constexpr int kStrikeHalfWidth = 72;
constexpr int kStrikeHalfDepth = 12;
constexpr int kStrikeTop       = -48;
constexpr int kStrikeBottom    = 4;

// The police special's vehicle (type $05, $599E police_vehicle_initialize): art cue 0 of the Nemesis cue table at
// $8672, at VRAM tile $25C; animation set $70A78, body animation 0, wheels animation 2 (its two frames swap the two
// wheel drawings, so the wheels turn in place), the wheels 4 lanes (2 lines) in front ($5A12
// police_vehicle_arrive_state); palette list $5A78, on line 1. Both face right, as the special drives the car; facing
// left is the same frames mirrored here. Animations 1 and 3 are not used: 3 mirrors only every other frame, and
// the wheels jumped back and forth with it (user report).
constexpr m_long kNemesisCueTable = 0x00008672u;
constexpr m_long kAnimationSet    = 0x00070A78u;
constexpr m_long kPaletteList     = 0x00005A78u;
constexpr int    kTileBase        = 0x25C;
constexpr int    kBodyAnimation   = 0;
constexpr m_word kRound7          = 6; // the car faces left on rounds 7 and 8 (user; round 8 scrolls left)
constexpr m_word kRound8          = 7;
constexpr int    kWheelAnimation  = 2;
constexpr int    kWheelFrames     = 2;
constexpr int    kWheelFrameTime  = 3;
constexpr int    kWheelDrop       = 2;

struct Sprite {
    int                 left   = 0; // relative to the object's origin
    int                 top    = 0;
    int                 width  = 0;
    int                 height = 0;
    std::vector<m_byte> pixels; // palette entry (line * 16 + colour), 0 transparent
};

struct Art {
    std::array<Sprite, 2>                           body;   // facing right, left
    std::array<std::array<Sprite, kWheelFrames>, 2> wheels; // facing, frame
    std::array<m_word, 64>                          palette{};
};

struct Car {
    bool         shown = false;
    std::int16_t x     = 0; // biased hardware coordinates of the origin
    std::int16_t y     = 0;
    bool         left  = false;
    int          wheel = 0;
};

struct State {
    bool         active = false;
    std::int16_t lastX  = 0;
    std::int16_t lastLane = 0;
    int          wheelTime  = 0;
    int          wheelFrame = 0;
};

State &state() {
    static State instance;
    return instance;
}

std::mutex                 shownMutex;
Car                        shownCar;
std::shared_ptr<const Art> shownArt;

std::int16_t readSigned(SystemMemory &memory, m_long address) {
    return static_cast<std::int16_t>(memory.readWord(address));
}

int signedByte(SystemMemory &memory, m_long address) {
    return static_cast<std::int8_t>(memory.readByte(address));
}

// One frame of the animation set, as $AF46 draws it: five-byte pieces after a five-byte header, the first piece in
// front; a frame offset with bit 15 is the frame mirrored (the flip bit toggled, X moved by the piece's width).
Sprite renderFrame(SystemMemory &memory, const std::vector<m_byte> &tiles, int animation, int frame, bool flip) {
    const m_long record = kAnimationSet + memory.readWord(kAnimationSet + static_cast<m_long>(animation) * 2u);
    const m_word entry  = memory.readWord(record + 2u + static_cast<m_long>(frame) * 2u);
    const m_long base   = record + 2u + ((entry - 2u) & 0x7FFFu);
    const bool   mirror = ((entry & 0x8000u) != 0) != flip;
    const m_byte count  = memory.readByte(base);
    Sprite       sprite;
    if ((count & 0x80u) != 0)
        return sprite;

    struct Piece {
        int    x, y, columns, rows;
        m_word attributes;
    };
    std::vector<Piece> pieces;
    int                left = 0x7FFF, top = 0x7FFF, right = -0x7FFF, bottom = -0x7FFF;
    for (int i = 0; i <= count; ++i) {
        const m_long at   = base + 5u + static_cast<m_long>(i) * 5u;
        const m_byte size = memory.readByte(at + 1u);
        Piece piece{signedByte(memory, at + 4u), signedByte(memory, at), ((size >> 2) & 3) + 1, (size & 3) + 1,
                    memory.readWord(at + 2u)};
        if (mirror) {
            piece.x = -piece.x - piece.columns * 8;
            piece.attributes ^= 0x0800u;
        }
        pieces.push_back(piece);
        left   = std::min(left, piece.x);
        top    = std::min(top, piece.y);
        right  = std::max(right, piece.x + piece.columns * 8);
        bottom = std::max(bottom, piece.y + piece.rows * 8);
    }
    sprite.left   = left;
    sprite.top    = top;
    sprite.width  = right - left;
    sprite.height = bottom - top;
    sprite.pixels.assign(static_cast<std::size_t>(sprite.width * sprite.height), 0);

    const int tileCount = static_cast<int>(tiles.size() / 32u);
    for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) { // the first piece ends on top
        const Piece &piece = *it;
        const bool   flipX = (piece.attributes & 0x0800u) != 0;
        const bool   flipY = (piece.attributes & 0x1000u) != 0;
        const int    line  = (piece.attributes >> 13) & 3;
        const int    first = (((piece.attributes & 0x07FF) + kTileBase) & 0x07FF) - kTileBase;
        for (int column = 0; column < piece.columns; ++column) {
            for (int row = 0; row < piece.rows; ++row) {
                const int tile = first + column * piece.rows + row; // tiles run down each column
                if (tile < 0 || tile >= tileCount)
                    continue;
                const int cellX = (flipX ? piece.columns - 1 - column : column) * 8;
                const int cellY = (flipY ? piece.rows - 1 - row : row) * 8;
                for (int py = 0; py < 8; ++py) {
                    for (int px = 0; px < 8; ++px) {
                        const m_byte pair   = tiles[static_cast<std::size_t>(tile * 32 + py * 4 + px / 2)];
                        const m_byte colour = (px & 1) == 0 ? pair >> 4 : pair & 0x0Fu;
                        if (colour == 0)
                            continue;
                        const int x = piece.x - left + cellX + (flipX ? 7 - px : px);
                        const int y = piece.y - top + cellY + (flipY ? 7 - py : py);
                        sprite.pixels[static_cast<std::size_t>(y * sprite.width + x)] =
                            static_cast<m_byte>(line * 16 + colour);
                    }
                }
            }
        }
    }
    return sprite;
}

std::shared_ptr<const Art> buildArt(SystemMemory &memory) {
    auto art = std::make_shared<Art>();
    // The cue's first record: its source long and VRAM destination word, after a count word ($8454).
    const m_long cue    = kNemesisCueTable + memory.readWord(kNemesisCueTable);
    const m_long source = memory.readLong(cue + 2u);
    std::vector<m_byte> tiles;
    try {
        tiles = SoRDecompress::nemesis(memory, source);
    } catch (const std::exception &) {
        return art; // nothing to draw
    }
    for (int facing = 0; facing < 2; ++facing) {
        const bool left = facing == 1;
        art->body[static_cast<std::size_t>(facing)] = renderFrame(memory, tiles, kBodyAnimation, 0, left);
        for (int frame = 0; frame < kWheelFrames; ++frame)
            art->wheels[static_cast<std::size_t>(facing)][static_cast<std::size_t>(frame)] =
                renderFrame(memory, tiles, kWheelAnimation, frame, left);
    }
    // $1053E: each word a colour (& $EEE) and its entry (bit 4, the top nibble, bit 8 as bit 5); bit 0 ends it.
    m_long at = kPaletteList;
    for (int guard = 0; guard < 64; ++guard, at += 2u) {
        const m_word value = memory.readWord(at);
        const int    index = (value & 0x10) | ((value >> 12) & 0x0F) | ((value & 0x100) >> 3);
        art->palette[static_cast<std::size_t>(index)] = static_cast<m_word>(value & 0x0EEEu);
        if ((value & 1u) != 0)
            break;
    }
    return art;
}

bool inGameplay(SystemMemory &memory) {
    const m_word state = memory.readWord(kGameState);
    return state == 0x28u || state == 0x2Au || (state >= 0x14u && state < 0x1Cu);
}

void paint(Framebuffer &framebuffer, int width, int height, const Art &art, const Sprite &sprite, int x, int y) {
    for (int row = 0; row < sprite.height; ++row) {
        const int py = y + sprite.top + row;
        if (py < 0 || py >= height)
            continue;
        for (int column = 0; column < sprite.width; ++column) {
            const int px = x + sprite.left + column;
            if (px < 0 || px >= width)
                continue;
            const m_byte entry = sprite.pixels[static_cast<std::size_t>(row * sprite.width + column)];
            if (entry == 0)
                continue;
            const m_word colour = art.palette[entry];
            framebuffer.setPixel(px,
                                 py,
                                 static_cast<m_byte>((colour >> 9) & 7u),
                                 static_cast<m_byte>((colour >> 5) & 7u),
                                 static_cast<m_byte>((colour >> 1) & 7u));
        }
    }
}

} // namespace

void begin(SystemMemory &memory) {
    State &s = state();
    s        = State{};
    s.active = true;
    for (m_long player = 0; player < 2; ++player)
        memory.writeLong(kScoreLong + 8u * player, 0);
    memory.writeByte(kPlayerMode, kModeOnePlayer);
    memory.writeWord(kGameState, kStateCharacterSelect);
}

bool active() {
    return state().active;
}

void onFrame(SystemMemory &memory) {
    State &s = state();
    if (s.active && memory.readWord(kGameState) < kStateModeMenu)
        s.active = false; // GAME OVER's high scores, the title after the ending, SAVE AND QUIT, a reset
}

bool isCar(SystemMemory &memory, m_long object) {
    return state().active && (object & 0x00FFFFFFu) == (kP1Object & 0x00FFFFFFu) &&
           memory.readByte(kP1Object + kType) == kPlayerType;
}

bool carStrikes(SystemMemory &memory, m_long object) {
    if (!isCar(memory, object) || readSigned(memory, kP1Object + kHealth) <= 0)
        return false;
    // Standing ($02), idle ($04) or driving ($06-$0F): not hit, falling, dying or held.
    const m_byte action = static_cast<m_byte>(memory.readByte(kP1Object + kAction) & 0xFEu);
    return action >= 0x02u && action <= 0x0Eu;
}

void writeStrikeBox(SystemMemory &memory, m_long object) {
    const int x      = readSigned(memory, object + kX);
    const int lane   = readSigned(memory, object + kLane);
    const int height = readSigned(memory, object + kHeight);
    const int box[6] = {x - kStrikeHalfWidth, x + kStrikeHalfWidth, lane - kStrikeHalfDepth,
                        lane + kStrikeHalfDepth, height + kStrikeTop, height + kStrikeBottom};
    for (m_long i = 0; i < 6; ++i)
        memory.writeWord(object + kAttack + i * 2u, static_cast<m_word>(box[i]));
    if (memory.readWord(object + kAttack) == 0) // the first word is also "there is a box"
        memory.writeWord(object + kAttack, 1);
}

bool hidesObject(SystemMemory &memory, m_long object) {
    if (!isCar(memory, kP1Object))
        return false;
    if ((object & 0x00FFFFFFu) == (kP1Object & 0x00FFFFFFu))
        return true;
    return memory.readByte(object + kType) == kShadowType &&
           memory.readWord(object + kShadowOwner) == static_cast<m_word>(kP1Object & 0xFFFFu);
}

void capture(SystemMemory &memory) {
    State &s = state();
    Car    car;
    std::shared_ptr<const Art> art;
    if (s.active && inGameplay(memory) && isCar(memory, kP1Object)) {
        {
            const std::lock_guard lock(shownMutex);
            art = shownArt;
        }
        if (!art)
            art = buildArt(memory);

        const std::int16_t x    = readSigned(memory, kP1Object + kX);
        const std::int16_t lane = readSigned(memory, kP1Object + kLane);
        if (x != s.lastX || lane != s.lastLane) { // the wheels turn while it moves
            if (++s.wheelTime >= kWheelFrameTime) {
                s.wheelTime  = 0;
                s.wheelFrame = (s.wheelFrame + 1) % kWheelFrames;
            }
        }
        s.lastX    = x;
        s.lastLane = lane;

        // As $AF46: lane/2 + height, biased by $80, in the camera.
        car.shown = (memory.readByte(kP1Object + kFlags) & 0x01u) == 0;
        car.x     = static_cast<std::int16_t>(x + 0x80 - readSigned(memory, kCamX));
        car.y     = static_cast<std::int16_t>((lane >> 1) + readSigned(memory, kP1Object + kHeight) + 0x80 -
                                          readSigned(memory, kCameraY));
        // The car never turns round (user): driving back is reversing. It faces right, or left on rounds 7 and 8.
        const m_word level = memory.readWord(kLevel);
        car.left  = level == kRound7 || level == kRound8;
        car.wheel = s.wheelFrame;
    }
    const std::lock_guard lock(shownMutex);
    shownCar = car;
    if (art)
        shownArt = art;
}

void draw(Framebuffer &framebuffer, int width, int height) {
    Car                        car;
    std::shared_ptr<const Art> art;
    {
        const std::lock_guard lock(shownMutex);
        car = shownCar;
        art = shownArt;
    }
    if (!car.shown || !art)
        return;
    // The hardware's left edge is $80, widescreen's $60; widescreen hides the picture's first 8 lines.
    const int    x      = car.x - (width > 320 ? 0x60 : 0x80);
    const int    y      = car.y - (0x80 + (height < 224 ? 8 : 0));
    const std::size_t facing = car.left ? 1 : 0;
    paint(framebuffer, width, height, *art, art->body[facing], x, y);
    paint(framebuffer, width, height, *art, art->wheels[facing][static_cast<std::size_t>(car.wheel)], x,
          y + kWheelDrop);
}

} // namespace SoRGta6
