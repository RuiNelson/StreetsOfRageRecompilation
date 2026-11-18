#pragma once

#include <filesystem>

class SystemMemory;

// EXTRA > SURVIVAL MODE (user): one or two players, 3 lives each and no continue, at the start of a round
// chosen for its background, against regular enemies that never stop coming: random types and colours, more
// of them at once as they fall, and now and then food or a weapon left behind. Each enemy defeated is a point
// for the player who struck the last blow, shown as that player's score; no clock, no police. The best result
// for one and for two players is kept in `survival.yaml`, next to controls.yaml.
//
// The enemies come through the ROM's own deferred spawn ($936 select_deferred_spawn, $B76
// load_deferred_spawn_art_and_spawn), from a section of records this module keeps filled in the ELC buffer:
// the ROM loads each family's art and each colour's palette into its residency slots, places the enemy at a
// screen edge, and its death path ($9D8C) gives the slots back, the spawn credit and the point
// (ordinary_enemy_award_score, manual here).
namespace SoRSurvival {

/// `survival.yaml`, in controls.yaml's folder.
std::filesystem::path defaultPath();

/// The best results, read once from the file (0 when there is none).
struct Records {
    unsigned onePlayer  = 0;
    unsigned twoPlayers = 0; // the two players' points added up
};
const Records &records();

/// How the last survival game ended, for the EXTRA menu.
struct Result {
    int      players  = 1;
    unsigned points[2] = {0, 0};
    unsigned total    = 0;
    unsigned record   = 0; // the record now, this game's total if it beat it
    bool     newRecord = false;
};

/// The EXTRA menu's START: a survival game for @p players (1 or 2) with round @p round (0-7) behind it. Sets
/// up the mode and goes to character select (game state $20), as the mode menu's 1 PLAYER and 2 PLAYERS do.
void begin(SystemMemory &memory, int players, int round);

/// A survival game is being played (from begin until the game is over).
bool active();

/// The round chosen last, to show again in the menu (0-7).
int lastRound();
/// The number of players chosen last (1 or 2).
int lastPlayers();

/// initialize_player_continues: 3 lives and no continue for each player.
void setUpPlayers(SystemMemory &memory);

/// Inside spawn_round_players, once the round has loaded: the first wave's objects go, the camera stays where
/// the round starts and the level pipeline waits for the first enemies (onFrame starts them).
void setUpRound(SystemMemory &memory);

/// CPU thread, at each frame boundary (both vblank waits): the clock, the police, the enemies to come and
/// the end of the game.
void onFrame(SystemMemory &memory);

/// ordinary_enemy_award_score, during a survival game: the enemy at @p enemy is about to be removed after its
/// death, and @p player (0 or 1) struck the last blow. A point for the player, and sometimes an item.
void enemyDefeated(SystemMemory &memory, unsigned enemy, int player);

} // namespace SoRSurvival
