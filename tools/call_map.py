#!/usr/bin/env python3
"""Collapse runtime 68000 call logs into a SQLite call-map database."""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import sqlite3
import sys
from collections import Counter
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable
from urllib.parse import urlsplit


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
        # Current logs record the dynamic 68000 entry and labels.csv identifies
        # it directly. Only apply the legacy approximation to anonymous grouped
        # C++ owners; otherwise a later label inside a non-contiguous routine
        # can incorrectly replace a valid source such as vblank_handler.
        if source not in labels:
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


def format_address(address: int) -> str:
    return f"${address:06X}"


def load_web_data(path: Path) -> dict[str, object]:
    """Load the immutable browser payload from a generated call-map database."""
    with sqlite3.connect(path) as connection:
        metadata = dict(connection.execute("SELECT key, value FROM metadata"))
        subroutines = [
            {
                "address": format_address(address),
                "name": name,
                "description": description,
                "incoming": incoming,
                "outgoing": outgoing,
            }
            for address, name, description, incoming, outgoing in connection.execute(
                """
                SELECT routine.address, routine.name, routine.description,
                       COUNT(DISTINCT incoming.source_address),
                       COUNT(DISTINCT outgoing.target_address)
                FROM subroutine AS routine
                LEFT JOIN call_edge AS incoming
                    ON incoming.target_address = routine.address
                LEFT JOIN call_edge AS outgoing
                    ON outgoing.source_address = routine.address
                GROUP BY routine.address
                ORDER BY routine.address
                """
            )
        ]
        callsites: dict[tuple[int, int], list[dict[str, object]]] = {}
        for source, callsite, target, count in connection.execute(
            """
            SELECT source_address, callsite_address, target_address, observed_count
            FROM callsite_target
            ORDER BY source_address, target_address, callsite_address
            """
        ):
            callsites.setdefault((source, target), []).append(
                {"address": format_address(callsite), "count": count}
            )
        flows = [
            {
                "source": format_address(source),
                "sourceName": source_name,
                "target": format_address(target),
                "targetName": target_name,
                "count": count,
                "callsites": callsites.get((source, target), []),
            }
            for source, source_name, target, target_name, count in connection.execute(
                """
                SELECT edge.source_address, source.name,
                       edge.target_address, target.name, edge.observed_count
                FROM call_edge AS edge
                JOIN subroutine AS source ON source.address = edge.source_address
                JOIN subroutine AS target ON target.address = edge.target_address
                ORDER BY edge.observed_count DESC, edge.source_address,
                         edge.target_address
                """
            )
        ]
    return {
        "summary": {
            "events": int(metadata.get("total_events", "0")),
            "subroutines": len(subroutines),
            "flows": len(flows),
            "callsites": sum(len(flow["callsites"]) for flow in flows),
        },
        "subroutines": subroutines,
        "flows": flows,
    }


WEB_PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Streets of Rage call map</title>
<style>
:root{color-scheme:dark;--bg:#080b10;--panel:#111722;--line:#263247;
--muted:#8b9ab4;--text:#eef4ff;--accent:#ffca28;--cyan:#64d8ff}
*{box-sizing:border-box} body{margin:0;background:radial-gradient(circle at 75% 0,
#172136 0,#080b10 38rem);color:var(--text);font:14px/1.45 ui-monospace,
SFMono-Regular,Menlo,monospace} header,main{width:min(1500px,calc(100% - 32px));
margin:auto} header{padding:32px 0 18px} h1{font:700 clamp(24px,4vw,42px)/1.05
system-ui;margin:0 0 8px;letter-spacing:-.04em} h1 span{color:var(--accent)}
.subtitle,.muted{color:var(--muted)} .stats{display:grid;
grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin:22px 0}
.stat,.panel{background:color-mix(in srgb,var(--panel) 92%,transparent);
border:1px solid var(--line);border-radius:10px;box-shadow:0 16px 48px #0005}
.stat{padding:15px}.stat strong{display:block;font-size:23px;color:var(--cyan)}
.stat small{color:var(--muted);text-transform:uppercase;letter-spacing:.1em}
.toolbar{display:flex;gap:10px;margin:0 0 14px} input{width:100%;padding:12px 14px;
border:1px solid var(--line);border-radius:8px;background:#090e17;color:var(--text);
font:inherit;outline:none}input:focus{border-color:var(--cyan)}
.panel{padding:18px;margin-bottom:16px}.panel h2{font:650 15px system-ui;
letter-spacing:.03em;margin:0 0 14px}.focus{display:grid;
grid-template-columns:minmax(0,1fr) minmax(260px,.8fr) minmax(0,1fr);gap:14px;
align-items:center;min-height:190px}.lane{display:grid;gap:7px;align-content:center}
.lane-label{color:var(--muted);font-size:11px;text-transform:uppercase;
letter-spacing:.12em}.node{border:1px solid var(--line);border-radius:7px;
background:#0b111c;padding:8px 10px;cursor:pointer;text-align:left;color:var(--text);
font:inherit;width:100%}.node:hover{border-color:var(--cyan)}.node b{display:block;
overflow:hidden;text-overflow:ellipsis}.node small{color:var(--muted)}
.center{padding:20px;text-align:center;border:1px solid var(--accent);
border-radius:10px;background:#171708}.center b{display:block;font-size:17px;
overflow-wrap:anywhere}.center code{color:var(--accent)}
.empty{color:var(--muted);padding:10px;border:1px dashed var(--line);
border-radius:7px}.table-wrap{overflow:auto;max-height:52vh}
table{border-collapse:collapse;width:100%;white-space:nowrap}th,td{padding:10px 12px;
border-bottom:1px solid var(--line);text-align:left}th{position:sticky;top:0;
background:var(--panel);color:var(--muted);font-size:11px;text-transform:uppercase;
letter-spacing:.08em}tbody tr:hover{background:#1a2434}
.callsites{white-space:normal;color:var(--muted)}
.routine-link{border:0;background:none;color:var(--cyan);font:inherit;padding:0;
cursor:pointer;text-align:left}.routine-link:hover{text-decoration:underline}
.count{text-align:right;font-variant-numeric:tabular-nums}
@media(max-width:800px){.stats{grid-template-columns:1fr 1fr}.focus{
grid-template-columns:1fr}.center{grid-row:1}.toolbar{display:block}}
</style>
</head>
<body>
<header><h1>Call <span>map</span></h1>
<div class="subtitle">Observed 68000 subroutine flows, collapsed by route and callsite.</div>
<div class="stats" id="stats"></div></header>
<main>
<div class="toolbar"><input id="search" type="search"
placeholder="Search label or address…"></div>
<section class="panel"><h2 id="focus-title">Subroutine neighbourhood</h2>
<div class="focus"><div class="lane" id="incoming"></div>
<div class="center" id="center"></div><div class="lane" id="outgoing"></div></div>
</section>
<section class="panel"><h2>Possible flows <span class="muted" id="shown"></span></h2>
<div class="table-wrap"><table><thead><tr><th>Source</th><th>Target</th>
<th>Callsites</th><th class="count">Observed</th></tr></thead>
<tbody id="flows"></tbody></table></div></section>
</main>
<script>
const fmt=n=>new Intl.NumberFormat().format(n);
const esc=s=>{const e=document.createElement("span");e.textContent=s;return e.innerHTML};
let data,selected;
function button(r,f){return `<button class="node" data-address="${r.address}">
<b>${esc(r.name)}</b><small>${r.address} · ${fmt(f.count)} calls</small></button>`}
function select(address){
 selected=address;
 const routine=data.subroutines.find(r=>r.address===address);
 if(!routine)return;
 const ins=data.flows.filter(f=>f.target===address).slice(0,8);
 const outs=data.flows.filter(f=>f.source===address).slice(0,8);
 document.getElementById("center").innerHTML=`<b>${esc(routine.name)}</b>
 <code>${routine.address}</code><div class="muted">${esc(routine.description||"No description")}</div>`;
 document.getElementById("incoming").innerHTML=`<div class="lane-label">Called by</div>`+
 (ins.map(f=>button(data.subroutines.find(r=>r.address===f.source),f)).join("")||
 '<div class="empty">No incoming calls observed</div>');
 document.getElementById("outgoing").innerHTML=`<div class="lane-label">Calls</div>`+
 (outs.map(f=>button(data.subroutines.find(r=>r.address===f.target),f)).join("")||
 '<div class="empty">No outgoing calls observed</div>');
 document.querySelectorAll(".node").forEach(n=>n.onclick=()=>select(n.dataset.address));
}
function renderFlows(query=""){
 const q=query.trim().toLowerCase();
 const rows=data.flows.filter(f=>!q||[f.source,f.sourceName,f.target,f.targetName,
 ...f.callsites.map(c=>c.address)].some(v=>v.toLowerCase().includes(q)));
 const exact=data.subroutines.filter(r=>r.name.toLowerCase()===q||
 r.address.toLowerCase()===q||r.address.slice(1).toLowerCase()===q);
 if(exact.length===1)select(exact[0].address);
 document.getElementById("shown").textContent=`(${fmt(rows.length)} shown)`;
 document.getElementById("flows").innerHTML=rows.map(f=>`<tr>
 <td><button class="routine-link" data-address="${f.source}">${esc(f.sourceName)}
 <span class="muted">${f.source}</span></button></td>
 <td><button class="routine-link" data-address="${f.target}">${esc(f.targetName)}
 <span class="muted">${f.target}</span></button></td>
 <td class="callsites">${f.callsites.map(c=>`${c.address} × ${fmt(c.count)}`).join("<br>")}</td>
 <td class="count">${fmt(f.count)}</td></tr>`).join("");
 document.querySelectorAll(".routine-link").forEach(
 r=>r.onclick=()=>select(r.dataset.address));
}
fetch("/api/data").then(r=>{if(!r.ok)throw Error(r.statusText);return r.json()})
.then(payload=>{data=payload;const s=data.summary;
 document.getElementById("stats").innerHTML=[["Events",s.events],["Subroutines",s.subroutines],
 ["Flows",s.flows],["Callsites",s.callsites]].map(([k,v])=>
 `<div class="stat"><strong>${fmt(v)}</strong><small>${k}</small></div>`).join("");
 selected=[...data.subroutines].sort((a,b)=>b.outgoing-a.outgoing)[0]?.address;
 renderFlows();if(selected)select(selected);
}).catch(e=>document.body.textContent=`Could not load call map: ${e}`);
document.getElementById("search").addEventListener("input",e=>renderFlows(e.target.value));
</script>
</body></html>
"""


def build_web_server(
    database_path: Path, host: str, port: int
) -> ThreadingHTTPServer:
    payload = json.dumps(load_web_data(database_path), separators=(",", ":")).encode()
    page = WEB_PAGE.encode()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            path = urlsplit(self.path).path
            if path == "/":
                body, content_type = page, "text/html; charset=utf-8"
            elif path == "/api/data":
                body, content_type = payload, "application/json; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header(
                "Content-Security-Policy",
                "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'",
            )
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, message: str, *args: object) -> None:
            print(f"call-map web: {message % args}")

    return ThreadingHTTPServer((host, port), Handler)


def serve_database(database_path: Path, host: str, port: int) -> None:
    server = build_web_server(database_path, host, port)
    actual_host, actual_port = server.server_address[:2]
    visible_host = "127.0.0.1" if actual_host in ("0.0.0.0", "::") else actual_host
    print(f"Web viewer: http://{visible_host}:{actual_port}")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping web viewer.")
    finally:
        server.server_close()


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
            "do not approximate anonymous legacy source addresses from the "
            "closest labels.csv entry"
        ),
    )
    parser.add_argument(
        "--port",
        type=int,
        help="start the local web viewer on this TCP port after generating the database",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="web viewer bind address (default: 127.0.0.1)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.port is not None and not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
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
    if args.port is not None:
        try:
            serve_database(database_path, args.host, args.port)
        except (OSError, ValueError, sqlite3.Error) as error:
            print(f"call-map: cannot start web viewer: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
