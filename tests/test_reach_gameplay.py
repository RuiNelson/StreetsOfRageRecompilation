from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "reach_gameplay.py"
SPEC = importlib.util.spec_from_file_location("reach_gameplay", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
reach_gameplay = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = reach_gameplay
SPEC.loader.exec_module(reach_gameplay)


class FakeGame:
    def __init__(self) -> None:
        self.memory = {}
        self.operations = []

    def ping(self) -> None:
        self.operations.append(("ping",))

    def restart_game(self, *, timeout_ms: int) -> None:
        self.operations.append(("restart", timeout_ms))

    def press_buttons(self, *, player1: object, frames: int, timeout_ms: int) -> None:
        self.operations.append(("press", int(player1), frames, timeout_ms))

    def wait_vsync(self, count: int, *, timeout_ms: int) -> None:
        self.operations.append(("vsync", count, timeout_ms))

    def wait_memory_equals(
        self,
        address: int,
        expected: int,
        *,
        width: int,
        timeout_ms: int,
    ) -> int:
        self.operations.append(("wait", address, expected, width, timeout_ms))
        self.memory[address] = expected
        if address == reach_gameplay.P1_CHAR_SLOT:
            self.memory[reach_gameplay.P1_CHARACTER_ID] = {0: 1, 1: 0, 2: 2}[expected]
        if address == reach_gameplay.GAME_STATE and expected == reach_gameplay.GAMEPLAY_UPDATE:
            self.memory[reach_gameplay.P1_CHARACTER_ID_INGAME] = self.memory[
                reach_gameplay.P1_CHARACTER_ID
            ]
            self.memory[reach_gameplay.P1_OBJECT] = 1
            self.memory[reach_gameplay.P1_HEALTH] = reach_gameplay.FULL_HEALTH
            self.memory[reach_gameplay.SOUND_MUSIC_VOICE_BANK] = 0x00070000
        return expected

    def read_value(self, address: int, *, width: int) -> int:
        return self.memory[address]


class ReachGameplayTests(unittest.TestCase):
    def test_all_characters_use_only_expected_joypad_taps(self) -> None:
        expected_buttons = {
            "adam": [
                reach_gameplay.Buttons.START,
                reach_gameplay.Buttons.START,
                reach_gameplay.Buttons.A,
                reach_gameplay.Buttons.A,
            ],
            "axel": [
                reach_gameplay.Buttons.START,
                reach_gameplay.Buttons.START,
                reach_gameplay.Buttons.A,
                reach_gameplay.Buttons.RIGHT,
                reach_gameplay.Buttons.A,
            ],
            "blaze": [
                reach_gameplay.Buttons.START,
                reach_gameplay.Buttons.START,
                reach_gameplay.Buttons.A,
                reach_gameplay.Buttons.LEFT,
                reach_gameplay.Buttons.A,
            ],
        }

        for character, buttons in expected_buttons.items():
            with self.subTest(character=character):
                game = FakeGame()
                result = reach_gameplay.reach_gameplay(game, character)
                observed_buttons = [
                    operation[1]
                    for operation in game.operations
                    if operation[0] == "press"
                ]

                self.assertEqual(observed_buttons, [int(button) for button in buttons])
                self.assertEqual(result["character"], character)
                self.assertEqual(result["game_state"], "0x0016")
                self.assertEqual(result["health"], 0x50)
                self.assertEqual(result["music_voice_bank"], "0x00070000")
                self.assertIn(
                    (
                        "wait",
                        reach_gameplay.PLAY_SE,
                        reach_gameplay.SPAWN_COMPLETE_SOUND_ID,
                        1,
                        30_000,
                    ),
                    game.operations,
                )

    def test_fake_client_exposes_no_memory_write_api(self) -> None:
        game = FakeGame()
        self.assertFalse(hasattr(game, "write_value"))
        self.assertFalse(hasattr(game, "write_memory"))
        reach_gameplay.reach_gameplay(game, "axel")


if __name__ == "__main__":
    unittest.main()
