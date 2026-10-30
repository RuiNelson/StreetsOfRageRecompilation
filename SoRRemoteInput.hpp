#pragma once

#include "SoRControlsConfig.hpp"

#include <cstdint>

class RemoteAccess;

// Remote (autoplay) input, as actions, through the remote-access
// execution-data buffer -- the game no longer reads the Mega Drive buttons
// a client sends with the gamepad commands.
//
// Both directions are binary, big-endian, and built to grow: a container
// `magic:4, version:u16, record count:u16, serial:u32`, then records
// `tag:u16, length:u16, payload`; an unknown tag is skipped. Mirrored by
// autoplay's `sor_autoplay/sor_remote.py` (Python `struct`); change both
// together.
//
// Client -> game (SET_EXECUTION_DATA, magic "SORX"):
//   0x0001 PLAYER_INPUT  player:u8 (1/2), flags:u8, held:u16, seq:u32,
//                        count:u16, then count x (actions:u16, frames:u16)
//     `held` is the player's steady state. A new `seq` starts the schedule
//     (actions for that many sampled frames each) on the first frame the
//     game samples it, in place of `held`; with flags bit 0 (QUEUE) a
//     schedule arriving while another plays waits for it to end and for one
//     frame of `held` after it (a fresh edge), and only the newest waiting
//     one is kept -- MegaDriveEnvironment's QUEUE_PRESS_BUTTONS.
// Game -> client (RemoteAccess::setGameData, GET_GAME_DATA, magic "SORG"):
//   0x0101 PLAYER_STATUS player:u8, scheme:u8 (1 ORIGINAL 3, 2 ENHANCED 6:
//                        the player's own, from controls.yaml -- a client
//                        reads it and adapts, it never sets it), seq:u32 (the last one
//                        applied), end frame:u64 (the frame its schedule is
//                        released on; 0 when none has played)
//
// Actions are SoRControlsConfig::Action bits. Input from a client that has
// disconnected, or left over from before a restart, is dropped until the
// buffer changes.
namespace SoRRemoteInput {

using SoRControlsConfig::ActionMask;

constexpr std::uint16_t kVersion = 1;

/// CPU thread, once per sampled frame (`sample_all_joypads`), with the
/// game-uptime frame count: reads the execution data, advances both
/// players' schedules and publishes their status.
void update(RemoteAccess &remote, std::uint64_t frame);

/// The actions the client holds for @p player (0 or 1) on this frame.
ActionMask actions(int player);

} // namespace SoRRemoteInput
