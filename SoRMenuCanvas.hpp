#pragma once

#include "data_types.hpp"
#include "system/memory/SystemMemory.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// What the host's menus on the OPTIONS screen share: the OPTIONS font, a screen
// composed in a buffer and written to plane A where it changed, and held
// directions that repeat. See SoRControlsMenu and SoRVideoMenu.
namespace SoRMenuCanvas {

inline constexpr m_long kVdpData    = 0x00C00000u;
inline constexpr m_long kVdpControl = 0x00C00004u;
inline constexpr m_long kPlaneA     = 0xC000u; // the OPTIONS screen's plane A (its $45A40003-style commands)
inline constexpr m_word kGreyText   = 0x2000u; // palette line 1; line 0 is the white of the selected entry
inline constexpr m_long kPlaySe     = 0xFFFFF00Au;
inline constexpr m_byte kSoundMove  = 0xB9u; // options_row_nav's cursor sound
inline constexpr m_byte kSoundPick  = 0xBAu; // the menus' confirm sound

inline constexpr int kColumns = 64;
inline constexpr int kRows    = 28;

inline constexpr int kRepeatDelay = 20;
inline constexpr int kRepeatRate  = 6;

// The menu font loaded for the title and OPTIONS screens: a text byte is the
// tile index (the ROM's own strings use these codes, e.g. "A:SPECIAL ATTACK"
// at $1C9FC).
inline std::optional<m_byte> glyph(char c) {
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return static_cast<m_byte>(c - 'A' + 0x01);
    if (c >= '0' && c <= '9')
        return static_cast<m_byte>(c - '0' + 0x1B);
    switch (c) {
        case ' ':
            return 0x00;
        case ':':
            return 0x26;
        case '>':
            return 0x27;
        case '.':
            return 0x29;
        case ',':
            return 0x2A;
        case '\'':
            return 0x2C;
        case '"':
            return 0x2D;
        case '?':
            return 0x2E;
        case '!':
            return 0x2F;
        default:
            return std::nullopt;
    }
}

inline m_long vramWriteCommand(m_long address) {
    return 0x40000000u | ((address & 0x3FFFu) << 16) | ((address >> 14) & 0x3u);
}

inline m_long cellAddress(int column, int row) {
    return kPlaneA + static_cast<m_long>(row * kColumns + column) * 2u;
}

struct Canvas {
    std::array<m_word, kColumns * kRows> cells{};

    void text(int column, int row, std::string_view value, bool highlighted) {
        if (row < 0 || row >= kRows)
            return;
        for (const char c : value) {
            if (column >= 0 && column < kColumns)
                cells[static_cast<std::size_t>(row * kColumns + column)] =
                    static_cast<m_word>(glyph(c).value_or(0x00) | (highlighted ? 0u : kGreyText));
            ++column;
        }
    }
};

/// The screen as it stands in plane A.
struct Shown {
    Canvas canvas;
    bool   valid = false;
};

inline void flush(SystemMemory &memory, Shown &shown, const Canvas &canvas) {
    for (int row = 0; row < kRows; ++row) {
        int column = 0;
        while (column < kColumns) {
            const auto index = static_cast<std::size_t>(row * kColumns + column);
            if (shown.valid && canvas.cells[index] == shown.canvas.cells[index]) {
                ++column;
                continue;
            }
            memory.writeLong(kVdpControl, vramWriteCommand(cellAddress(column, row)));
            while (column < kColumns) {
                const auto i = static_cast<std::size_t>(row * kColumns + column);
                if (shown.valid && canvas.cells[i] == shown.canvas.cells[i])
                    break;
                memory.writeWord(kVdpData, canvas.cells[i]);
                ++column;
            }
        }
    }
    shown.canvas = canvas;
    shown.valid  = true;
}

inline void drawText(SystemMemory &memory, int column, int row, std::string_view text, bool highlighted) {
    memory.writeLong(kVdpControl, vramWriteCommand(cellAddress(column, row)));
    for (const char c : text)
        memory.writeWord(kVdpData, static_cast<m_word>(glyph(c).value_or(0x00) | (highlighted ? 0u : kGreyText)));
}

inline void playSound(SystemMemory &memory, m_byte id) {
    memory.writeByte(kPlaySe, id);
}

struct Repeat {
    int held = 0;

    // A press, then repeats while held.
    bool step(bool down, bool repeats) {
        if (!down) {
            held = 0;
            return false;
        }
        ++held;
        return held == 1 || (repeats && held >= kRepeatDelay && (held - kRepeatDelay) % kRepeatRate == 0);
    }
};

} // namespace SoRMenuCanvas
