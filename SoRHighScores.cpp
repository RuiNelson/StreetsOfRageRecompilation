#include "SoRHighScores.hpp"

#include "SoRControlsConfig.hpp"
#include "SoRQolConfig.hpp"
#include "SoRRemoteInput.hpp"

#include "system/memory/SystemMemory.hpp"

#include <yaml-cpp/yaml.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <utility>
#include <string>
#include <system_error>

namespace SoRHighScores {
namespace {

constexpr std::uint32_t kTable      = 0xFFFFFF4Au;
constexpr std::size_t   kEntries    = 10;
constexpr std::size_t   kEntrySize  = 12;
constexpr std::uint8_t  kFlagBits   = 0xC0; // +0: player 1 and player 2 "new entry" colours
constexpr std::uint32_t kP1Object   = 0xFFFFB800u;
constexpr std::uint32_t kP2Object   = 0xFFFFB880u;
constexpr std::uint32_t kNameEntry  = 0x4Bu; // object +$4B bit 7: typing a name

constexpr std::uint32_t kGameState    = 0xFFFFFF00u;
constexpr std::uint32_t kLevel        = 0xFFFFFF02u;
constexpr std::uint32_t kPlayerMode   = 0xFFFFFF18u;
constexpr std::uint32_t kDifficulty   = 0xFFFFFFC6u;
constexpr std::uint32_t kScoreP1      = 0xFFFFFF08u; // score long, then the name at +5
constexpr std::uint32_t kScoreP2      = 0xFFFFFF10u;
constexpr std::uint32_t kLastEntry    = kTable + (kEntries - 1) * kEntrySize;
constexpr std::uint32_t kObjectState  = 0x30u;
constexpr std::uint16_t kGameStateEnding = 0x1A;

using Table = std::array<std::uint8_t, kEntries * kEntrySize>;

Table &saved() {
    static Table table{};
    return table;
}

Table read(SystemMemory &memory) {
    Table table{};
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i] = memory.readByte(kTable + static_cast<std::uint32_t>(i));
    for (std::size_t entry = 0; entry < kEntries; ++entry)
        table[entry * kEntrySize] &= static_cast<std::uint8_t>(~kFlagBits);
    return table;
}

std::string toHex(const std::uint8_t *bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    for (std::size_t i = 0; i < kEntrySize; ++i) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 0x0F];
    }
    return text;
}

bool fromHex(const std::string &text, std::uint8_t *bytes) {
    if (text.size() != kEntrySize * 2)
        return false;
    for (std::size_t i = 0; i < kEntrySize; ++i) {
        unsigned value = 0;
        for (std::size_t half = 0; half < 2; ++half) {
            const char c = text[i * 2 + half];
            unsigned   digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned>(c - '0');
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned>(c - 'A' + 10);
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned>(c - 'a' + 10);
            else
                return false;
            value = value * 16 + digit;
        }
        bytes[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

bool write(const Table &table, const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    YAML::Node scores(YAML::NodeType::Sequence);
    for (std::size_t entry = 0; entry < kEntries; ++entry)
        scores.push_back(toHex(table.data() + entry * kEntrySize));
    YAML::Node root;
    root["scores"] = scores;

    YAML::Emitter emitter;
    emitter << root;

    // Write next to the file and rename, so a crash never leaves half a file.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "# Streets of Rage high scores (OPTIONS > QUALITY-OF-LIFE > SAVE HIGH SCORES).\n"
            << "# Ten entries of twelve bytes, best first, as the game keeps them.\n"
            << emitter.c_str() << '\n';
        if (!out)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

bool typingName(SystemMemory &memory) {
    return (memory.readByte(kP1Object + kNameEntry) & 0x80u) != 0 || (memory.readByte(kP2Object + kNameEntry) & 0x80u) != 0;
}

// $12784 and $127BA: the player's score goes in the tenth place and the table is sorted. The sort is the
// ROM's, entry by entry (swaps keep the flag bits), so equal scores end up as they do there.
void insertAndSort(SystemMemory &memory, int player) {
    const std::uint32_t score = player == 0 ? kScoreP1 : kScoreP2;
    const unsigned      flag  = player == 0 ? 7u : 6u;

    memory.writeByte(score, static_cast<std::uint8_t>(memory.readByte(score) | (1u << flag)));
    memory.writeLong(kLastEntry, memory.readLong(score));
    memory.writeWord(kLastEntry + 4, static_cast<std::uint16_t>(memory.readWord(kLevel) + 1u));
    memory.writeLong(kLastEntry + 6, memory.readLong(score + 4));
    memory.writeWord(kLastEntry + 10, memory.readWord(kDifficulty));
    for (std::size_t entry = 0; entry + 1 < kEntries; ++entry) {
        const std::uint32_t address = kTable + static_cast<std::uint32_t>(entry * kEntrySize);
        memory.writeByte(address, static_cast<std::uint8_t>(memory.readByte(address) & ~(1u << flag)));
    }

    std::array<std::array<std::uint8_t, kEntrySize>, kEntries> entries{};
    for (std::size_t entry = 0; entry < kEntries; ++entry)
        for (std::size_t i = 0; i < kEntrySize; ++i)
            entries[entry][i] = memory.readByte(kTable + static_cast<std::uint32_t>(entry * kEntrySize + i));

    const auto be32 = [](const std::array<std::uint8_t, kEntrySize> &e) {
        return (static_cast<std::uint32_t>(e[0]) << 24 | static_cast<std::uint32_t>(e[1]) << 16 | static_cast<std::uint32_t>(e[2]) << 8 | e[3]) & 0x3FFFFFFFu;
    };
    const auto be16 = [](const std::array<std::uint8_t, kEntrySize> &e, std::size_t offset) {
        return static_cast<std::uint16_t>(e[offset] << 8 | e[offset + 1]);
    };
    for (std::size_t i = 0; i + 1 < kEntries; ++i) {
        for (std::size_t j = i + 1; j < kEntries; ++j) {
            const auto &a = entries[i];
            const auto &b = entries[j];
            bool swap;
            if (be32(a) != be32(b))
                swap = be32(a) < be32(b);
            else if (be16(a, 10) != be16(b, 10))
                swap = be16(a, 10) < be16(b, 10);
            else
                swap = be16(a, 4) < be16(b, 4);
            if (swap)
                std::swap(entries[i], entries[j]);
        }
    }
    for (std::size_t entry = 0; entry < kEntries; ++entry)
        for (std::size_t i = 0; i < kEntrySize; ++i)
            memory.writeByte(kTable + static_cast<std::uint32_t>(entry * kEntrySize + i), entries[entry][i]);
}

} // namespace

bool enterInitials(SystemMemory &memory, std::uint32_t object) {
    if (memory.readByte(object + kObjectState) != 0)
        return false; // the name entry is already under way
    const int player = object == kP1Object ? 0 : 1;

    std::string initials = player == 0 ? SoRQolConfig::current().initialsP1 : SoRQolConfig::current().initialsP2;
    if (player == 1 && SoRRemoteInput::aiAssist())
        initials = "AI ";
    if (initials.empty())
        return false;

    // The name is the low three bytes of the long after the score: a letter's number from 1, blank 0.
    const std::uint32_t name = (player == 0 ? kScoreP1 : kScoreP2) + 4;
    memory.writeLong(name, 0);
    for (std::size_t i = 0; i < 3; ++i) {
        const char c = i < initials.size() ? initials[i] : ' ';
        memory.writeByte(name + 1 + static_cast<std::uint32_t>(i), c == ' ' ? 0 : static_cast<std::uint8_t>(c - 'A' + 1));
    }

    // $5846: what the name entry does when the last letter is in.
    if (memory.readWord(kGameState) == kGameStateEnding) {
        memory.writeByte(object + kObjectState, 2);
    } else {
        memory.writeByte(kPlayerMode, static_cast<std::uint8_t>(memory.readByte(kPlayerMode) | (1u << player)));
        memory.writeByte(object + 0x4Bu, static_cast<std::uint8_t>(memory.readByte(object + 0x4Bu) & 0x7Fu));
        memory.writeByte(object + kObjectState, 0);
    }
    insertAndSort(memory, player);
    return true;
}

std::filesystem::path defaultPath() {
    return SoRControlsConfig::defaultPath().parent_path() / "highscores.yaml";
}

void load(SystemMemory &memory) {
    const std::filesystem::path path = defaultPath();
    std::error_code             error;
    if (std::filesystem::exists(path, error)) {
        Table table{};
        bool  valid = false;
        try {
            const YAML::Node root = YAML::LoadFile(path.string());
            const YAML::Node list = root.IsMap() ? root["scores"] : YAML::Node();
            valid                 = list && list.IsSequence() && list.size() == kEntries;
            for (std::size_t entry = 0; valid && entry < kEntries; ++entry)
                valid = fromHex(list[entry].as<std::string>(), table.data() + entry * kEntrySize);
        } catch (const YAML::Exception &exception) {
            std::fprintf(stderr, "Could not read %s (%s): using the game's high scores\n", path.string().c_str(), exception.what());
            valid = false;
        }
        if (valid) {
            for (std::size_t i = 0; i < table.size(); ++i)
                memory.writeByte(kTable + static_cast<std::uint32_t>(i), table[i]);
        } else {
            std::fprintf(stderr, "%s is not a table of ten high scores: using the game's\n", path.string().c_str());
        }
    }
    saved() = read(memory);
}

void saveIfChanged(SystemMemory &memory, bool force) {
    if (!SoRQolConfig::current().persistHighScores)
        return;
    if (!force && typingName(memory))
        return;
    const Table table = read(memory);
    if (table == saved())
        return;
    if (!write(table, defaultPath()))
        std::fprintf(stderr, "Could not write %s\n", defaultPath().string().c_str());
    saved() = table;
}

void erase() {
    std::error_code error;
    std::filesystem::remove(defaultPath(), error);
}

} // namespace SoRHighScores
