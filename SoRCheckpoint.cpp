#include "SoRCheckpoint.hpp"

#include "SoRControlsConfig.hpp"
#include "SoRPython.hpp"
#include "SoRRemoteInput.hpp"
#include "SoR.hpp"

#include "system/memory/SystemMemory.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace SoRCheckpoint {
namespace {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr int kFileVersion = 1;

// Persistent fields at the top of work RAM (init_levelstart keeps them).
constexpr u32 kGameState      = 0xFFFFFF00u;
constexpr u32 kLevel          = 0xFFFFFF02u;
constexpr u32 kWave           = 0xFFFFFF04u;
constexpr u32 kScoreP1        = 0xFFFFFF08u; // long; the long after it holds the high-score name
constexpr u32 kPlayerMode     = 0xFFFFFF18u;
constexpr u32 kP1Continues    = 0xFFFFFF1Au; // word, P2 at +2
constexpr u32 kP1CharacterId  = 0xFFFFFF1Eu; // byte, P2 at +1
constexpr u32 kP1Lives        = 0xFFFFFF20u; // P2's three bytes at +3: lives, specials, out flag
constexpr u32 kP1ExtraLife    = 0xFFFFFF26u; // lives_via_points_ptr, byte, P2 at +1
constexpr u32 kDemoMode       = 0xFFFFFF2Au;
constexpr u32 kSpecialRestart = 0xFFFFFF34u;
constexpr u32 kFlagFF36       = 0xFFFFFF36u;
constexpr u32 kPlayerModeCopy = 0xFFFFFF38u;
constexpr u32 kDifficulty     = 0xFFFFFFC6u;

// The level pipeline and what it shares with the camera ($5F4-$E38, $19570).
constexpr u32 kLevelSpawnFlowFlags   = 0xFFFFFA05u;
constexpr u32 kWaveAdvancePending    = 0xFFFFFA0Du;
constexpr u32 kLevelIntroActive      = 0xFFFFFA1Fu;
constexpr u32 kDeferredSpawnCredits  = 0xFFFFFA70u;
constexpr u32 kLevelFlowFlags        = 0xFFFFFA72u;
constexpr u32 kSpawnXBase            = 0xFFFFFB12u;
constexpr u32 kStatusBitfield1       = 0xFFFFFB14u; // low byte: the pipeline state
constexpr u32 kActiveProgression     = 0xFFFFFB1Eu;
constexpr u32 kSpawnXStride          = 0xFFFFFB20u;
constexpr u32 kElcSpawnStreamCursor  = 0xFFFFFC14u;
constexpr u32 kElcBuffer             = 0x00FF6800u;
constexpr u32 kElcBufferSize         = 0x400u; // the longest decoded stream is round 8's $380
constexpr u32 kCameras               = 0xFFFFE000u; // primary $E000 and secondary $E100, $100 bytes each
constexpr std::size_t kCamerasSize   = 0x200;

constexpr u32 kP1Object      = 0xFFFFB800u;
constexpr u32 kP2Object      = 0xFFFFB880u;
constexpr u32 kObjectTable   = 0xFFFFB900u;
constexpr int kObjectSlots   = 32;
constexpr u32 kObjectSize    = 0x80u;
constexpr u32 kObjectHealth  = 0x32u;
constexpr u8  kObjectPlayer  = 0x01u;
constexpr u16 kFullHealth    = 0x50u;

constexpr u16 kStateIngame     = 0x16u;
constexpr u16 kStateRoundIntro = 0x28u;
constexpr u16 kStateRoundIntroUpdate = 0x2Au;
// Pipeline state $0C (clear_object_table) does nothing while boss_forced_reaction_flags bit 0 is clear: the
// pipeline waits there until the intro fade is over.
constexpr u16 kPipelineIdle = 0x000Cu;

// The HUD, as draw_player_lives_and_specials ($4E14) and adjust_player_health ($4E6C) write it.
constexpr u32 kHudLives       = 0xFFFF6020u;
constexpr u32 kHudHealthBar   = 0xFFFF605Au;
constexpr u32 kHudHealthTile  = 0xFFFFFC0Cu;
constexpr u32 kHealthBarsP1   = 0x00004ED8u;
constexpr u32 kHealthBarsP2   = 0x00004F02u;
constexpr u32 kHealthPartials = 0x00004F2Cu;

struct Player {
    u8  character = 0;
    u32 score     = 0;
    u16 continues = 0;
    u8  lives     = 0;
    u8  specials  = 0;
    u8  out       = 0;
    u8  extraLife = 0;
    u16 health    = kFullHealth;
};

struct Checkpoint {
    u16  level          = 0;
    u16  wave           = 0;
    bool insideRound    = false; // false: the round's start, which the ROM's own round load restores
    u8   playerMode     = 1;
    u8   playerModeCopy = 0;
    bool aiAssist       = false;
    u16  difficulty     = 1;
    std::array<Player, 2> players{};
    // Inside the round only: where the camera, the stream and the batch columns were at the gate.
    std::array<u8, kCamerasSize> cameras{};
    u32 streamOffset   = 0;
    u16 spawnXBase     = 0;
    u16 spawnXStride   = 0;
    u8  levelFlowFlags = 0;
};

enum class Phase { None, AwaitSpawn, AwaitPlayers, AwaitFade };

std::optional<Checkpoint> last;      // the last wave start of the game under way
std::optional<Checkpoint> restoring; // the checkpoint being loaded
Phase                     phase = Phase::None;
std::optional<bool>       fileThere;
u16                       previousGameState = 0;
u16                       previousPipelineState = 0;
std::optional<Checkpoint> candidate; // the gate being read, until init_level_state is seen to run

u32 objectOf(int player) { return player == 0 ? kP1Object : kP2Object; }

Player readPlayer(SystemMemory &memory, int player) {
    const u32 p = static_cast<u32>(player);
    Player    data;
    data.character = memory.readByte(kP1CharacterId + p);
    data.score     = memory.readLong(kScoreP1 + 8 * p);
    data.continues = memory.readWord(kP1Continues + 2 * p);
    data.lives     = memory.readByte(kP1Lives + 3 * p);
    data.specials  = memory.readByte(kP1Lives + 3 * p + 1);
    data.out       = memory.readByte(kP1Lives + 3 * p + 2);
    data.extraLife = memory.readByte(kP1ExtraLife + p);
    const u32 object = objectOf(player);
    const u16 health = memory.readWord(object + kObjectHealth);
    // A player down or between lives starts the wave with a new one: full health.
    data.health = memory.readByte(object) == kObjectPlayer && health > 0 && health <= kFullHealth ? health : kFullHealth;
    return data;
}

Checkpoint readCommon(SystemMemory &memory) {
    Checkpoint cp;
    cp.level          = memory.readWord(kLevel);
    cp.wave           = memory.readWord(kWave);
    cp.playerMode     = memory.readByte(kPlayerMode);
    cp.playerModeCopy = memory.readByte(kPlayerModeCopy);
    cp.aiAssist       = SoRRemoteInput::aiAssist();
    cp.difficulty     = memory.readWord(kDifficulty);
    for (int player = 0; player < 2; ++player)
        cp.players[static_cast<std::size_t>(player)] = readPlayer(memory, player);
    return cp;
}

Checkpoint readInsideRound(SystemMemory &memory) {
    Checkpoint cp  = readCommon(memory);
    cp.insideRound = true;
    for (std::size_t i = 0; i < kCamerasSize; ++i)
        cp.cameras[i] = memory.readByte(kCameras + static_cast<u32>(i));
    cp.streamOffset   = (memory.readLong(kElcSpawnStreamCursor) & 0x00FFFFFFu) - kElcBuffer;
    cp.spawnXBase     = memory.readWord(kSpawnXBase);
    cp.spawnXStride   = memory.readWord(kSpawnXStride);
    cp.levelFlowFlags = memory.readByte(kLevelFlowFlags);
    return cp;
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
    memory.writeWord(static_cast<u32>(hud + second), specials);
    memory.writeWord(static_cast<u32>(hud + second + 0x50), static_cast<u16>(specials + 1));
}

// The bar half of adjust_player_health ($4E88), for a health already in range.
void drawHealthBar(SystemMemory &memory, int player, u16 health) {
    const u32 whole     = health / 8u;
    const u32 remainder = health % 8u;
    const u32 source    = (player == 0 ? kHealthBarsP1 : kHealthBarsP2) + (10u - whole) * 2u;
    const u32 target    = kHudHealthBar + (player == 0 ? 0u : 0x34u);
    for (u32 i = 0; i < 20; ++i)
        memory.writeByte(target + i, memory.readByte(source + i));
    memory.writeLong(kHudHealthTile + (player == 0 ? 0u : 4u), kHealthPartials + remainder * 4u);
}

// --- The file ---

std::string toHex(const u8 *bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string           text;
    text.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 0x0F];
    }
    return text;
}

bool fromHex(const std::string &text, u8 *bytes, std::size_t size) {
    if (text.size() != size * 2)
        return false;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < size; ++i) {
        const int high = nibble(text[i * 2]);
        const int low  = nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        bytes[i] = static_cast<u8>(high * 16 + low);
    }
    return true;
}

bool write(const Checkpoint &cp) {
    YAML::Node root;
    root["version"]          = kFileVersion;
    root["round"]            = cp.level + 1;
    root["wave"]             = cp.wave;
    root["inside_round"]     = cp.insideRound;
    root["player_mode"]      = static_cast<int>(cp.playerMode);
    root["player_mode_copy"] = static_cast<int>(cp.playerModeCopy);
    root["ai_assist"]        = cp.aiAssist;
    root["difficulty"]       = cp.difficulty;
    YAML::Node players(YAML::NodeType::Sequence);
    for (const Player &p : cp.players) {
        YAML::Node node;
        node["character"]  = static_cast<int>(p.character);
        node["score"]      = toHex(std::array<u8, 4>{static_cast<u8>(p.score >> 24), static_cast<u8>(p.score >> 16),
                                                     static_cast<u8>(p.score >> 8), static_cast<u8>(p.score)}
                                       .data(),
                                   4);
        node["continues"]  = p.continues;
        node["lives"]      = static_cast<int>(p.lives);
        node["specials"]   = static_cast<int>(p.specials);
        node["out"]        = static_cast<int>(p.out);
        node["extra_life"] = static_cast<int>(p.extraLife);
        node["health"]     = p.health;
        players.push_back(node);
    }
    root["players"] = players;
    if (cp.insideRound) {
        root["stream_offset"]    = cp.streamOffset;
        root["spawn_x_base"]     = cp.spawnXBase;
        root["spawn_x_stride"]   = cp.spawnXStride;
        root["level_flow_flags"] = static_cast<int>(cp.levelFlowFlags);
        root["cameras"]          = toHex(cp.cameras.data(), cp.cameras.size());
    }

    YAML::Emitter emitter;
    emitter << root;

    const std::filesystem::path path = defaultPath();
    std::error_code             error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "# Streets of Rage checkpoint (pause menu > SAVE AND QUIT). Used once by CONTINUE.\n"
            << emitter.c_str() << '\n';
        if (!out)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

std::optional<Checkpoint> read() {
    const std::filesystem::path path = defaultPath();
    try {
        const YAML::Node root = YAML::LoadFile(path.string());
        if (!root.IsMap() || root["version"].as<int>() != kFileVersion)
            return std::nullopt;
        Checkpoint cp;
        const int  round  = root["round"].as<int>();
        const int  wave   = root["wave"].as<int>();
        const int  mode   = root["player_mode"].as<int>();
        const int  copy   = root["player_mode_copy"].as<int>();
        const int  diff   = root["difficulty"].as<int>();
        if (round < 1 || round > 8 || wave < 0 || wave > 8 || mode < 1 || mode > 3 || copy < 0 || copy > 3 || diff < 0 ||
            diff > 3)
            return std::nullopt;
        cp.level          = static_cast<u16>(round - 1);
        cp.wave           = static_cast<u16>(wave);
        cp.insideRound    = root["inside_round"].as<bool>();
        cp.playerMode     = static_cast<u8>(mode);
        cp.playerModeCopy = static_cast<u8>(copy);
        cp.aiAssist       = root["ai_assist"].as<bool>();
        cp.difficulty     = static_cast<u16>(diff);
        const YAML::Node players = root["players"];
        if (!players.IsSequence() || players.size() != 2)
            return std::nullopt;
        for (std::size_t i = 0; i < 2; ++i) {
            const YAML::Node node = players[i];
            Player          &p    = cp.players[i];
            const int        character = node["character"].as<int>();
            const int        lives     = node["lives"].as<int>();
            const int        specials  = node["specials"].as<int>();
            const int        health    = node["health"].as<int>();
            const int        continues = node["continues"].as<int>();
            if (character < 0 || character > 2 || lives < 0 || lives > 0xFF || specials < 0 || specials > 0xFF ||
                health < 1 || health > kFullHealth || continues < 0 || continues > 0xFFFF)
                return std::nullopt;
            std::array<u8, 4> score{};
            if (!fromHex(node["score"].as<std::string>(), score.data(), score.size()))
                return std::nullopt;
            p.character = static_cast<u8>(character);
            p.score     = static_cast<u32>(score[0]) << 24 | static_cast<u32>(score[1]) << 16 |
                      static_cast<u32>(score[2]) << 8 | score[3];
            p.continues = static_cast<u16>(continues);
            p.lives     = static_cast<u8>(lives);
            p.specials  = static_cast<u8>(specials);
            p.out       = static_cast<u8>(node["out"].as<int>());
            p.extraLife = static_cast<u8>(node["extra_life"].as<int>());
            p.health    = static_cast<u16>(health);
        }
        if (cp.insideRound) {
            cp.streamOffset   = root["stream_offset"].as<u32>();
            cp.spawnXBase     = root["spawn_x_base"].as<u16>();
            cp.spawnXStride   = root["spawn_x_stride"].as<u16>();
            cp.levelFlowFlags = static_cast<u8>(root["level_flow_flags"].as<int>());
            if (cp.streamOffset >= kElcBufferSize || cp.streamOffset % 2 != 0 ||
                !fromHex(root["cameras"].as<std::string>(), cp.cameras.data(), cp.cameras.size()))
                return std::nullopt;
        }
        return cp;
    } catch (const YAML::Exception &exception) {
        std::fprintf(stderr, "Could not read %s (%s)\n", path.string().c_str(), exception.what());
        return std::nullopt;
    }
}

void erase() {
    std::error_code error;
    std::filesystem::remove(defaultPath(), error);
    fileThere = false;
}

void finishRestore() {
    last      = restoring;
    restoring.reset();
    phase = Phase::None;
}

} // namespace

std::filesystem::path defaultPath() {
    return SoRControlsConfig::defaultPath().parent_path() / "checkpoint.yaml";
}

bool available() {
    if (!fileThere) {
        std::error_code error;
        fileThere = std::filesystem::exists(defaultPath(), error);
    }
    return *fileThere;
}

void onFrame(SystemMemory &memory) {
    const u16 gameState = memory.readWord(kGameState);
    const u16 before    = previousGameState;
    previousGameState   = gameState;

    if (restoring) {
        if (phase == Phase::AwaitSpawn && gameState == kStateRoundIntroUpdate)
            phase = Phase::AwaitPlayers; // the round loaded without spawn_round_players seeing it: nothing to move
        if (phase == Phase::AwaitPlayers && gameState == kStateRoundIntroUpdate) {
            // The players' own initializer ran inside the round load: full health and the round's police call.
            // Put back what they had at the gate, and the HUD the ROM drew from the new values.
            bool ready = true;
            for (int player = 0; player < 2; ++player)
                if ((restoring->playerMode & (1u << player)) != 0 && memory.readByte(objectOf(player)) != kObjectPlayer)
                    ready = false;
            if (!ready)
                return;
            for (int player = 0; player < 2; ++player) {
                if ((restoring->playerMode & (1u << player)) == 0)
                    continue;
                const Player &p = restoring->players[static_cast<std::size_t>(player)];
                memory.writeByte(kP1Lives + 3u * static_cast<u32>(player) + 1u, p.specials);
                memory.writeWord(objectOf(player) + kObjectHealth, p.health);
                drawHealthBar(memory, player, p.health);
                drawLivesAndSpecials(memory, player);
            }
            if (restoring->insideRound)
                phase = Phase::AwaitFade;
            else
                finishRestore();
            return;
        }
        if (phase == Phase::AwaitFade && gameState == kStateIngame && memory.readByte(kLevelIntroActive) == 0) {
            // The fade-in is over (it copies the target palette, so init_level_state's palettes would not last
            // through it): the pipeline goes on as it does after a wave, at init_level_state.
            memory.writeWord(kStatusBitfield1, 0);
            finishRestore();
        }
        return;
    }

    if (memory.readByte(kDemoMode) != 0)
        return;

    // The round has just loaded: its start is the first checkpoint.
    if (gameState == kStateRoundIntroUpdate && before != kStateRoundIntroUpdate) {
        last = readCommon(memory);
        return;
    }

    // A wave was cleared. The pipeline drains it in state $08, $0A or $0C, and init_level_state ($5F4) runs
    // within the frame that ends it (state $00 never lasts to a frame boundary), so the gate is read on every
    // frame of those states and kept once $5F4 is seen to have run: state $12 without the round start's bit 10,
    // or $0E (the boss's arrival). The stream cursor, the batch columns and the wave are then still what $5F4
    // read (the camera adds one to the wave only once the next wave's art is in), and the camera is at the gate.
    const u16  pipe     = memory.readWord(kStatusBitfield1);
    const u16  low      = static_cast<u16>(pipe & 0x00FFu);
    const u16  previousLow = previousPipelineState;
    previousPipelineState = low;
    const bool playing  = gameState == kStateIngame && last && last->level == memory.readWord(kLevel) &&
                         memory.readByte(kLevelIntroActive) == 0;
    if (!playing) {
        candidate.reset();
        return;
    }
    if (low == 0x00u || low == 0x08u || low == 0x0Au || low == 0x0Cu) {
        candidate = readInsideRound(memory);
        return;
    }
    const bool initLevelStateRan = (low == 0x12u && (pipe & 0x0400u) == 0) || low == 0x0Eu;
    if (initLevelStateRan && candidate && (previousLow == 0x0Cu || previousLow == 0x0Au || previousLow == 0x08u || previousLow == 0x00u) &&
        candidate->wave == memory.readWord(kWave) && (!last->insideRound || last->wave != candidate->wave))
        last = candidate;
    candidate.reset();
}

bool save() {
    if (!last)
        return false;
    if (!write(*last)) {
        std::fprintf(stderr, "Could not write %s\n", defaultPath().string().c_str());
        return false;
    }
    fileThere = true;
    return true;
}

bool beginContinue(SystemMemory &memory) {
    std::optional<Checkpoint> cp = read();
    erase(); // used once, and an unusable one is gone too
    if (!cp)
        return false;

    // Without the AI's code now, its seat stays empty.
    if (cp->aiAssist && !SoRPython::available()) {
        cp->aiAssist   = false;
        cp->playerMode = static_cast<u8>(cp->playerMode & 1u ? 1u : cp->playerMode);
    }

    memory.writeWord(kLevel, cp->level);
    memory.writeWord(kWave, 0);
    memory.writeByte(kPlayerMode, cp->playerMode);
    memory.writeByte(kPlayerModeCopy, cp->playerModeCopy);
    memory.writeWord(kDifficulty, cp->difficulty);
    memory.writeByte(kDemoMode, 0);
    memory.writeByte(kSpecialRestart, 0);
    memory.writeByte(kFlagFF36, 0);
    for (int player = 0; player < 2; ++player) {
        const u32     p    = static_cast<u32>(player);
        const Player &data = cp->players[p];
        memory.writeByte(kP1CharacterId + p, data.character);
        memory.writeLong(kScoreP1 + 8 * p, data.score);
        memory.writeLong(kScoreP1 + 8 * p + 4, 0);
        memory.writeWord(kP1Continues + 2 * p, data.continues);
        memory.writeByte(kP1Lives + 3 * p, data.lives);
        memory.writeByte(kP1Lives + 3 * p + 1, data.specials);
        memory.writeByte(kP1Lives + 3 * p + 2, data.out);
        memory.writeByte(kP1ExtraLife + p, data.extraLife);
    }

    SoRRemoteInput::setAiAssist(cp->aiAssist);
    if (cp->aiAssist)
        SoRPython::start(memory);

    restoring = cp;
    phase     = Phase::AwaitSpawn;
    memory.writeWord(kGameState, kStateRoundIntro);
    return true;
}

bool restoreInsideRound(SystemMemory &memory) {
    if (!restoring || phase != Phase::AwaitSpawn)
        return false;
    phase = Phase::AwaitPlayers;
    if (!restoring->insideRound || memory.readWord(kLevel) != restoring->level)
        return false;

    const Checkpoint &cp = *restoring;
    for (std::size_t i = 0; i < kCamerasSize; ++i)
        memory.writeByte(kCameras + static_cast<u32>(i), cp.cameras[i]);

    // start_round_setup has just created the first wave's objects, none of which has run yet. The round's
    // scenery stays: round 3's two palms ($4E) and round 7's elevator props ($50-$52). Round 7's $53 goes: it
    // starts the first climb (wave_advance_pending), which init_level_state's art step asks for here.
    for (int slot = 0; slot < kObjectSlots; ++slot) {
        const u32 object = kObjectTable + static_cast<u32>(slot) * kObjectSize;
        const u8  type   = memory.readByte(object);
        if (type == 0 || type == 0x4E || (type >= 0x50 && type <= 0x52))
            continue;
        for (u32 i = 0; i < kObjectSize; i += 4)
            memory.writeLong(object + i, 0);
    }
    memory.writeWord(kActiveProgression, 0);

    memory.writeWord(kWave, cp.wave);
    memory.writeLong(kElcSpawnStreamCursor, kElcBuffer + cp.streamOffset);
    memory.writeWord(kSpawnXBase, cp.spawnXBase);
    memory.writeWord(kSpawnXStride, cp.spawnXStride);
    memory.writeByte(kLevelFlowFlags, cp.levelFlowFlags);
    memory.writeByte(kLevelSpawnFlowFlags, 0);
    memory.writeByte(kDeferredSpawnCredits, 0);
    memory.writeByte(kWaveAdvancePending, 0);
    memory.writeByte(kPlayerModeCopy, cp.playerModeCopy);
    memory.writeWord(kStatusBitfield1, kPipelineIdle);
    return true;
}

} // namespace SoRCheckpoint

// ---------------------------------------------------------------------------
// $0107F2 — spawn_round_players
//
// Creates the active players' objects, as the ROM does, after a checkpoint inside the round has put the camera
// back (SoRCheckpoint): the players' own initializer, on this frame, places them at the camera's X, and the
// planes are drawn again there with the round load's initial draw ($19AF4).
// ---------------------------------------------------------------------------
void StreetsOfRage::spawn_round_players(m_long /*entry_*/) {
    traceEnter(0x000107F2u);
    auto &mem = memory();

    if (SoRCheckpoint::restoreInsideRound(mem)) {
        const CPU68K saved = cpu();
        const auto   draw  = [&](m_long camera, m_long blockmap, m_long queue) {
            cpu().a[1] = camera;
            cpu().a[2] = blockmap;
            cpu().a[3] = queue;
            logCallFromReturn(0x000107F2u, 0x00019AF4u);
            cpu().ssp -= 4;
            mem.writeLong(cpu().ssp, 0x000107F2u);
            initialize_visible_tilemap_plane();
        };
        draw(0xFFFFE000u, 0x00FF0000u, 0xFFFFEA00u);
        draw(0xFFFFE100u, 0x00FF2000u, 0xFFFFEB20u);
        cpu() = saved;
    }

    const m_byte playerMode = mem.readByte(0xFFFFFF18u);
    bool         zero       = (playerMode & 0x01u) == 0;
    if (!zero) {
        mem.writeByte(0xFFFFB850u, mem.readByte(0xFFFFFF1Eu)); // p1_character_id_ingame
        mem.writeByte(0xFFFFB800u, 1);
        mem.writeByte(0xFFFFFF21u, 2);
        mem.writeByte(0xFFFFB859u, static_cast<m_byte>(mem.readByte(0xFFFFB859u) | 0x04u));
        const m_byte flags = mem.readByte(0xFFFFB84Bu);
        zero               = (flags & 0x08u) == 0;
        mem.writeByte(0xFFFFB84Bu, static_cast<m_byte>(flags | 0x08u));
    }
    const bool second = (playerMode & 0x02u) != 0;
    if (second) {
        mem.writeByte(0xFFFFB8D0u, mem.readByte(0xFFFFFF1Fu)); // p2_character_id_ingame
        mem.writeByte(0xFFFFB880u, 1);
        mem.writeByte(0xFFFFFF24u, 2);
        mem.writeByte(0xFFFFB8D9u, static_cast<m_byte>(mem.readByte(0xFFFFB8D9u) | 0x04u));
        const m_byte flags = mem.readByte(0xFFFFB8CBu);
        zero               = (flags & 0x08u) == 0;
        mem.writeByte(0xFFFFB8CBu, static_cast<m_byte>(flags | 0x08u));
    } else {
        zero = true;
    }
    cpu().setFlag(CPU68K::FlagZ, zero);
    cpu().ssp += 4;
}
