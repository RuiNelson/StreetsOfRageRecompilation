#include "SoRSurvival.hpp"

#include "SoRControlsConfig.hpp"
#include "SoRExtraMenu.hpp"
#include "SoRQolConfig.hpp"
#include "SoR.hpp"

#include "system/memory/SystemMemory.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <system_error>

namespace SoRSurvival {
namespace {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// Persistent fields.
constexpr u32 kGameState     = 0xFFFFFF00u;
constexpr u32 kLevel         = 0xFFFFFF02u;
constexpr u32 kWave          = 0xFFFFFF04u;
constexpr u32 kScoreP1       = 0xFFFFFF09u; // three packed-BCD bytes, P2's 8 further on
constexpr u32 kScoreLongP1   = 0xFFFFFF08u; // score_p1, the long holding them; score_p2 8 further on
constexpr u32 kPlayerMode    = 0xFFFFFF18u;
constexpr u32 kP1Continues   = 0xFFFFFF1Au; // word, P2 at +2
constexpr u32 kP1Lives       = 0xFFFFFF20u; // lives, specials; P2's at +3

constexpr u16 kStateTop10           = 0x0Cu; // where ingame_finish_fade goes after GAME OVER
constexpr u16 kStateModeMenu        = 0x10u;
constexpr u16 kStateIngame          = 0x16u;
constexpr u16 kStateCharacterSelect = 0x20u;
constexpr u8  kModeOnePlayer        = 1; // select_menu_player_mode_tbl ($117A)
constexpr u8  kModeTwoPlayers       = 3;
constexpr u8  kLives                = 3; // BCD

// The camera ($FFE000): the X integer, and the bounds' integer words.
constexpr u32 kCamX      = 0xFFFFE002u;
constexpr u32 kCamXMax   = 0xFFFFE01Au;
constexpr u32 kCamXMin   = 0xFFFFE01Eu;
constexpr u16 kRound8    = 7; // round 8 scrolls left: its open bound is the minimum
constexpr int kRound7    = 6; // the elevator: past its right edge is the drop

// Round 7: an enemy that would come in from the right falls from the sky onto the elevator instead (user: the
// right is the drop, and they fell into it). Placed over the deck's right half, above the top of the screen, it
// comes down under the ordinary enemies' own gravity ($973E, in every walk) and lands on the floor.
constexpr u16 kDropFromX  = 0xA0; // screen X, from the camera
constexpr u16 kDropToX    = 0x100;
constexpr u16 kDropHeight = 0xFFF0; // +$18, as Haku-Ro's ambush drop ($E952)
constexpr u16 kRightEdge  = 0x140; // $C60 places a right-side enemy at cam_x + $148 ($168 in widescreen)

// The level pipeline and the enemy residency allocator ($5F4-$C28, $9D8C).
constexpr u32 kLevelIntroActive     = 0xFFFFFA1Fu;
constexpr u32 kLevelSpawnFlowFlags  = 0xFFFFFA05u;
constexpr u32 kWaveAdvancePending   = 0xFFFFFA0Du;
constexpr u32 kPalCorrect           = 0xFFFFFA60u; // family per residency slot, then the boss slot $FA63
constexpr u32 kFamilyCounts         = 0xFFFFFA65u; // $FA65-$FA67, the boss slot's $FA68
constexpr u32 kVariantCounts        = 0xFFFFFA6Du; // $FA6D-$FA6F
constexpr u32 kDeferredSpawnCredits = 0xFFFFFA70u;
constexpr u32 kStopClock            = 0xFFFFFA79u;
constexpr u32 kStatusBitfield1      = 0xFFFFFB14u; // its low byte is the pipeline state
constexpr u32 kActiveProgression    = 0xFFFFFB1Eu;
constexpr u32 kCurrentSpawnSection  = 0xFFFFFC2Cu;
constexpr u8  kFlowRelease          = 0x04u; // bit 2: a death gives back its residency ($9DC0)
constexpr u8  kFlowSectionEmpty     = 0x08u; // bit 3: the last tracked death ends the section
constexpr u16 kPipelineSelect       = 0x08u; // $936 select_deferred_spawn
constexpr u16 kPipelineIdle         = 0x0Cu; // $C28, idle while boss_forced_reaction_flags bit 0 is clear

// The section of deferred records: the ELC buffer, whose round stream is not read again in this mode.
constexpr u32 kSection       = 0x00FF6800u;
constexpr int kRecordSize    = 6;
constexpr int kSectionLength = 6;
constexpr u8  kEndOfSection  = 0x99u;

constexpr u32 kP1Object    = 0xFFFFB800u;
constexpr u32 kP2Object    = 0xFFFFB880u;
constexpr u32 kObjectTable = 0xFFFFB900u;
constexpr int kObjectSlots = 32;
constexpr u32 kObjectSize  = 0x80u;

// The HUD block, as draw_player_lives_and_specials ($4E14) writes it, and the round timer's two digits, two
// tiles tall ($10CCC), with the tile the HUD blanks a cell with.
constexpr u32 kHudLives      = 0xFFFF6020u;
constexpr u32 kHudTimer      = 0xFFFF602Eu;
constexpr u32 kHudRowBelow   = 0x50u;
constexpr u16 kHudBlankTile  = 0x06D7u;

// How many enemies at once (user): 1, then 2 after the first one beaten, 3 after 3, and one more every 2 after
// that (1, 3, 5, 7...), up to 16. The object table has 32 slots, which items, Jack's axes and the scenery share,
// so the count also stops where fewer than 8 would be left free.
constexpr unsigned kMostOnScreen = 16;
constexpr unsigned kFreeSlotsKept = 8;
unsigned enemiesAtOnce(unsigned kills) {
    return std::min(kMostOnScreen, 1 + (kills + 1) / 2);
}
// One enemy in 6 leaves an item.
constexpr unsigned kItemOneIn = 6;

// What an enemy can be, from the records that bring each regular type in from a screen edge in the eight
// ELC streams (object +$40 behaviour, +$41 approach offsets): a camera-gated +$40 (bit 7) never wakes at a
// locked camera, and $43 is round 7's drop from above. Type $2A has none: its colours are $25's anyway.
struct Spawn {
    u8 type;
    u8 behaviour; // +$40
    u8 approach;  // +$41
};
constexpr Spawn kSpawns[] = {
    {0x20, 0x00, 0x19}, {0x20, 0x00, 0x2B}, {0x20, 0x00, 0x39}, {0x20, 0x00, 0x4C}, {0x20, 0x00, 0x5A},
    {0x20, 0x01, 0x28}, {0x20, 0x01, 0x5C}, {0x20, 0x01, 0x6D}, {0x20, 0x01, 0x78},
    {0x21, 0x00, 0x1C}, {0x21, 0x00, 0x2C}, {0x21, 0x00, 0x3A}, {0x21, 0x00, 0x4A}, {0x21, 0x00, 0x5D},
    {0x21, 0x00, 0x6B}, {0x21, 0x00, 0xFB},
    {0x22, 0x00, 0x1E}, {0x22, 0x00, 0x2C}, {0x22, 0x00, 0x3B}, {0x22, 0x00, 0x4C}, {0x22, 0x00, 0x5B},
    {0x22, 0x00, 0x7D}, {0x22, 0x03, 0x3E}, {0x22, 0x03, 0x4C}, {0x22, 0x03, 0x5A}, {0x22, 0x03, 0x6B},
    {0x23, 0x00, 0x5B}, {0x23, 0x00, 0x5E}, {0x23, 0x00, 0xFA}, {0x23, 0x01, 0x1C},
    {0x24, 0x00, 0x1D}, {0x24, 0x00, 0x2D}, {0x24, 0x00, 0x3C}, {0x24, 0x00, 0x4D}, {0x24, 0x00, 0x5B},
    {0x24, 0x00, 0x7A}, {0x24, 0x01, 0x2C}, {0x24, 0x01, 0x3B}, {0x24, 0x01, 0x49}, {0x24, 0x01, 0x5B},
    {0x25, 0x00, 0x39}, {0x25, 0x00, 0x3C}, {0x25, 0x00, 0x49}, {0x25, 0x00, 0x4D}, {0x25, 0x00, 0x5E},
    {0x25, 0x01, 0x3C},
    {0x26, 0x00, 0x1C}, {0x26, 0x00, 0x2C}, {0x26, 0x00, 0x3C}, {0x26, 0x00, 0x4A}, {0x26, 0x00, 0x5B},
    {0x26, 0x00, 0x7C}, {0x26, 0x10, 0x49}, {0x26, 0x10, 0x59},
    {0x27, 0x00, 0x2E}, {0x27, 0x00, 0x3C}, {0x27, 0x02, 0x3C}, {0x27, 0x02, 0x4A}, {0x27, 0x03, 0x4B},
    {0x27, 0x03, 0x4C}, {0x27, 0x12, 0x3A}, {0x27, 0x12, 0x4C},
};
constexpr u8 kTypes[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};
constexpr u8 kVariants = 3; // object +$49: the colour, with its health and damage

// The lanes (+$15) those records use in each round.
constexpr std::array<std::array<u8, 8>, 8> kLanes = {{
    {0x20, 0x35, 0x40, 0x48, 0x50, 0x58, 0x68, 0x6A},
    {0x30, 0x38, 0x39, 0x40, 0x41, 0x58, 0x60, 0x62},
    {0x20, 0x38, 0x50, 0x58, 0x59, 0x61, 0x68, 0x69},
    {0x11, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x60},
    {0x10, 0x20, 0x22, 0x30, 0x38, 0x40, 0x50, 0x60},
    {0x20, 0x30, 0x40, 0x48, 0x50, 0x58, 0x62, 0x68},
    {0x40, 0x40, 0x68, 0x68, 0x69, 0x69, 0x6A, 0x6A},
    {0x20, 0x28, 0x30, 0x38, 0x48, 0x50, 0x60, 0x68},
}};

// What an enemy can leave (user): an apple, a chicken, a knife, a pipe or a bottle.
constexpr u8 kItems[] = {0x4B, 0x47, 0x08, 0x0B, 0x09};

struct State {
    std::optional<Records> records;
    std::string            initials[2]; // as kept in the file; empty: QUALITY-OF-LIFE's
    bool                   active   = false;
    bool                   started  = false; // the enemies have begun to come
    int                    players  = 1;
    int                    round    = 0;
    u16                    savedLevel = 0;   // OPTIONS' ROUND NO. (cheat), put back afterwards
    u16                    lockedX  = 0;
    unsigned               kills    = 0;
    std::mt19937           random{std::random_device{}()};
};

State &state() {
    static State instance;
    return instance;
}

std::vector<Entry> readTable(const YAML::Node &node, int players) {
    std::vector<Entry> table;
    if (!node)
        return table;
    if (node.IsScalar()) { // the first version kept one number, without initials
        Entry entry;
        entry.points = node.as<unsigned>();
        if (entry.points > 0)
            table.push_back(entry);
        return table;
    }
    for (const YAML::Node &item : node) {
        Entry entry;
        entry.points = item["points"].as<unsigned>(0);
        if (const YAML::Node who = item["initials"]) {
            if (who.IsSequence()) {
                for (std::size_t i = 0; i < 2 && i < who.size(); ++i)
                    entry.initials[i] = SoRQolConfig::normalizeInitials(who[i].as<std::string>(""));
            } else {
                entry.initials[0] = SoRQolConfig::normalizeInitials(who.as<std::string>(""));
            }
        }
        if (players == 1)
            entry.initials[1].clear();
        table.push_back(entry);
    }
    std::stable_sort(table.begin(), table.end(), [](const Entry &a, const Entry &b) { return a.points > b.points; });
    if (table.size() > kTopEntries)
        table.resize(kTopEntries);
    return table;
}

void load(State &s, const std::filesystem::path &path) {
    s.records = Records{};
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return;
    try {
        const YAML::Node root = YAML::LoadFile(path.string());
        s.records->onePlayer  = readTable(root["best_1p"], 1);
        s.records->twoPlayers = readTable(root["best_2p"], 2);
        s.initials[0]         = SoRQolConfig::normalizeInitials(root["initials1"].as<std::string>(""));
        s.initials[1]         = SoRQolConfig::normalizeInitials(root["initials2"].as<std::string>(""));
    } catch (const YAML::Exception &exception) {
        std::fprintf(stderr, "Could not read %s: %s\n", path.string().c_str(), exception.what());
        s.records = Records{};
    }
}

void loadOnce(State &s) {
    if (!s.records)
        load(s, defaultPath());
}

YAML::Node writeTable(const std::vector<Entry> &table, int players) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (const Entry &entry : table) {
        YAML::Node item;
        if (players == 2) {
            YAML::Node who(YAML::NodeType::Sequence);
            who.push_back(entry.initials[0]);
            who.push_back(entry.initials[1]);
            who.SetStyle(YAML::EmitterStyle::Flow);
            item["initials"] = who;
        } else {
            item["initials"] = entry.initials[0];
        }
        item["points"] = entry.points;
        item.SetStyle(YAML::EmitterStyle::Flow);
        node.push_back(item);
    }
    return node;
}

bool save(const State &s, const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    YAML::Node root;
    root["initials1"] = s.initials[0];
    root["initials2"] = s.initials[1];
    root["best_1p"]   = writeTable(s.records->onePlayer, 1);
    root["best_2p"]   = writeTable(s.records->twoPlayers, 2);
    YAML::Emitter emitter;
    emitter << root;
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "# Streets of Rage survival records (EXTRA > SURVIVAL MODE): enemies beaten.\n" << emitter.c_str() << '\n';
        if (!out)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

void saveOrReport(const State &s) {
    if (!save(s, defaultPath()))
        std::fprintf(stderr, "Could not write %s\n", defaultPath().string().c_str());
}

unsigned fromBcd(SystemMemory &memory, u32 address, int bytes) {
    unsigned value = 0;
    for (int i = 0; i < bytes; ++i) {
        const u8 byte = memory.readByte(address + static_cast<u32>(i));
        value         = value * 100 + (byte >> 4) * 10 + (byte & 0x0Fu);
    }
    return value;
}

void toBcd(SystemMemory &memory, u32 address, int bytes, unsigned value) {
    for (int i = bytes - 1; i >= 0; --i) {
        const unsigned pair = value % 100;
        value /= 100;
        memory.writeByte(address + static_cast<u32>(i), static_cast<u8>((pair / 10) << 4 | (pair % 10)));
    }
}

unsigned pointsOf(SystemMemory &memory, int player) {
    return fromBcd(memory, kScoreP1 + 8u * static_cast<u32>(player), 3);
}

// draw_player_lives_and_specials ($4E14), for one player.
void drawLivesAndSpecials(SystemMemory &memory, int player) {
    const u32 hud    = kHudLives + (player == 0 ? 0u : 0x34u);
    const int second = player == 0 ? 0x0A : -0x1A;
    const u32 values = kP1Lives + (player == 0 ? 0u : 3u);
    const auto digit = [](u8 value) { return static_cast<u16>((value < 0x10 ? value : 9) * 2 + 0x6C0); };
    const u16 lives    = digit(memory.readByte(values));
    const u16 specials = digit(memory.readByte(values + 1));
    memory.writeWord(hud, lives);
    memory.writeWord(hud + 0x50, static_cast<u16>(lives + 1));
    memory.writeWord(static_cast<u32>(static_cast<int>(hud) + second), specials);
    memory.writeWord(static_cast<u32>(static_cast<int>(hud) + second + 0x50), static_cast<u16>(specials + 1));
}

void writeRecord(SystemMemory &memory, u32 address) {
    State &s = state();
    const u8 type = kTypes[std::uniform_int_distribution<std::size_t>(0, std::size(kTypes) - 1)(s.random)];
    std::array<const Spawn *, std::size(kSpawns)> choices{};
    std::size_t                                    count = 0;
    for (const Spawn &entry : kSpawns)
        if (entry.type == type)
            choices[count++] = &entry;
    const Spawn &entry   = *choices[std::uniform_int_distribution<std::size_t>(0, count - 1)(s.random)];
    const auto  &lanes   = kLanes[static_cast<std::size_t>(s.round)];
    memory.writeByte(address + 0, entry.type);
    memory.writeByte(address + 1, entry.behaviour);
    memory.writeByte(address + 2, entry.approach);
    memory.writeByte(address + 3, static_cast<u8>(std::uniform_int_distribution<int>(0, kVariants - 1)(s.random)));
    memory.writeByte(address + 4, lanes[std::uniform_int_distribution<std::size_t>(0, lanes.size() - 1)(s.random)]);
    // 0 right, else left. Round 8 starts at its map's right end with the camera scrolling left, and an enemy
    // placed at its left edge (cam_x - 8) never wakes there (seen live: state 0 for good): the right only.
    const bool left = s.round != kRound8 && std::uniform_int_distribution<int>(0, 1)(s.random) != 0;
    memory.writeByte(address + 5, left ? 1 : 0);
}

// Fills the section up to its length, after what is still waiting in it.
void fillSection(SystemMemory &memory) {
    u32 address = kSection;
    int count   = 0;
    while (memory.readByte(address) != kEndOfSection && count < kSectionLength) {
        address += kRecordSize;
        ++count;
    }
    for (; count < kSectionLength; ++count, address += kRecordSize)
        writeRecord(memory, address);
    memory.writeByte(address, kEndOfSection);
    memory.writeByte(address + 1, 0);
}

void dropFromTheSky(SystemMemory &memory) {
    State    &s     = state();
    const u16 camX  = memory.readWord(kCamX);
    for (int slot = 0; slot < kObjectSlots; ++slot) {
        const u32 object = kObjectTable + static_cast<u32>(slot) * kObjectSize;
        const u8  type   = memory.readByte(object);
        // Only one $C60 has just placed: not yet awake (primary state 0), at the right edge.
        const u16 fromCamera = static_cast<u16>(memory.readWord(object + 0x10) - camX); // the left edge is cam_x - 8
        if (type < 0x20 || type > 0x27 || memory.readWord(object + 0x30) != 0 || fromCamera < kRightEdge ||
            fromCamera >= kRightEdge + 0x40)
            continue;
        const u16 x = static_cast<u16>(camX + std::uniform_int_distribution<int>(kDropFromX, kDropToX)(s.random));
        memory.writeWord(object + 0x10, x);
        memory.writeWord(object + 0x18, kDropHeight);
        memory.writeLong(object + 0x24, 0);
    }
}

void lockCamera(SystemMemory &memory) {
    const State &s = state();
    memory.writeLong(memory.readWord(kLevel) == kRound8 ? kCamXMin : kCamXMax, static_cast<u32>(s.lockedX) << 16);
}

void clearScores(SystemMemory &memory) {
    for (u32 player = 0; player < 2; ++player)
        memory.writeLong(kScoreLongP1 + 8u * player, 0);
}

void finish(SystemMemory &memory, bool completed) {
    State &s  = state();
    s.active  = false;
    s.started = false;
    memory.writeWord(kLevel, s.savedLevel);
    memory.writeWord(kWave, 0);
    memory.writeByte(kStopClock, 0);
    Result result;
    result.players   = s.players;
    result.points[0] = pointsOf(memory, 0);
    result.points[1] = s.players == 2 ? pointsOf(memory, 1) : 0;
    result.total     = result.points[0] + result.points[1];
    // The game over screen's way to the high-score table, which clears the scores, is not taken (user report:
    // the next game started with the survival points).
    clearScores(memory);
    if (!completed)
        return;

    for (int player = 0; player < s.players; ++player)
        result.initials[player] = initials(player);
    loadOnce(s);
    std::vector<Entry> &table = s.players == 2 ? s.records->twoPlayers : s.records->onePlayer;
    Entry entry;
    entry.initials[0] = result.initials[0];
    entry.initials[1] = result.initials[1];
    entry.points      = result.total;
    // A tie keeps the older entry above.
    std::size_t place = 0;
    while (place < table.size() && table[place].points >= result.total)
        ++place;
    if (result.total > 0 && place < kTopEntries) {
        table.insert(table.begin() + static_cast<std::ptrdiff_t>(place), entry);
        if (table.size() > kTopEntries)
            table.resize(kTopEntries);
        result.place = static_cast<int>(place);
        saveOrReport(s);
    }
    SoRExtraMenu::showResult(result);
}

} // namespace

std::filesystem::path defaultPath() {
    return SoRControlsConfig::defaultPath().parent_path() / "survival.yaml";
}

const Records &records() {
    State &s = state();
    loadOnce(s);
    return *s.records;
}

std::string initials(int player) {
    State &s = state();
    loadOnce(s);
    const std::string &own = s.initials[player == 1 ? 1 : 0];
    if (!own.empty())
        return own;
    const SoRQolConfig::QolConfig &qol = SoRQolConfig::current();
    return player == 1 ? qol.initialsP2 : qol.initialsP1;
}

void setInitials(int player, const std::string &initials) {
    State &s = state();
    loadOnce(s);
    s.initials[player == 1 ? 1 : 0] = SoRQolConfig::normalizeInitials(initials);
    saveOrReport(s);
}

void begin(SystemMemory &memory, int players, int round) {
    State &s     = state();
    s.active     = true;
    s.started    = false;
    s.players    = players == 2 ? 2 : 1;
    s.round      = std::clamp(round, 0, 7);
    s.kills      = 0;
    s.savedLevel = memory.readWord(kLevel);
    clearScores(memory);
    memory.writeByte(kPlayerMode, s.players == 2 ? kModeTwoPlayers : kModeOnePlayer);
    memory.writeWord(kLevel, static_cast<u16>(s.round));
    memory.writeWord(kWave, 0);
    memory.writeWord(kGameState, kStateCharacterSelect);
}

bool active() {
    return state().active;
}

int lastRound() {
    return state().round;
}

int lastPlayers() {
    return state().players;
}

void setUpPlayers(SystemMemory &memory) {
    for (u32 player = 0; player < 2; ++player) {
        memory.writeWord(kP1Continues + 2u * player, 0);
        memory.writeByte(kP1Lives + 3u * player, kLives);
    }
}

void setUpRound(SystemMemory &memory) {
    State &s = state();
    if (!s.active)
        return;

    // start_round_setup has just created the first wave, none of which has run yet. The round's scenery stays
    // (round 3's palms $4E, round 7's elevator props $50-$52); round 7's $53 goes, so the elevator never climbs.
    for (int slot = 0; slot < kObjectSlots; ++slot) {
        const u32 object = kObjectTable + static_cast<u32>(slot) * kObjectSize;
        const u8  type   = memory.readByte(object);
        if (type == 0 || type == 0x4E || (type >= 0x50 && type <= 0x52))
            continue;
        for (u32 i = 0; i < kObjectSize; i += 4)
            memory.writeLong(object + i, 0);
    }
    memory.writeWord(kActiveProgression, 0);

    // The camera stays where the round starts.
    s.lockedX = memory.readWord(kCamX);
    lockCamera(memory);

    // No residency is taken: every family and colour slot is free for the first enemies, and the boss slot is
    // never used (release_progression_entity clears the family slots when its count reaches zero).
    memory.writeByte(kPalCorrect + 3, 0);
    for (u32 i = memory.readWord(kLevel) == 0 ? 1u : 0u; i < 4; ++i)
        memory.writeByte(kFamilyCounts + i, 0);
    for (u32 i = 0; i < 3; ++i)
        memory.writeByte(kVariantCounts + i, 0);

    memory.writeByte(kLevelSpawnFlowFlags, 0);
    memory.writeByte(kDeferredSpawnCredits, 0);
    memory.writeByte(kWaveAdvancePending, 0);
    memory.writeWord(kStatusBitfield1, kPipelineIdle);
    s.started = false;
}

void onFrame(SystemMemory &memory) {
    State &s = state();
    if (!s.active)
        return;

    const u16 gameState = memory.readWord(kGameState);
    if (gameState == kStateTop10) {
        // GAME OVER: ingame_finish_fade sends the game to the high-score table. The results, in EXTRA.
        finish(memory, true);
        memory.writeWord(kGameState, kStateModeMenu);
        return;
    }
    if (gameState < kStateModeMenu) {
        finish(memory, false); // a reset, or anything else that left the game
        return;
    }
    if (gameState != kStateIngame || memory.readByte(kLevelIntroActive) != 0)
        return;

    // No clock (user): stopped, and its digits blank under TIME.
    memory.writeByte(kStopClock, 1);
    for (u32 cell = 0; cell < 2; ++cell) {
        memory.writeWord(kHudTimer + 2u * cell, kHudBlankTile);
        memory.writeWord(kHudTimer + kHudRowBelow + 2u * cell, kHudBlankTile);
    }
    lockCamera(memory);
    if (s.round == kRound7)
        dropFromTheSky(memory);
    for (int player = 0; player < 2; ++player) {
        const u32 specials = kP1Lives + 3u * static_cast<u32>(player) + 1u;
        if (memory.readByte(specials) != 0) {
            memory.writeByte(specials, 0);
            drawLivesAndSpecials(memory, player);
        }
    }

    // The enemies: the ROM's deferred spawn, fed from the section.
    const u16 pipeline = static_cast<u16>(memory.readWord(kStatusBitfield1) & 0x00FFu);
    if (!s.started) {
        memory.writeByte(kSection, kEndOfSection);
        fillSection(memory);
        memory.writeLong(kCurrentSpawnSection, kSection);
        memory.writeWord(kStatusBitfield1, kPipelineSelect);
        s.started = true;
    } else if (pipeline == kPipelineSelect) {
        fillSection(memory);
    } else if (pipeline != 0x0Au) {
        // Nothing else is ever wanted: back to the section (the next wave's setup would read the round's stream).
        memory.writeLong(kCurrentSpawnSection, kSection);
        fillSection(memory);
        memory.writeWord(kStatusBitfield1, kPipelineSelect);
    }
    memory.writeByte(kLevelSpawnFlowFlags,
                     static_cast<u8>((memory.readByte(kLevelSpawnFlowFlags) | kFlowRelease) & ~kFlowSectionEmpty));

    unsigned freeSlots = 0;
    for (int slot = 0; slot < kObjectSlots; ++slot)
        if (memory.readByte(kObjectTable + static_cast<u32>(slot) * kObjectSize) == 0)
            ++freeSlots;
    const unsigned onScreen = enemiesAtOnce(s.kills);
    const unsigned alive    = memory.readWord(kActiveProgression);
    unsigned       credits  = alive < onScreen ? onScreen - alive : 0;
    credits                 = std::min(credits, freeSlots > kFreeSlotsKept ? freeSlots - kFreeSlotsKept : 0u);
    memory.writeByte(kDeferredSpawnCredits, static_cast<u8>(credits));
}

void enemyDefeated(SystemMemory &memory, unsigned enemy, int player) {
    State &s = state();
    ++s.kills;
    const u32 score = kScoreP1 + 8u * static_cast<u32>(player);
    toBcd(memory, score, 3, std::min(999999u, fromBcd(memory, score, 3) + 1));

    if (std::uniform_int_distribution<unsigned>(1, kItemOneIn)(s.random) != 1)
        return;
    for (int slot = 0; slot < kObjectSlots; ++slot) {
        const u32 object = kObjectTable + static_cast<u32>(slot) * kObjectSize;
        if ((object & 0x00FFFFFFu) == (enemy & 0x00FFFFFFu) || memory.readByte(object) != 0)
            continue;
        for (u32 i = 0; i < kObjectSize; i += 4)
            memory.writeLong(object + i, 0);
        memory.writeByte(object, kItems[std::uniform_int_distribution<std::size_t>(0, std::size(kItems) - 1)(s.random)]);
        memory.writeWord(object + 0x10, memory.readWord(enemy + 0x10));
        memory.writeWord(object + 0x14, memory.readWord(enemy + 0x14));
        memory.writeWord(object + 0x18, memory.readWord(enemy + 0x18));
        return;
    }
}

} // namespace SoRSurvival

// ---------------------------------------------------------------------------
// $009E26 — ordinary_enemy_award_score
//
// Called by the ordinary enemies' death path ($9D8C) just before the slot is cleared: the score of the enemy
// (+$39, its table entry) goes to the player bit 7 names, P2 when set. In a survival game the enemy is a point
// instead (SoRSurvival); otherwise add_bcd_score_value adds the entry, as the ROM's tail jump does.
// ---------------------------------------------------------------------------
void StreetsOfRage::ordinary_enemy_award_score(m_long /*entry_*/) {
    traceEnter(0x00009E26u);
    auto &mem = memory();

    cpu().a[6] = 0xFFFFFF0Cu; // score_p1_end
    cpu().d[7] = 0;
    cpu().setDb(7, mem.readByte(cpu().a[0] + 0x39u));
    const bool p2 = (cpu().db(7) & 0x80u) != 0;
    cpu().setDb(7, static_cast<m_byte>(cpu().db(7) & 0x7Fu));
    cpu().setFlag(CPU68K::FlagZ, !p2);
    if (p2)
        cpu().a[6] = 0xFFFFFF14u; // score_p2_end

    if (SoRSurvival::active()) {
        SoRSurvival::enemyDefeated(mem, cpu().a[0], p2 ? 1 : 0);
        cpu().ssp += 4;
        return;
    }
    add_bcd_score_value();
}
