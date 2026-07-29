import sqlite3
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.call_map import main


def test_call_map_collapses_events_and_preserves_callsites():
    with TemporaryDirectory() as directory:
        root = Path(directory)
        log = root / "calls.csv"
        labels = root / "labels.csv"
        database = root / "calls.sqlite"
        mermaid = root / "calls.mmd"
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
                "--mermaid",
                str(mermaid),
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

        diagram = mermaid.read_text(encoding="utf-8")
        assert 'n000100["source_routine<br/>$000100"]' in diagram
        assert 'n000100 -->|"$000110 × 2 &#124; total × 2"| n000200' in diagram


def test_mermaid_root_and_depth_do_not_filter_database():
    with TemporaryDirectory() as directory:
        root = Path(directory)
        log = root / "calls.csv"
        labels = root / "labels.csv"
        database = root / "calls.sqlite"
        mermaid = root / "calls.mmd"
        log.write_text(
            "source,callsite,target\n"
            "000100,000110,000200\n"
            "000200,000210,000300\n"
            "000400,000410,000500\n",
            encoding="ascii",
        )
        labels.write_text("", encoding="utf-8")

        assert main(
            [
                str(log),
                "--database",
                str(database),
                "--mermaid",
                str(mermaid),
                "--labels",
                str(labels),
                "--root",
                "100",
                "--max-depth",
                "1",
            ]
        ) == 0

        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT count(*) FROM call_edge").fetchone() == (3,)

        diagram = mermaid.read_text(encoding="utf-8")
        assert "n000100" in diagram
        assert "n000200" in diagram
        assert "n000300" not in diagram
        assert "n000400" not in diagram


def test_old_grouped_owner_is_normalized_to_closest_labelled_source():
    with TemporaryDirectory() as directory:
        root = Path(directory)
        log = root / "calls.csv"
        labels = root / "labels.csv"
        database = root / "calls.sqlite"
        mermaid = root / "calls.mmd"
        log.write_text(
            "source,callsite,target\n"
            "000100,000310,000400\n",
            encoding="ascii",
        )
        labels.write_text(
            "00000100,cpp_owner,owner\n"
            "00000300,human_entry,human entry\n"
            "00000400,target,target\n",
            encoding="utf-8",
        )

        assert main(
            [
                str(log),
                "--database",
                str(database),
                "--mermaid",
                str(mermaid),
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
