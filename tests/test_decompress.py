from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "decompress.py"
SPEC = importlib.util.spec_from_file_location("sor_decompress", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
sor_decompress = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sor_decompress
SPEC.loader.exec_module(sor_decompress)


def _pack_msb(bits: str) -> bytes:
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[index : index + 8], 2) for index in range(0, len(bits), 8))


def _pack_lsb(bits: list[int]) -> bytes:
    value = sum(bit << index for index, bit in enumerate(bits))
    return value.to_bytes(2, "little")


class NemesisTests(unittest.TestCase):
    def test_decodes_rows_from_prefix_table(self) -> None:
        stream = bytes.fromhex("0001 0A 71 00 FF 0000")
        result = sor_decompress.decompress_nemesis(stream)
        self.assertEqual(result.data, bytes.fromhex("AA AA AA AA") * 8)
        self.assertEqual(result.consumed, len(stream))

    def test_xor_mode_reconstructs_rows(self) -> None:
        stream = bytes.fromhex("8001 0A 71 00 FF 0000")
        result = sor_decompress.decompress_nemesis(stream)
        expected = b"".join(
            bytes.fromhex("AA AA AA AA") if row % 2 == 0 else bytes(4)
            for row in range(8)
        )
        self.assertEqual(result.data, expected)

    def test_repeat_token_can_cross_a_row_boundary(self) -> None:
        # A 7-nibble token followed by an 8-nibble token crosses into row two.
        stream = bytes.fromhex("0001 0A 61 00 8B 71 01 FF 555555")
        result = sor_decompress.decompress_nemesis(stream)
        self.assertEqual(len(result.data), 32)
        self.assertEqual(result.data[:8], bytes.fromhex("AAAAAAAB BBBBBBBA"))


class EnigmaTests(unittest.TestCase):
    def test_decodes_incrementing_and_common_runs(self) -> None:
        # 00/1 => incrementing twice, 01/0 => common once, 111/F => end.
        payload = _pack_msb("00" "0001" "01" "0000" "111" "1111") + b"\x00"
        stream = bytes.fromhex("01 00 0001 0002") + payload
        result = sor_decompress.decompress_enigma(stream)
        self.assertEqual(result.data, bytes.fromhex("0001 0002 0002"))
        self.assertEqual(result.consumed, 10)

    def test_preserves_plane_header_and_applies_base_tile(self) -> None:
        plane_header = bytes.fromhex("0011223344556677")
        payload = _pack_msb("01" "0000" "111" "1111")
        stream = plane_header + bytes.fromhex("01 00 0001 0002") + payload
        result = sor_decompress.decompress_enigma(
            stream, base_tile=0x20, plane_header=True
        )
        self.assertEqual(result.data, plane_header + bytes.fromhex("0022"))


class KosinskiTests(unittest.TestCase):
    def test_decodes_literals_and_terminator(self) -> None:
        descriptor = _pack_lsb([1, 1, 0, 1])
        stream = descriptor + b"AB" + bytes.fromhex("000000")
        result = sor_decompress.decompress_kosinski(stream)
        self.assertEqual(result.data, b"AB")
        self.assertEqual(result.consumed, len(stream))

    def test_decodes_overlapping_short_match(self) -> None:
        descriptor = _pack_lsb([1, 1, 0, 0, 0, 1, 0, 1])
        stream = descriptor + b"AB" + bytes([0xFE]) + bytes.fromhex("000000")
        result = sor_decompress.decompress_kosinski(stream)
        self.assertEqual(result.data, b"ABABA")

    def test_rejects_back_reference_before_output(self) -> None:
        descriptor = _pack_lsb([0, 0, 0, 0])
        with self.assertRaises(sor_decompress.DecodeError):
            sor_decompress.decompress_kosinski(descriptor + b"\xFF")


class RomIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        rom_path = ROOT / "rom" / "SOR.bin"
        if not rom_path.exists():
            raise unittest.SkipTest("local Streets of Rage ROM is not available")
        cls.rom = rom_path.read_bytes()

    def test_known_nemesis_stream(self) -> None:
        result = sor_decompress.decompress_nemesis(self.rom, 0x1B046)
        self.assertEqual(len(result.data), 448)

    def test_known_enigma_stream(self) -> None:
        result = sor_decompress.decompress_enigma(self.rom, 0x65976)
        self.assertEqual(result.consumed, 814)
        self.assertEqual(len(result.data), 768 * 2)

    def test_known_headered_enigma_stream(self) -> None:
        result = sor_decompress.decompress_enigma(
            self.rom, 0x7228E, base_tile=1, plane_header=True
        )
        self.assertEqual(result.consumed, 20)
        self.assertEqual(len(result.data), 8 + 52 * 2)

    def test_known_kosinski_stream(self) -> None:
        result = sor_decompress.decompress_kosinski(self.rom, 0x71C6C)
        self.assertEqual(result.consumed, 656)
        self.assertEqual(len(result.data), 2248)


if __name__ == "__main__":
    unittest.main()
