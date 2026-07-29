#!/usr/bin/env python3
"""Collapse runtime 68000 call logs into a SQLite call-map database."""

from __future__ import annotations

import argparse
import bisect
import csv
import sqlite3
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ADDRESS_LIMIT = 0x00FFFFFF


@dataclass(frozen=True)
class Label:
    name: str
    description: str


@dataclass
class CallMap:
    callsites: Counter[tuple[int, int]]
    targets: Counter[tuple[int, int, int]]
    total_events: int = 0

    @classmethod
    def empty(cls) -> "CallMap":
        return cls(Counter(), Counter())

    @property
    def edges(self) -> Counter[tuple[int, int]]:
        result: Counter[tuple[int, int]] = Counter()
        for (source, _callsite, target), count in self.targets.items():
            result[source, target] += count
        return result

    @property
    def addresses(self) -> set[int]:
        result: set[int] = set()
        for source, target in self.edges:
            result.add(source)
            result.add(target)
        return result


def parse_address(value: str, *, source: Path, line_number: int, field: str) -> int:
    try:
        address = int(value, 16)
    except ValueError as error:
        raise ValueError(
            f"{source}:{line_number}: invalid hexadecimal {field} address {value!r}"
        ) from error
    if not 0 <= address <= ADDRESS_LIMIT:
        raise ValueError(
            f"{source}:{line_number}: {field} address is outside the 24-bit "
            f"68000 address space: {value!r}"
        )
    return address


def read_call_logs(paths: Iterable[Path]) -> CallMap:
    call_map = CallMap.empty()
    for path in paths:
        with path.open(newline="", encoding="ascii") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != ["source", "callsite", "target"]:
                raise ValueError(
                    f"{path}: expected CSV header source,callsite,target; "
                    f"got {','.join(reader.fieldnames or [])!r}"
                )
            for line_number, row in enumerate(reader, 2):
                source = parse_address(
                    row["source"], source=path, line_number=line_number, field="source"
                )
                callsite = parse_address(
                    row["callsite"], source=path, line_number=line_number, field="callsite"
                )
                target = parse_address(
                    row["target"], source=path, line_number=line_number, field="target"
                )
                call_map.callsites[source, callsite] += 1
                call_map.targets[source, callsite, target] += 1
                call_map.total_events += 1
    return call_map


def read_labels(path: Path | None) -> dict[int, Label]:
    if path is None:
        return {}
    labels: dict[int, Label] = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for line_number, row in enumerate(csv.reader(stream), 1):
            if not row or row[0].lstrip().startswith(("#", ";")):
                continue
            if len(row) < 2:
                raise ValueError(f"{path}:{line_number}: expected address and name")
            try:
                address = int(row[0], 16)
            except ValueError as error:
                raise ValueError(
                    f"{path}:{line_number}: invalid label address {row[0]!r}"
                ) from error
            description = row[2].strip() if len(row) >= 3 else ""
            labels[address] = Label(row[1].strip(), description)
    return labels


def normalize_sources(call_map: CallMap, labels: dict[int, Label]) -> int:
    """Repair older logs that recorded a grouped C++ owner as their source."""
    label_addresses = sorted(labels)
    normalized_targets: Counter[tuple[int, int, int]] = Counter()
    normalized_events = 0
    for (source, callsite, target), count in call_map.targets.items():
        index = bisect.bisect_right(label_addresses, callsite)
        if index:
            labelled_source = label_addresses[index - 1]
            if source < labelled_source <= callsite:
                source = labelled_source
                normalized_events += count
        normalized_targets[source, callsite, target] += count

    if normalized_events:
        call_map.targets = normalized_targets
        call_map.callsites = Counter()
        for (source, callsite, _target), count in call_map.targets.items():
            call_map.callsites[source, callsite] += count
    return normalized_events


SCHEMA = """
PRAGMA foreign_keys = ON;

CREATE TABLE metadata (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE subroutine (
    address INTEGER PRIMARY KEY CHECK (address BETWEEN 0 AND 16777215),
    name TEXT NOT NULL,
    description TEXT NOT NULL
);

CREATE TABLE callsite (
    address INTEGER NOT NULL,
    source_address INTEGER NOT NULL REFERENCES subroutine(address),
    observed_count INTEGER NOT NULL CHECK (observed_count > 0),
    PRIMARY KEY (source_address, address)
);

CREATE TABLE call_edge (
    source_address INTEGER NOT NULL REFERENCES subroutine(address),
    target_address INTEGER NOT NULL REFERENCES subroutine(address),
    observed_count INTEGER NOT NULL CHECK (observed_count > 0),
    PRIMARY KEY (source_address, target_address)
);

CREATE TABLE callsite_target (
    source_address INTEGER NOT NULL,
    callsite_address INTEGER NOT NULL,
    target_address INTEGER NOT NULL REFERENCES subroutine(address),
    observed_count INTEGER NOT NULL CHECK (observed_count > 0),
    PRIMARY KEY (source_address, callsite_address, target_address),
    FOREIGN KEY (source_address, callsite_address)
        REFERENCES callsite(source_address, address)
);

CREATE INDEX call_edge_target_idx ON call_edge(target_address);
CREATE INDEX callsite_target_target_idx ON callsite_target(target_address);

CREATE VIEW subroutine_flow AS
SELECT
    printf('$%06X', edge.source_address) AS source_address,
    source.name AS source_name,
    printf('$%06X', edge.target_address) AS target_address,
    target.name AS target_name,
    edge.observed_count
FROM call_edge AS edge
JOIN subroutine AS source ON source.address = edge.source_address
JOIN subroutine AS target ON target.address = edge.target_address;

CREATE VIEW callsite_flow AS
SELECT
    printf('$%06X', relation.source_address) AS source_address,
    source.name AS source_name,
    printf('$%06X', relation.callsite_address) AS callsite_address,
    printf('$%06X', relation.target_address) AS target_address,
    target.name AS target_name,
    relation.observed_count
FROM callsite_target AS relation
JOIN subroutine AS source ON source.address = relation.source_address
JOIN subroutine AS target ON target.address = relation.target_address;
"""


def subroutine_name(address: int, labels: dict[int, Label]) -> str:
    label = labels.get(address)
    return label.name if label else f"sub_{address:06x}"


def write_database(
    path: Path,
    call_map: CallMap,
    labels: dict[int, Label],
    input_paths: Iterable[Path],
    normalized_source_events: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(path)
    try:
        with connection:
            connection.executescript(
                """
                DROP VIEW IF EXISTS callsite_flow;
                DROP VIEW IF EXISTS subroutine_flow;
                DROP TABLE IF EXISTS callsite_target;
                DROP TABLE IF EXISTS call_edge;
                DROP TABLE IF EXISTS callsite;
                DROP TABLE IF EXISTS subroutine;
                DROP TABLE IF EXISTS metadata;
                """
            )
            connection.executescript(SCHEMA)
            connection.executemany(
                "INSERT INTO metadata(key, value) VALUES (?, ?)",
                (
                    ("format_version", "1"),
                    ("total_events", str(call_map.total_events)),
                    ("normalized_source_events", str(normalized_source_events)),
                    ("input_files", "\n".join(str(path) for path in input_paths)),
                ),
            )
            connection.executemany(
                "INSERT INTO subroutine(address, name, description) VALUES (?, ?, ?)",
                (
                    (
                        address,
                        subroutine_name(address, labels),
                        labels.get(address, Label("", "")).description,
                    )
                    for address in sorted(call_map.addresses)
                ),
            )
            connection.executemany(
                """
                INSERT INTO callsite(address, source_address, observed_count)
                VALUES (?, ?, ?)
                """,
                (
                    (callsite, source, count)
                    for (source, callsite), count in sorted(call_map.callsites.items())
                ),
            )
            connection.executemany(
                """
                INSERT INTO call_edge(source_address, target_address, observed_count)
                VALUES (?, ?, ?)
                """,
                (
                    (source, target, count)
                    for (source, target), count in sorted(call_map.edges.items())
                ),
            )
            connection.executemany(
                """
                INSERT INTO callsite_target(
                    source_address, callsite_address, target_address, observed_count
                ) VALUES (?, ?, ?, ?)
                """,
                (
                    (source, callsite, target, count)
                    for (source, callsite, target), count
                    in sorted(call_map.targets.items())
                ),
            )
    finally:
        connection.close()


def build_parser() -> argparse.ArgumentParser:
    default_labels = Path(__file__).resolve().parents[1] / "code-analysis" / "labels.csv"
    parser = argparse.ArgumentParser(
        description=(
            "Collapse one or more source,callsite,target runtime CSV logs into "
            "a queryable SQLite database."
        )
    )
    parser.add_argument("logs", nargs="+", type=Path, help="runtime call-log CSV file")
    parser.add_argument(
        "--database", "-d", required=True, type=Path, help="output SQLite database"
    )
    parser.add_argument(
        "--labels",
        type=Path,
        default=default_labels,
        help=f"labels CSV used for readable names (default: {default_labels})",
    )
    parser.add_argument(
        "--trust-recorded-source",
        action="store_true",
        help=(
            "do not repair source addresses from the closest labels.csv entry; "
            "use for logs produced after the grouped-entry logger fix"
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    database_path = args.database.resolve()
    protected_paths = {path.resolve() for path in args.logs}
    protected_paths.add(args.labels.resolve())
    if database_path in protected_paths:
        parser.error("database file must not overwrite an input log or labels file")

    try:
        labels = read_labels(args.labels)
        call_map = read_call_logs(args.logs)
        normalized_source_events = (
            0 if args.trust_recorded_source else normalize_sources(call_map, labels)
        )
        write_database(
            args.database,
            call_map,
            labels,
            args.logs,
            normalized_source_events,
        )
    except (OSError, ValueError, sqlite3.Error) as error:
        print(f"call-map: {error}", file=sys.stderr)
        return 1

    print(
        f"Processed {call_map.total_events:,} call events into "
        f"{len(call_map.edges):,} unique subroutine flows and "
        f"{len(call_map.targets):,} unique callsite targets."
    )
    if normalized_source_events:
        print(
            f"Reassigned {normalized_source_events:,} events from grouped C++ "
            "owners to human-facing labelled source entries."
        )
    print(f"SQLite: {args.database}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
