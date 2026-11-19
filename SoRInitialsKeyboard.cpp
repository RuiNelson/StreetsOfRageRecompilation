#include "SoRInitialsKeyboard.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace SoRInitialsKeyboard {
namespace {

using namespace SoRMenuCanvas;

constexpr std::array<std::string_view, 3> kLetterRows = {"ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ"};
enum class Special { Space, Delete, Ok, Cancel };
constexpr std::array<Special, 4> kSpecials    = {Special::Space, Special::Delete, Special::Ok, Special::Cancel};
constexpr std::array<int, 4>     kSpecialCols = {8, 15, 21, 26};
constexpr int                    kKeyRows     = 4;
constexpr int                    kKeyColumn   = 8;
constexpr int                    kKeyStride   = 3;
constexpr int                    kFirstKeyRow = 9;
constexpr int                    kInitials    = 3;
constexpr int                    kTitleRow    = 3;
constexpr int                    kLabelColumn = 4;
constexpr int                    kHelpRow     = 22;

std::string_view specialLabel(Special key) {
    switch (key) {
        case Special::Space:
            return "SPACE";
        case Special::Delete:
            return "DEL";
        case Special::Ok:
            return "OK";
        case Special::Cancel:
            return "CANCEL";
    }
    return "";
}

int keysInRow(int keyRow) {
    return keyRow < 3 ? static_cast<int>(kLetterRows[static_cast<std::size_t>(keyRow)].size()) : static_cast<int>(kSpecials.size());
}

} // namespace

Keyboard open(std::string title, std::string text) {
    Keyboard keyboard;
    keyboard.title = std::move(title);
    keyboard.text  = std::move(text);
    return keyboard;
}

Result step(Keyboard &k, SystemMemory &memory, bool up, bool down, bool left, bool right, bool confirm, bool back) {
    if (back) {
        playSound(memory, kSoundPick);
        return Result::Cancel;
    }
    if (up || down) {
        k.keyRow = (k.keyRow + (up ? -1 : 1) + kKeyRows) % kKeyRows;
        k.keyCol = std::min(k.keyCol, keysInRow(k.keyRow) - 1);
        playSound(memory, kSoundMove);
    } else if (left || right) {
        const int count = keysInRow(k.keyRow);
        k.keyCol        = (k.keyCol + (left ? -1 : 1) + count) % count;
        playSound(memory, kSoundMove);
    } else if (confirm) {
        playSound(memory, kSoundPick);
        if (k.keyRow < 3) {
            if (static_cast<int>(k.text.size()) < kInitials)
                k.text += kLetterRows[static_cast<std::size_t>(k.keyRow)][static_cast<std::size_t>(k.keyCol)];
            return Result::Open;
        }
        switch (kSpecials[static_cast<std::size_t>(k.keyCol)]) {
            case Special::Space:
                if (static_cast<int>(k.text.size()) < kInitials)
                    k.text += ' ';
                break;
            case Special::Delete:
                if (!k.text.empty())
                    k.text.pop_back();
                break;
            case Special::Ok:
                return Result::Ok;
            case Special::Cancel:
                return Result::Cancel;
        }
    }
    return Result::Open;
}

void draw(const Keyboard &k, Canvas &canvas) {
    canvas.text(kLabelColumn, kTitleRow, k.title, true);
    std::string shown = k.text;
    shown.resize(kInitials, '-');
    canvas.text(kKeyColumn, 6, "INITIALS: " + shown, true);

    for (int keyRow = 0; keyRow < kKeyRows; ++keyRow) {
        const int y = kFirstKeyRow + keyRow * 2;
        for (int col = 0; col < keysInRow(keyRow); ++col) {
            const bool selected = keyRow == k.keyRow && col == k.keyCol;
            if (keyRow < 3) {
                const char letter = kLetterRows[static_cast<std::size_t>(keyRow)][static_cast<std::size_t>(col)];
                canvas.text(kKeyColumn + col * kKeyStride, y, std::string(1, letter), selected);
            } else {
                canvas.text(kSpecialCols[static_cast<std::size_t>(col)], y, specialLabel(kSpecials[static_cast<std::size_t>(col)]), selected);
            }
        }
    }
    canvas.text(kLabelColumn, kHelpRow, "MOVE AND CONFIRM TO PICK A LETTER.", true);
    canvas.text(kLabelColumn, kHelpRow + 1, "BACK LEAVES WITHOUT CHANGES.", true);
}

} // namespace SoRInitialsKeyboard
