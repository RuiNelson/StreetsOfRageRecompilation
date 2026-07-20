#!/usr/bin/env python3
"""Reach playable Streets of Rage gameplay using only joypad input."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence


def _import_remote_client() -> Any:
    """Import the installed client or the sibling MegaDriveEnvironment checkout."""

    try:
        import megadrive_remote
    except ModuleNotFoundError:
        source = Path(__file__).resolve().parents[2] / "MegaDriveEnvironment" / "python" / "src"
        if not source.is_dir():
            raise SystemExit(
                "megadrive_remote is not installed and the sibling "
                f"client was not found at {source}"
            )
        sys.path.insert(0, str(source))
        import megadrive_remote
    return megadrive_remote


remote = _import_remote_client()
Buttons = remote.Buttons
MegaDriveClient = remote.MegaDriveClient
MegaDriveRemoteError = remote.MegaDriveRemoteError


# Canonical names and widths come from code-analysis/addresses.csv.
GAME_STATE = 0xFFFF00                  # W
P1_CHARACTER_ID = 0xFFFF1E             # B
P1_OBJECT = 0xFFB800                   # B, 1 = active player
P1_HEALTH = 0xFFB832                   # W
SELECT_MENU_CURSOR = 0xFFB840          # W, 0 = one player
P1_CHARACTER_ID_INGAME = 0xFFB850      # B
P1_CHAR_SLOT = 0xFFB858                # W, screen order: Adam/Axel/Blaze
LEVEL_INTRO_ACTIVE = 0xFFFA1F          # B
SELECT_SCREEN_SUBSTATE = 0xFFFB0E      # W
CHAR_SELECT_SUBSTATE = 0xFFF904        # W
SOUND_MUSIC_VOICE_BANK = 0xFFF014      # L
PLAY_SE = 0xFFF00A                     # B, sound command queue slot 0

STORY_UPDATE = 0x0006
TITLE_UPDATE = 0x000A
MENU_UPDATE = 0x0012
CHARACTER_SELECT_UPDATE = 0x0022
LEVEL_INTRO_UPDATE = 0x002A
GAMEPLAY_UPDATE = 0x0016
MENU_INPUT_SUBSTATE = 0x0002
CHARACTER_INPUT_SUBSTATE = 0x0004
FULL_HEALTH = 0x0050
SPAWN_COMPLETE_SOUND_ID = 0x00A1

# Character-select screen order is Adam, Axel, Blaze. Persistent character IDs
# use the game's different Axel, Adam, Blaze order.
CHARACTERS = {
    "adam": {"slot": 0, "id": 1, "direction": None},
    "axel": {"slot": 1, "id": 0, "direction": Buttons.RIGHT},
    "blaze": {"slot": 2, "id": 2, "direction": Buttons.LEFT},
}


def _wait_ram(
    game: Any,
    observations: List[Dict[str, object]],
    label: str,
    address: int,
    expected: int,
    *,
    width: int,
    timeout_ms: int,
) -> None:
    value = game.wait_memory_equals(
        address,
        expected,
        width=width,
        timeout_ms=timeout_ms,
    )
    observations.append({"label": label, "value": f"0x{value:0{width * 2}X}"})


def _tap(game: Any, button: Any, *, timeout_ms: int) -> None:
    """Press one P1 button for one frame, then observe one released frame."""

    game.press_buttons(player1=button, frames=1, timeout_ms=timeout_ms)
    game.wait_vsync(1, timeout_ms=timeout_ms)


def reach_gameplay(
    game: Any,
    character: str,
    *,
    timeout_ms: int = 30_000,
) -> Dict[str, object]:
    """Navigate the real menus and return once the chosen character is playable."""

    choice = CHARACTERS[character]
    observations: List[Dict[str, object]] = []

    game.ping()
    game.restart_game(timeout_ms=timeout_ms)

    # The Sega logo has no Start skip. Observe the first skippable story state,
    # then use only the same joypad path a player would use.
    _wait_ram(
        game,
        observations,
        "story",
        GAME_STATE,
        STORY_UPDATE,
        width=2,
        timeout_ms=timeout_ms,
    )
    _tap(game, Buttons.START, timeout_ms=timeout_ms)

    _wait_ram(
        game,
        observations,
        "title",
        GAME_STATE,
        TITLE_UPDATE,
        width=2,
        timeout_ms=timeout_ms,
    )
    _tap(game, Buttons.START, timeout_ms=timeout_ms)

    _wait_ram(
        game,
        observations,
        "menu",
        GAME_STATE,
        MENU_UPDATE,
        width=2,
        timeout_ms=timeout_ms,
    )
    _wait_ram(
        game,
        observations,
        "menu_input",
        SELECT_SCREEN_SUBSTATE,
        MENU_INPUT_SUBSTATE,
        width=2,
        timeout_ms=timeout_ms,
    )
    _wait_ram(
        game,
        observations,
        "one_player",
        SELECT_MENU_CURSOR,
        0,
        width=2,
        timeout_ms=timeout_ms,
    )
    _tap(game, Buttons.A, timeout_ms=timeout_ms)

    _wait_ram(
        game,
        observations,
        "character_select",
        GAME_STATE,
        CHARACTER_SELECT_UPDATE,
        width=2,
        timeout_ms=timeout_ms,
    )
    _wait_ram(
        game,
        observations,
        "character_input",
        CHAR_SELECT_SUBSTATE,
        CHARACTER_INPUT_SUBSTATE,
        width=2,
        timeout_ms=timeout_ms,
    )

    direction = choice["direction"]
    if direction is not None:
        _tap(game, direction, timeout_ms=timeout_ms)
    _wait_ram(
        game,
        observations,
        f"{character}_slot",
        P1_CHAR_SLOT,
        int(choice["slot"]),
        width=2,
        timeout_ms=timeout_ms,
    )
    _tap(game, Buttons.A, timeout_ms=timeout_ms)

    # Do not skip the level intro: it performs the normal music and campaign
    # setup. RAM is observation-only throughout this script.
    _wait_ram(
        game,
        observations,
        "level_intro",
        GAME_STATE,
        LEVEL_INTRO_UPDATE,
        width=2,
        timeout_ms=timeout_ms,
    )
    _wait_ram(
        game,
        observations,
        "gameplay",
        GAME_STATE,
        GAMEPLAY_UPDATE,
        width=2,
        timeout_ms=timeout_ms,
    )
    _wait_ram(
        game,
        observations,
        "controls_enabled",
        LEVEL_INTRO_ACTIVE,
        0,
        width=1,
        timeout_ms=timeout_ms,
    )

    # The player can exist in gameplay RAM before the entrance animation has
    # finished. Its landing queues sound $A1; slot 0 remains visible in RAM
    # until sound_process_queue drains it later in the frame.
    _wait_ram(
        game,
        observations,
        "spawn_complete_sound",
        PLAY_SE,
        SPAWN_COMPLETE_SOUND_ID,
        width=1,
        timeout_ms=timeout_ms,
    )
    game.wait_vsync(2, timeout_ms=timeout_ms)

    observed_character = game.read_value(P1_CHARACTER_ID_INGAME, width=1)
    persistent_character = game.read_value(P1_CHARACTER_ID, width=1)
    observed_health = game.read_value(P1_HEALTH, width=2)
    observed_object = game.read_value(P1_OBJECT, width=1)
    observed_state = game.read_value(GAME_STATE, width=2)
    music_voice_bank = game.read_value(SOUND_MUSIC_VOICE_BANK, width=4)

    expected_id = int(choice["id"])
    if observed_state != GAMEPLAY_UPDATE:
        raise RuntimeError(f"game left gameplay state: 0x{observed_state:04X}")
    if persistent_character != expected_id or observed_character != expected_id:
        raise RuntimeError(
            f"expected {character} id {expected_id}, observed "
            f"persistent={persistent_character}, in_game={observed_character}"
        )
    if observed_object != 1 or observed_health != FULL_HEALTH:
        raise RuntimeError(
            "player was not fully initialized: "
            f"object={observed_object}, health=0x{observed_health:04X}"
        )
    if music_voice_bank == 0:
        raise RuntimeError("gameplay music did not initialize a voice bank")

    return {
        "character": character,
        "character_id": observed_character,
        "game_state": f"0x{observed_state:04X}",
        "health": observed_health,
        "music_voice_bank": f"0x{music_voice_bank:08X}",
        "observations": observations,
        "player_object_type": observed_object,
    }


def positive_int(value: str) -> int:
    parsed = int(value, 10)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def port_number(value: str) -> int:
    parsed = int(value, 10)
    if not 1 <= parsed <= 65_535:
        raise argparse.ArgumentTypeError("must be in 1..65535")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Navigate a running Streets of Rage recompilation to playable "
            "one-player gameplay using only joypad input and RAM observations."
        )
    )
    parser.add_argument("character", type=str.lower, choices=tuple(CHARACTERS))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=port_number, default=6969)
    parser.add_argument(
        "--timeout-ms",
        type=positive_int,
        default=30_000,
        help="timeout for each observed transition (default: 30000)",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        with MegaDriveClient(args.host, args.port) as game:
            result = reach_gameplay(
                game,
                args.character,
                timeout_ms=args.timeout_ms,
            )
    except (MegaDriveRemoteError, OSError, TimeoutError, RuntimeError, ValueError) as error:
        print(f"reach_gameplay: {error}", file=sys.stderr)
        return 1

    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
