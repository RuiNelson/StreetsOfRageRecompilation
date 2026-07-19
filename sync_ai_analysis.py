#!/usr/bin/env python3
"""Synchronize ai-analysis Markdown references with the analysis CSV files.

Canonical references use the form `` `$ADDRESS (label)` ``.  The address is
the stable identity: when a CSV label changes, an existing canonical reference
is rewritten from the address even if its old label no longer exists.
"""

from __future__ import annotations

import argparse
import collections
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path


CANONICAL_RE = re.compile(
    r"^\$(?P<address>[0-9A-Fa-f]+) \((?P<label>[A-Za-z_][A-Za-z0-9_]*)\)$"
)
ADDRESS_RE = re.compile(r"^\$(?P<address>[0-9A-Fa-f]+)$")
BACKTICK_RE = re.compile(r"`([^`]+)`")
TABLE_ADDRESS_RE = re.compile(
    r"^`\$(?P<address>[0-9A-Fa-f]+)`(?P<suffix>.*)$"
)
REDUNDANT_ADDRESS_RE = re.compile(
    r"(?P<reference>`\$(?P<canonical>[0-9A-Fa-f]+) "
    r"\([A-Za-z_][A-Za-z0-9_]*\)`)(?P<style>\*{0,2})\s*"
    r"\(\s*`\$(?P<repeated>[0-9A-Fa-f]+)`\s*\)"
)
REDUNDANT_REFERENCE_RE = re.compile(
    r"(?P<reference>`\$(?P<first_address>[0-9A-Fa-f]+) "
    r"\((?P<first_label>[A-Za-z_][A-Za-z0-9_]*)\)`)(?P<style>\*{0,2})"
    r"\s*\(\s*`\$(?P<second_address>[0-9A-Fa-f]+) "
    r"\((?P<second_label>[A-Za-z_][A-Za-z0-9_]*)\)`\s*\)"
)
PAIR_PATTERNS = (
    re.compile(
        r"`(?P<label>[A-Za-z_][A-Za-z0-9_]*)`\s+(?:at|@)\s+"
        r"`\$(?P<address>[0-9A-Fa-f]+)`"
    ),
    re.compile(
        r"`(?P<label>[A-Za-z_][A-Za-z0-9_]*)`\s*\(\s*"
        r"`?\$(?P<address>[0-9A-Fa-f]+)`?\s*\)"
    ),
)


@dataclass(frozen=True)
class Symbol:
    address: int
    label: str


def parse_csv(path: Path) -> list[Symbol]:
    symbols: list[Symbol] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for raw_line in stream:
            stripped = raw_line.strip()
            if not stripped or stripped.startswith((";", "#")):
                continue
            row = next(csv.reader([raw_line], skipinitialspace=True))
            if len(row) < 2:
                raise ValueError(f"{path}: malformed row: {raw_line.rstrip()}")
            try:
                address = int(row[0], 16)
            except ValueError as error:
                raise ValueError(
                    f"{path}: invalid hexadecimal address {row[0]!r}"
                ) from error
            symbols.append(Symbol(address, row[1].strip()))
    return symbols


class SymbolIndex:
    def __init__(self, symbols: list[Symbol]) -> None:
        self.by_address: dict[int, str] = {}
        label_addresses: dict[str, set[int]] = collections.defaultdict(set)
        for symbol in symbols:
            previous_label = self.by_address.get(symbol.address)
            if previous_label is not None and previous_label != symbol.label:
                raise ValueError(
                    f"address ${symbol.address:X} has both "
                    f"{previous_label!r} and {symbol.label!r}"
                )
            self.by_address[symbol.address] = symbol.label
            label_addresses[symbol.label].add(symbol.address)
        # A code routine and a RAM field can intentionally share a descriptive
        # label. Such a label needs an explicit address in prose; an unqualified
        # standalone occurrence is left alone.
        self.by_label: dict[str, int | None] = {
            label: next(iter(addresses)) if len(addresses) == 1 else None
            for label, addresses in label_addresses.items()
        }

    def normalize_document_address(self, address: int) -> int:
        """Expand the manuscripts' common 16-bit work-RAM shorthand."""
        expanded = 0xFF0000 + address
        if 0xB000 <= address <= 0xFFFF and expanded in self.by_address:
            return expanded
        return address

    def label_at(self, address: int) -> str | None:
        return self.by_address.get(self.normalize_document_address(address))

    def canonical(self, address: int, fallback: str | None = None) -> str:
        normalized = self.normalize_document_address(address)
        label = self.by_address.get(normalized, fallback)
        shown_address = normalized if normalized != address else address
        if label is None:
            return f"`${shown_address:X}`"
        return f"`${shown_address:X} ({label})`"


def split_table_row(line: str) -> list[str] | None:
    if not line.lstrip().startswith("|"):
        return None
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def is_separator(cells: list[str]) -> bool:
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)


def merge_address_symbol_tables(lines: list[str], index: SymbolIndex) -> list[str]:
    result: list[str] = []
    position = 0
    while position < len(lines):
        header = split_table_row(lines[position])
        if header is None:
            result.append(lines[position])
            position += 1
            continue

        lowered = [re.sub(r"[*_`]", "", cell).strip().lower() for cell in header]
        address_columns = [i for i, cell in enumerate(lowered) if "address" in cell]
        symbol_columns = [
            i
            for i, cell in enumerate(lowered)
            if "symbol" in cell or "proposed name" in cell
        ]
        if (
            len(address_columns) != 1
            or len(symbol_columns) != 1
            or address_columns[0] == symbol_columns[0]
        ):
            result.append(lines[position])
            position += 1
            continue

        end = position + 1
        while end < len(lines) and split_table_row(lines[end]) is not None:
            end += 1
        block = [split_table_row(line) for line in lines[position:end]]
        assert all(cells is not None for cells in block)
        rows = [cells for cells in block if cells is not None]
        address_column = address_columns[0]
        symbol_column = symbol_columns[0]
        expected_width = len(header)
        data_rows = rows[2:]
        can_merge = len(rows) >= 2 and is_separator(rows[1]) and all(
            len(row) == expected_width for row in rows
        )
        parsed_rows: list[tuple[int, str, str | None]] = []
        for row in data_rows:
            address_match = TABLE_ADDRESS_RE.fullmatch(row[address_column])
            symbol_contents = row[symbol_column].strip("`")
            canonical_symbol = CANONICAL_RE.fullmatch(symbol_contents)
            plain_symbol = (
                symbol_contents
                if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol_contents)
                else None
            )
            if not address_match:
                can_merge = False
                break
            address = int(address_match.group("address"), 16)
            expected = index.label_at(address)
            if canonical_symbol:
                symbol_address = int(canonical_symbol.group("address"), 16)
                if index.normalize_document_address(symbol_address) != index.normalize_document_address(address):
                    # For example, a Z80-local address paired with its 68000
                    # memory-map address: these are intentionally distinct.
                    can_merge = False
                    break
                plain_symbol = canonical_symbol.group("label")
            if expected is not None:
                # The address is authoritative. This is what lets one run
                # replace a stale table label after labels.csv is renamed.
                plain_symbol = expected
            parsed_rows.append((address, address_match.group("suffix"), plain_symbol))
        if not can_merge:
            result.extend(lines[position:end])
            position = end
            continue

        keep_column = min(address_column, symbol_column)
        drop_column = max(address_column, symbol_column)
        for row_number, row in enumerate(rows):
            if row_number == 0:
                row[keep_column] = "Reference"
            elif row_number == 1:
                row[keep_column] = "---"
            else:
                address, suffix, plain_symbol = parsed_rows[row_number - 2]
                if plain_symbol is not None:
                    row[keep_column] = index.canonical(address, plain_symbol) + suffix
                else:
                    # Preserve descriptive non-symbol cells such as a mapped
                    # address range labelled "(Z80 RAM window)".
                    row[keep_column] = row[address_column] + " " + row[symbol_column]
            del row[drop_column]
            result.append("| " + " | ".join(row) + " |")
        position = end
    return result


def synchronize_line(line: str, index: SymbolIndex) -> str:
    for pattern in PAIR_PATTERNS:
        def replace_pair(match: re.Match[str]) -> str:
            address = int(match.group("address"), 16)
            csv_label = index.label_at(address)
            if csv_label is None:
                return match.group(0)
            return index.canonical(address, csv_label)

        line = pattern.sub(replace_pair, line)

    def replace_backtick(match: re.Match[str]) -> str:
        contents = match.group(1)
        canonical_match = CANONICAL_RE.fullmatch(contents)
        if canonical_match:
            address = int(canonical_match.group("address"), 16)
            label = index.label_at(address)
            if label is None:
                return match.group(0)
            return index.canonical(address, label)
        raw_address = ADDRESS_RE.fullmatch(contents)
        if raw_address:
            address = int(raw_address.group("address"), 16)
            label = index.label_at(address)
            if label is not None:
                return index.canonical(address, label)
        address = index.by_label.get(contents)
        if address is not None:
            return index.canonical(address, contents)
        return match.group(0)

    line = BACKTICK_RE.sub(replace_backtick, line)

    def remove_repeated_address(match: re.Match[str]) -> str:
        canonical = int(match.group("canonical"), 16)
        repeated = int(match.group("repeated"), 16)
        if index.normalize_document_address(canonical) != index.normalize_document_address(repeated):
            return match.group(0)
        return match.group("reference") + match.group("style")

    line = REDUNDANT_ADDRESS_RE.sub(remove_repeated_address, line)

    def remove_repeated_reference(match: re.Match[str]) -> str:
        first_address = index.normalize_document_address(
            int(match.group("first_address"), 16)
        )
        second_address = index.normalize_document_address(
            int(match.group("second_address"), 16)
        )
        if (
            first_address != second_address
            or match.group("first_label") != match.group("second_label")
        ):
            return match.group(0)
        return match.group("reference") + match.group("style")

    return REDUNDANT_REFERENCE_RE.sub(remove_repeated_reference, line)


def synchronize_document(text: str, index: SymbolIndex) -> str:
    had_final_newline = text.endswith("\n")
    lines = merge_address_symbol_tables(text.splitlines(), index)
    synchronized: list[str] = []
    in_fence = False
    for line in lines:
        line = line.rstrip()
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            synchronized.append(line)
        elif in_fence:
            synchronized.append(line)
        else:
            synchronized.append(synchronize_line(line, index))
    output = "\n".join(synchronized)
    return output + ("\n" if had_final_newline else "")


def main() -> int:
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if updates are needed")
    parser.add_argument("--labels", type=Path, default=root / "code-analysis/labels.csv")
    parser.add_argument("--addresses", type=Path, default=root / "code-analysis/addresses.csv")
    parser.add_argument("documents", nargs="*", type=Path)
    args = parser.parse_args()

    symbols = parse_csv(args.labels) + parse_csv(args.addresses)
    try:
        index = SymbolIndex(symbols)
    except ValueError as error:
        parser.error(str(error))

    documents = args.documents or sorted((root / "ai-analysis").glob("*.md"))
    changed: list[Path] = []
    for document in documents:
        original = document.read_text(encoding="utf-8")
        synchronized = synchronize_document(original, index)
        if synchronized == original:
            continue
        changed.append(document)
        if not args.check:
            document.write_text(synchronized, encoding="utf-8")

    if changed:
        action = "need synchronization" if args.check else "synchronized"
        for document in changed:
            print(f"{action}: {document}")
        return 1 if args.check else 0
    print(f"analysis manuscripts are synchronized ({len(documents)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
