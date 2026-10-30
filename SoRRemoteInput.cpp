#include "SoRRemoteInput.hpp"

#include "SoRInput.hpp"
#include "system/remote/RemoteAccess.hpp"

#include <array>
#include <vector>

namespace SoRRemoteInput {
namespace {

using SoRControlsConfig::Scheme;

constexpr std::array<std::uint8_t, 4> kInputMagic  = {'S', 'O', 'R', 'X'};
constexpr std::array<std::uint8_t, 4> kStatusMagic = {'S', 'O', 'R', 'G'};
constexpr std::size_t                 kHeaderSize  = 12;

constexpr std::uint16_t kTagPlayerInput    = 0x0001;
constexpr std::uint16_t kTagPlayerStatus   = 0x0101;

constexpr std::uint8_t kFlagQueue = 0x01;

constexpr ActionMask kAllActions = (1u << SoRControlsConfig::kActionCount) - 1u;

struct Entry {
    ActionMask    actions = 0;
    std::uint32_t frames  = 0;
};

struct Player {
    ActionMask         held = 0;
    std::uint32_t      seq  = 0;
    std::vector<Entry> active;
    std::uint64_t      activeStart = 0;
    std::uint64_t      activeEnd   = 0; // released from this frame
    std::vector<Entry> waiting;
    bool               hasWaiting = false;
    ActionMask         current    = 0;
    std::uint64_t      endFrame   = 0; // reported in PLAYER_STATUS
};

// CPU thread only.
struct State {
    std::array<Player, 2>     players{};
    std::vector<std::uint8_t> document;   // the buffer last read
    std::vector<std::uint8_t> stale;      // dropped until the buffer changes
    bool                      staleValid = false;
    std::uint64_t             lastFrame  = 0;
    std::vector<std::uint8_t> published;
};

State &state() {
    static State instance;
    return instance;
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>((bytes[at] << 8) | bytes[at + 1]);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return (static_cast<std::uint32_t>(readU16(bytes, at)) << 16) | readU16(bytes, at + 2);
}

void appendU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    appendU16(out, static_cast<std::uint16_t>(value >> 16));
    appendU16(out, static_cast<std::uint16_t>(value));
}

void appendU64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    appendU32(out, static_cast<std::uint32_t>(value >> 32));
    appendU32(out, static_cast<std::uint32_t>(value));
}

std::uint64_t scheduleLength(const std::vector<Entry> &entries) {
    std::uint64_t frames = 0;
    for (const Entry &entry : entries)
        frames += entry.frames;
    return frames;
}

void clearPlayers() {
    for (Player &player : state().players) {
        const auto seq = player.seq;
        player         = Player{};
        player.seq     = seq;
    }
}

void startSchedule(Player &player, std::vector<Entry> entries, std::uint64_t frame) {
    player.active      = std::move(entries);
    player.activeStart = frame;
    player.activeEnd   = frame + scheduleLength(player.active);
    player.endFrame    = player.activeEnd;
}

void applyPlayerInput(std::span<const std::uint8_t> payload, std::uint64_t frame) {
    if (payload.size() < 10)
        return;
    const int index = payload[0] - 1;
    if (index < 0 || index > 1)
        return;
    const std::uint8_t  flags = payload[1];
    const std::uint32_t seq   = readU32(payload, 4);
    const std::uint16_t count = readU16(payload, 8);
    if (payload.size() < 10u + count * 4u)
        return;

    Player &player = state().players[static_cast<std::size_t>(index)];
    player.held    = static_cast<ActionMask>(readU16(payload, 2) & kAllActions);
    if (seq == player.seq)
        return;
    player.seq = seq;

    std::vector<Entry> entries;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = 10 + i * 4;
        const Entry entry{static_cast<ActionMask>(readU16(payload, at) & kAllActions), readU16(payload, at + 2)};
        if (entry.frames > 0)
            entries.push_back(entry);
    }

    const bool playing = !player.active.empty() && frame <= player.activeEnd;
    if ((flags & kFlagQueue) != 0 && playing) {
        player.waiting    = std::move(entries);
        player.hasWaiting = true;
    } else {
        player.waiting.clear();
        player.hasWaiting = false;
        startSchedule(player, std::move(entries), frame);
    }
}

void applyDocument(std::span<const std::uint8_t> document, std::uint64_t frame) {
    if (document.size() < kHeaderSize || !std::equal(kInputMagic.begin(), kInputMagic.end(), document.begin()) ||
        readU16(document, 4) != kVersion)
        return;
    const std::uint16_t records = readU16(document, 6);

    std::size_t at = kHeaderSize;
    for (std::uint16_t r = 0; r < records && at + 4 <= document.size(); ++r) {
        const std::uint16_t tag    = readU16(document, at);
        const std::uint16_t length = readU16(document, at + 2);
        at += 4;
        if (at + length > document.size())
            return;
        const auto payload = document.subspan(at, length);
        if (tag == kTagPlayerInput)
            applyPlayerInput(payload, frame);
        at += length;
    }
}

void advance(Player &player, std::uint64_t frame) {
    // A waiting schedule starts after the running one and one frame of the
    // steady state (MegaDriveEnvironment's queued presses).
    if (player.hasWaiting && frame > player.activeEnd) {
        startSchedule(player, std::move(player.waiting), frame);
        player.waiting.clear();
        player.hasWaiting = false;
    }
    player.current = player.held;
    if (player.active.empty() || frame < player.activeStart || frame >= player.activeEnd)
        return;
    std::uint64_t offset = frame - player.activeStart;
    for (const Entry &entry : player.active) {
        if (offset < entry.frames) {
            player.current = entry.actions;
            return;
        }
        offset -= entry.frames;
    }
}

void publish(RemoteAccess &remote) {
    State                    &s = state();
    std::vector<std::uint8_t> out(kStatusMagic.begin(), kStatusMagic.end());
    appendU16(out, kVersion);
    appendU16(out, 2);
    appendU32(out, 0);
    for (int index = 0; index < 2; ++index) {
        const Player &player = s.players[static_cast<std::size_t>(index)];
        appendU16(out, kTagPlayerStatus);
        appendU16(out, 14);
        out.push_back(static_cast<std::uint8_t>(index + 1));
        out.push_back(SoRInput::scheme(index) == Scheme::Enhanced6 ? 2 : 1);
        appendU32(out, player.seq);
        appendU64(out, player.endFrame);
    }
    if (out != s.published) {
        s.published = out;
        remote.setGameData(out);
    }
}

} // namespace

void update(RemoteAccess &remote, std::uint64_t frame) {
    State &s = state();

    // A restart (the frame count goes back) or a client gone: what is in
    // the buffer is left over, and is dropped until it changes.
    const bool restarted = frame < s.lastFrame;
    s.lastFrame          = frame;
    if (restarted || !remote.clientConnected()) {
        if (!s.staleValid) {
            s.stale      = remote.executionData();
            s.staleValid = true;
            s.document   = s.stale;
            clearPlayers();
        }
    } else {
        std::vector<std::uint8_t> document = remote.executionData();
        if (document != s.document) {
            s.document = std::move(document);
            if (!s.staleValid || s.document != s.stale) {
                s.staleValid = false;
                applyDocument(s.document, frame);
            }
        }
    }

    for (Player &player : s.players)
        advance(player, frame);
    publish(remote);
}

ActionMask actions(int player) {
    if (player < 0 || player > 1)
        return 0;
    return state().players[static_cast<std::size_t>(player)].current;
}

} // namespace SoRRemoteInput
