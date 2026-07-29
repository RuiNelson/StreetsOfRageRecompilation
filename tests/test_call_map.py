import json
import sqlite3
import threading
import urllib.request
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.call_map import build_web_server, load_web_data, main


def test_call_map_collapses_events_and_preserves_callsites():
    with TemporaryDirectory() as directory:
        root = Path(directory)
        log = root / "calls.csv"
        labels = root / "labels.csv"
        database = root / "calls.sqlite"
        log.write_text(
            "source,callsite,target\n"
            "000100,000110,000200\n"
            "000100,000110,000200\n"
            "000100,000118,000300\n"
            "000200,000210,000300\n",
            encoding="ascii",
        )
        labels.write_text(
            "; label-file comment\n"
            "00000100,source_routine,source\n"
            "00000200,middle_routine,middle\n"
            "00000300,target_routine,target\n",
            encoding="utf-8",
        )

        assert main(
            [
                str(log),
                "--database",
                str(database),
                "--labels",
                str(labels),
            ]
        ) == 0

        with sqlite3.connect(database) as connection:
            assert connection.execute(
                """
                SELECT source_address, target_address, observed_count
                FROM call_edge ORDER BY source_address, target_address
                """
            ).fetchall() == [
                (0x100, 0x200, 2),
                (0x100, 0x300, 1),
                (0x200, 0x300, 1),
            ]
            assert connection.execute(
                """
                SELECT source_address, callsite_address, target_address,
                       observed_count
                FROM callsite_target
                ORDER BY source_address, callsite_address, target_address
                """
            ).fetchall() == [
                (0x100, 0x110, 0x200, 2),
                (0x100, 0x118, 0x300, 1),
                (0x200, 0x210, 0x300, 1),
            ]


def test_old_grouped_owner_is_normalized_to_closest_labelled_source():
    with TemporaryDirectory() as directory:
        root = Path(directory)
        log = root / "calls.csv"
        labels = root / "labels.csv"
        database = root / "calls.sqlite"
        log.write_text(
            "source,callsite,target\n"
            "000100,000310,000400\n",
            encoding="ascii",
        )
        labels.write_text(
            "00000300,human_entry,human entry\n"
            "00000400,target,target\n",
            encoding="utf-8",
        )

        assert main(
            [
                str(log),
                "--database",
                str(database),
                "--labels",
                str(labels),
            ]
        ) == 0

        with sqlite3.connect(database) as connection:
            assert connection.execute(
                "SELECT source_address, target_address FROM call_edge"
            ).fetchone() == (0x300, 0x400)
            assert connection.execute(
                "SELECT value FROM metadata WHERE key = 'normalized_source_events'"
            ).fetchone() == ("1",)


def test_labelled_dynamic_source_is_not_replaced_by_a_later_label():
    with TemporaryDirectory() as directory:
        root = Path(directory)
        log = root / "calls.csv"
        labels = root / "labels.csv"
        database = root / "calls.sqlite"
        log.write_text(
            "source,callsite,target\n"
            "019D16,01A0A8,072914\n",
            encoding="ascii",
        )
        labels.write_text(
            "00019D16,vblank_handler,VBlank handler\n"
            "00019DA6,vblank_helper,VBlank helper\n"
            "00072914,sound_engine,Sound engine\n",
            encoding="utf-8",
        )

        assert main(
            [str(log), "--database", str(database), "--labels", str(labels)]
        ) == 0

        with sqlite3.connect(database) as connection:
            assert connection.execute(
                "SELECT source_address, target_address FROM call_edge"
            ).fetchone() == (0x19D16, 0x72914)
            assert connection.execute(
                "SELECT value FROM metadata WHERE key = 'normalized_source_events'"
            ).fetchone() == ("0",)


def test_web_data_and_http_server_expose_labelled_flows():
    with TemporaryDirectory() as directory:
        root = Path(directory)
        log = root / "calls.csv"
        labels = root / "labels.csv"
        database = root / "calls.sqlite"
        log.write_text(
            "source,callsite,target\n"
            "000100,000110,000200\n"
            "000100,000110,000200\n",
            encoding="ascii",
        )
        labels.write_text(
            "00000100,player_update,Updates the player\n"
            "00000200,collision_check,Checks collision\n",
            encoding="utf-8",
        )
        assert main(
            [str(log), "--database", str(database), "--labels", str(labels)]
        ) == 0

        payload = load_web_data(database)
        assert payload["summary"] == {
            "events": 2,
            "subroutines": 2,
            "flows": 1,
            "callsites": 1,
        }
        assert payload["flows"][0] == {
            "source": "$000100",
            "sourceName": "player_update",
            "target": "$000200",
            "targetName": "collision_check",
            "count": 2,
            "callsites": [{"address": "$000110", "count": 2}],
        }

        server = build_web_server(database, "127.0.0.1", 0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            base_url = f"http://127.0.0.1:{server.server_port}"
            with urllib.request.urlopen(f"{base_url}/", timeout=2) as response:
                page = response.read().decode()
                assert response.status == 200
                assert "Call <span>map</span>" in page
                assert 'class="routine-link"' in page
                assert "r.address.slice(1).toLowerCase()===q" in page
            with urllib.request.urlopen(f"{base_url}/api/data", timeout=2) as response:
                api_payload = json.load(response)
                assert api_payload["flows"][0]["sourceName"] == "player_update"
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
