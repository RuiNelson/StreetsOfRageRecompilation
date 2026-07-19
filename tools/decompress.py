#!/usr/bin/env python3
"""Offline Nemesis, Enigma, and Kosinski decompressor for the SoR ROM."""

from __future__ import annotations

import argparse
import binascii
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence


DEFAULT_MAX_OUTPUT = 16 * 1024 * 1024
DEFAULT_TILE_PALETTE = (
    (0x00, 0x00, 0x00),
    (0x24, 0x24, 0x3A),
    (0x3C, 0x4A, 0x78),
    (0x58, 0x72, 0xA5),
    (0x6A, 0xA9, 0xD6),
    (0x7D, 0xE0, 0xDC),
    (0x55, 0xB8, 0x6A),
    (0xA2, 0xD4, 0x59),
    (0xF1, 0xDF, 0x62),
    (0xF2, 0xA6, 0x4B),
    (0xD9, 0x62, 0x48),
    (0xA8, 0x3E, 0x55),
    (0xC4, 0x59, 0x91),
    (0xDD, 0x84, 0xB5),
    (0xC9, 0xC9, 0xD4),
    (0xFF, 0xFF, 0xFF),
)


class DecodeError(ValueError):
    """A compressed stream is truncated or malformed."""


@dataclass(frozen=True)
class DecodeResult:
    data: bytes
    consumed: int


def render_4bpp_tiles_png(
    data: bytes,
    *,
    columns: int = 16,
    scale: int = 2,
    palette: Sequence[tuple[int, int, int]] = DEFAULT_TILE_PALETTE,
) -> tuple[bytes, int, int]:
    """Render Mega Drive 8x8, 4-bpp tiles as an indexed PNG tile sheet."""

    if not data or len(data) % 32:
        raise DecodeError(
            "PNG rendering requires a non-empty whole number of 32-byte tiles"
        )
    if columns <= 0 or scale <= 0:
        raise DecodeError("PNG columns and scale must be greater than zero")
    if len(palette) != 16 or any(len(color) != 3 for color in palette):
        raise DecodeError("PNG rendering requires exactly 16 RGB palette entries")

    tile_count = len(data) // 32
    tile_columns = min(columns, tile_count)
    tile_rows = (tile_count + tile_columns - 1) // tile_columns
    source_width = tile_columns * 8
    source_height = tile_rows * 8
    rows = [bytearray(source_width) for _ in range(source_height)]

    for tile_index in range(tile_count):
        tile_x = (tile_index % tile_columns) * 8
        tile_y = (tile_index // tile_columns) * 8
        tile = data[tile_index * 32 : (tile_index + 1) * 32]
        for y in range(8):
            for byte_index, packed in enumerate(tile[y * 4 : y * 4 + 4]):
                rows[tile_y + y][tile_x + byte_index * 2] = packed >> 4
                rows[tile_y + y][tile_x + byte_index * 2 + 1] = packed & 0x0F

    width = source_width * scale
    height = source_height * scale
    raw = bytearray()
    for row in rows:
        scaled_row = bytearray()
        for pixel in row:
            scaled_row.extend([pixel] * scale)
        scanline = b"\x00" + scaled_row
        for _ in range(scale):
            raw.extend(scanline)

    def chunk(kind: bytes, payload: bytes) -> bytes:
        checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)

    header = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    palette_bytes = bytes(component for color in palette for component in color)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"PLTE", palette_bytes)
        + chunk(b"IDAT", zlib.compress(bytes(raw), level=9))
        + chunk(b"IEND", b"")
    )
    return png, width, height


class _ByteReader:
    def __init__(self, source: bytes, offset: int) -> None:
        if not 0 <= offset <= len(source):
            raise DecodeError(
                f"source offset 0x{offset:X} is outside the {len(source)}-byte input"
            )
        self.source = source
        self.start = offset
        self.pos = offset

    def read_u8(self) -> int:
        if self.pos >= len(self.source):
            raise DecodeError(f"compressed stream is truncated at 0x{self.pos:X}")
        value = self.source[self.pos]
        self.pos += 1
        return value

    def read_be16(self) -> int:
        return (self.read_u8() << 8) | self.read_u8()

    def read_le16(self) -> int:
        return self.read_u8() | (self.read_u8() << 8)


class _MsbBitReader:
    def __init__(self, reader: _ByteReader) -> None:
        self.reader = reader
        self.buffer = 0
        self.bits = 0

    def _fill(self, count: int) -> None:
        while self.bits < count:
            self.buffer = (self.buffer << 8) | self.reader.read_u8()
            self.bits += 8

    def peek(self, count: int) -> int:
        self._fill(count)
        return (self.buffer >> (self.bits - count)) & ((1 << count) - 1)

    def read(self, count: int) -> int:
        value = self.peek(count)
        self.bits -= count
        self.buffer &= (1 << self.bits) - 1 if self.bits else 0
        return value


class _KosinskiDescriptorReader:
    """LSB-first descriptor reader with the cartridge's eager refill rule."""

    def __init__(self, reader: _ByteReader) -> None:
        self.reader = reader
        self.descriptor = reader.read_le16()
        self.remaining = 16

    def read(self) -> int:
        bit = self.descriptor & 1
        self.descriptor >>= 1
        self.remaining -= 1
        # The 68000 reloads after bit 16 before reading that token's data.
        if self.remaining == 0:
            self.descriptor = self.reader.read_le16()
            self.remaining = 16
        return bit


def _check_output_limit(size: int, limit: int) -> None:
    if size > limit:
        raise DecodeError(
            f"decoded output exceeds the configured limit of {limit} bytes"
        )


def decompress_nemesis(
    source: bytes, offset: int = 0, *, max_output: int = DEFAULT_MAX_OUTPUT
) -> DecodeResult:
    """Decode one Nemesis stream at *offset*."""

    reader = _ByteReader(source, offset)
    header = reader.read_be16()
    xor_mode = bool(header & 0x8000)
    tile_count = header & 0x7FFF
    _check_output_limit(tile_count * 32, max_output)

    table: list[tuple[int, int] | None] = [None] * 256
    group = reader.read_u8()
    while group != 0xFF:
        pixel = group & 0x0F
        while True:
            descriptor = reader.read_u8()
            if descriptor >= 0x80:
                group = descriptor
                break
            length = descriptor & 0x0F
            if not 1 <= length <= 8:
                raise DecodeError(f"invalid Nemesis prefix length {length}")
            token = (descriptor & 0x70) | pixel
            code = reader.read_u8()
            if code >= (1 << length):
                raise DecodeError(
                    f"Nemesis prefix 0x{code:X} does not fit in {length} bits"
                )
            first = code << (8 - length)
            for index in range(first, first + (1 << (8 - length))):
                if table[index] is not None:
                    raise DecodeError(f"overlapping Nemesis prefix at 0x{index:02X}")
                table[index] = (length, token)

    bits = _MsbBitReader(reader)
    output = bytearray()
    previous_row = 0
    rows_remaining = tile_count * 8
    row = 0
    nibbles = 0
    while rows_remaining:
        prefix = bits.peek(8)
        if prefix >= 0xFC:
            bits.read(6)
            token = bits.read(7)
        else:
            entry = table[prefix]
            if entry is None:
                raise DecodeError(f"undefined Nemesis prefix 0x{prefix:02X}")
            length, token = entry
            bits.read(length)

        value = token & 0x0F
        for _ in range((token >> 4) + 1):
            row = (row << 4) | value
            nibbles += 1
            if nibbles != 8:
                continue

            if xor_mode:
                row ^= previous_row
                previous_row = row
            output.extend(row.to_bytes(4, "big"))
            rows_remaining -= 1
            if not rows_remaining:
                break
            row = 0
            nibbles = 0

    return DecodeResult(bytes(output), reader.pos - offset)


def decompress_enigma(
    source: bytes,
    offset: int = 0,
    *,
    base_tile: int = 0,
    plane_header: bool = False,
    max_output: int = DEFAULT_MAX_OUTPUT,
) -> DecodeResult:
    """Decode Enigma, optionally preserving its 8-byte plane header."""

    if not 0 <= base_tile <= 0xFFFF:
        raise DecodeError("Enigma base tile must be between 0 and 0xFFFF")
    reader = _ByteReader(source, offset)
    output = bytearray()
    if plane_header:
        for _ in range(8):
            output.append(reader.read_u8())
        _check_output_limit(len(output), max_output)

    index_bits = reader.read_u8()
    attribute_mask = reader.read_u8()
    if index_bits > 16:
        raise DecodeError(f"invalid Enigma tile-index width {index_bits}")
    incrementing = (reader.read_be16() + base_tile) & 0xFFFF
    common = (reader.read_be16() + base_tile) & 0xFFFF
    bits = _MsbBitReader(reader)

    def emit(word: int) -> None:
        _check_output_limit(len(output) + 2, max_output)
        output.extend((word & 0xFFFF).to_bytes(2, "big"))

    def inline_word() -> int:
        word = base_tile
        for mask_bit, tile_bit in zip(
            (0x10, 0x08, 0x04, 0x02, 0x01),
            (0x8000, 0x4000, 0x2000, 0x1000, 0x0800),
        ):
            if attribute_mask & mask_bit and bits.read(1):
                # Match the original instruction sequence: palette bits use
                # ADD while priority and flip bits use OR.
                if tile_bit in (0x4000, 0x2000):
                    word = (word + tile_bit) & 0xFFFF
                else:
                    word |= tile_bit
        if index_bits:
            word = (word + bits.read(index_bits)) & 0xFFFF
        return word

    while True:
        if bits.read(1) == 0:
            opcode = bits.read(1)
        else:
            opcode = 4 + bits.read(2)
        count = bits.read(4)
        if opcode == 7 and count == 0x0F:
            if reader.pos & 1:
                reader.read_u8()
            return DecodeResult(bytes(output), reader.pos - offset)

        run = count + 1
        if opcode == 0:
            for _ in range(run):
                emit(incrementing)
                incrementing = (incrementing + 1) & 0xFFFF
        elif opcode == 1:
            for _ in range(run):
                emit(common)
        elif opcode == 4:
            word = inline_word()
            for _ in range(run):
                emit(word)
        elif opcode == 5:
            word = inline_word()
            for _ in range(run):
                emit(word)
                word = (word + 1) & 0xFFFF
        elif opcode == 6:
            word = inline_word()
            for _ in range(run):
                emit(word)
                word = (word - 1) & 0xFFFF
        elif opcode == 7:
            for _ in range(run):
                emit(inline_word())


def decompress_kosinski(
    source: bytes, offset: int = 0, *, max_output: int = DEFAULT_MAX_OUTPUT
) -> DecodeResult:
    """Decode one Kosinski stream at *offset*."""

    reader = _ByteReader(source, offset)
    descriptor = _KosinskiDescriptorReader(reader)
    output = bytearray()

    def copy_match(displacement: int, length: int) -> None:
        if displacement >= 0 or -displacement > len(output):
            raise DecodeError(
                f"invalid Kosinski back-reference {displacement} at output "
                f"offset 0x{len(output):X}"
            )
        _check_output_limit(len(output) + length, max_output)
        for _ in range(length):
            output.append(output[len(output) + displacement])

    while True:
        if descriptor.read():
            _check_output_limit(len(output) + 1, max_output)
            output.append(reader.read_u8())
            continue

        if descriptor.read() == 0:
            length = ((descriptor.read() << 1) | descriptor.read()) + 2
            encoded = reader.read_u8()
            copy_match(encoded - 0x100, length)
            continue

        low = reader.read_u8()
        high = reader.read_u8()
        encoded_offset = 0xE000 | ((high & 0xF8) << 5) | low
        displacement = encoded_offset - 0x10000
        length_code = high & 0x07
        if length_code:
            copy_match(displacement, length_code + 2)
            continue

        extension = reader.read_u8()
        if extension == 0:
            return DecodeResult(bytes(output), reader.pos - offset)
        if extension == 1:
            continue
        copy_match(displacement, extension + 1)


DECODERS: dict[str, Callable[..., DecodeResult]] = {
    "nemesis": decompress_nemesis,
    "enigma": decompress_enigma,
    "kosinski": decompress_kosinski,
}


def _integer(value: str) -> int:
    text = value.strip()
    if text.startswith("$"):
        text = "0x" + text[1:]
    try:
        return int(text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {value!r}") from error


def _positive_integer(value: str) -> int:
    result = _integer(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("value must be greater than zero")
    return result


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Decompress a game blob directly from the Streets of Rage ROM."
    )
    parser.add_argument("format", choices=DECODERS, help="compression format")
    parser.add_argument("offset", type=_integer, help="ROM offset, e.g. 0x1B046 or $1B046")
    parser.add_argument("output", type=Path, help="decoded output file")
    parser.add_argument(
        "--rom", type=Path, default=Path("rom/SOR.bin"), help="ROM path (default: rom/SOR.bin)"
    )
    parser.add_argument("--base-tile", type=_integer, default=0, help="Enigma base tile word")
    parser.add_argument(
        "--plane-header", action="store_true", help="Enigma: preserve its 8-byte plane header"
    )
    parser.add_argument(
        "--max-output",
        type=_positive_integer,
        default=DEFAULT_MAX_OUTPUT,
        help="maximum decoded size (default: 16 MiB)",
    )
    parser.add_argument(
        "--png",
        action="store_true",
        help="write decoded bytes as a Mega Drive 4-bpp tile-sheet PNG",
    )
    parser.add_argument(
        "--columns",
        type=_positive_integer,
        default=16,
        help="tiles per PNG row (default: 16)",
    )
    parser.add_argument(
        "--scale",
        type=_positive_integer,
        default=2,
        help="nearest-neighbour PNG scale (default: 2)",
    )
    parser.add_argument("--json", action="store_true", help="print metadata as JSON")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    if args.format != "enigma" and (args.base_tile or args.plane_header):
        parser.error("--base-tile and --plane-header are only valid for Enigma")

    try:
        rom = args.rom.read_bytes()
        decoder = DECODERS[args.format]
        options = {"max_output": args.max_output}
        if args.format == "enigma":
            options.update(base_tile=args.base_tile, plane_header=args.plane_header)
        result = decoder(rom, args.offset, **options)
        output_data = result.data
        png_dimensions = None
        if args.png:
            output_data, width, height = render_4bpp_tiles_png(
                result.data, columns=args.columns, scale=args.scale
            )
            png_dimensions = [width, height]
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(output_data)
    except (DecodeError, OSError) as error:
        parser.exit(1, f"error: {error}\n")

    metadata = {
        "format": args.format,
        "offset": args.offset,
        "consumed": result.consumed,
        "output_size": len(result.data),
        "file_size": len(output_data),
        "output": str(args.output),
    }
    if png_dimensions is not None:
        metadata["png_dimensions"] = png_dimensions
    if args.json:
        print(json.dumps(metadata, sort_keys=True))
    else:
        summary = (
            f"{args.format}: 0x{args.offset:X} -> {args.output} "
            f"({result.consumed} compressed bytes, {len(result.data)} decoded bytes"
        )
        if png_dimensions is not None:
            summary += f", {png_dimensions[0]}x{png_dimensions[1]} PNG"
        print(summary + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
