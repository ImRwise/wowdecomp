"""Find and merge the client's effective DBFilesClient tables.

The 3.3.5a client reads one `DBFilesClient\\<Table>.dbc` per table: the copy in the highest-priority archive. It knows
nothing of mod-wxl-dbc `.dbc1-<project>` continuations (those are server-side only), so rows the client must see have
to live in a complete table shipped in an archive that sorts after every other copy. This module reads the current
winning copy out of the client's Data folder with StormLib, merges the new rows into it and reports collisions.

Archive priority mirrors Engine/Source/Retail/PackedArchives.cpp: `patch-*.mpq` descending by lower-cased name wins,
then the base archives.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

BASE_ARCHIVES = ["patch.mpq", "lichking.mpq", "expansion.mpq", "common-3.mpq", "common-2.mpq", "common.mpq"]


def archive_order(data_dir: Path) -> list[Path]:
    """Archives from highest to lowest priority."""
    files = {p.name.lower(): p for p in Path(data_dir).iterdir() if p.is_file() and p.suffix.lower() == ".mpq"}
    patches = sorted((n for n in files if n.startswith("patch-")), reverse=True)
    ordered = [files[n] for n in patches]
    ordered += [files[n] for n in BASE_ARCHIVES if n in files]
    return ordered


def _storm(stormlib: Path):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "MountStudio"))
    from worker import Storm  # noqa: E402
    return Storm(stormlib)


def find_winning(stormlib: Path, data_dir: Path, internal_name: str, skip: set[str] | None = None):
    """(bytes, archive path) of the highest-priority copy of `internal_name`, or (None, None).
    `skip` lists lower-cased archive file names to ignore (for example the archive this tool is about to replace)."""
    storm = _storm(Path(stormlib))
    for archive in archive_order(Path(data_dir)):
        if skip and archive.name.lower() in skip:
            continue
        with storm.archive(archive) as handle:
            data = storm.read(handle, internal_name, missing=True)
        if data is not None:
            return data, archive
    return None, None


class MergeReport:
    def __init__(self, table: str, base_rows: int, added: int, replaced_own: int, collisions: list[int], source: str):
        self.table, self.base_rows, self.added, self.replaced_own = table, base_rows, added, replaced_own
        self.collisions, self.source = collisions, source

    def line(self) -> str:
        bad = f", {len(self.collisions)} COLLISION(S) e.g. {self.collisions[:3]}" if self.collisions else ", 0 collisions"
        if getattr(self, "out_of_pool_rows", None):
            bad += f"; {len(self.out_of_pool_rows)} existing row(s) have string offsets outside the pool (kept as is: {self.out_of_pool_rows[:4]})"
        return (f"{self.table}.dbc: base {self.base_rows} rows from {self.source} + {self.added} new "
                f"({self.replaced_own} of ours refreshed){bad}")


class RawTable:
    """A WDBC table merged at byte level. Existing records are kept bit-for-bit and the string pool is only appended to,
    so every old string offset stays valid -- including the out-of-pool offsets that other packs' rows sometimes carry
    (the client's winning ItemDisplayInfo.dbc has two). Parsing and re-serialising through Dbc would refuse such a file
    or silently change it."""

    def __init__(self, data: bytes, kinds: str):
        if len(data) < 20 or data[:4] != b"WDBC":
            raise ValueError("Not a WDBC file")
        _, count, fields, size, pool_size = struct.unpack_from("<4s4I", data)
        if fields != len(kinds) or size != fields * 4 or len(data) != 20 + count * size + pool_size:
            raise ValueError(f"Unexpected table layout ({fields} fields, {size} bytes/record) for schema of {len(kinds)}")
        self.kinds, self.size = kinds, size
        self.records = [data[20 + i * size:20 + (i + 1) * size] for i in range(count)]
        self.pool = bytearray(data[20 + count * size:])
        self.original_pool = len(self.pool)
        self._strings: dict[str, int] = {}
        self.out_of_pool_rows = [struct.unpack_from("<I", r)[0] for r in self.records if self._bad_string(r)]

    def _bad_string(self, record: bytes) -> bool:
        return any(kind == "s" and struct.unpack_from("<I", record, k * 4)[0] >= self.original_pool
                   for k, kind in enumerate(self.kinds))

    def ids(self) -> set[int]:
        return {struct.unpack_from("<I", r)[0] for r in self.records}

    def _intern(self, value: str) -> int:
        if not value:
            return 0
        if value not in self._strings:
            self._strings[value] = len(self.pool)
            self.pool += value.encode("utf-8") + b"\0"
        return self._strings[value]

    def encode(self, row: list) -> bytes:
        if len(row) != len(self.kinds):
            raise ValueError(f"Row has {len(row)} columns, schema has {len(self.kinds)}")
        out = bytearray()
        for kind, value in zip(self.kinds, row):
            if kind == "s":
                out += struct.pack("<I", self._intern(value))
            elif kind == "f":
                out += struct.pack("<f", value)
            elif kind == "i":
                out += struct.pack("<i", value)
            else:
                out += struct.pack("<I", value)
        return bytes(out)

    def upsert(self, row: list) -> None:
        record = self.encode(row)
        for index, existing in enumerate(self.records):
            if existing[:4] == record[:4]:
                self.records[index] = record
                return
        self.records.append(record)

    def by_id(self) -> dict:
        """id -> decoded row (strings read from the pool; an out-of-pool offset reads as '')."""
        rows = {}
        for record in self.records:
            row = []
            for k, kind in enumerate(self.kinds):
                (value,) = struct.unpack_from("<I", record, k * 4)
                if kind == "s":
                    if value >= len(self.pool):
                        row.append("")
                    else:
                        row.append(self.pool[value:self.pool.index(b"\0", value)].decode("utf-8", "replace"))
                elif kind == "f":
                    row.append(struct.unpack("<f", struct.pack("<I", value))[0])
                elif kind == "i":
                    row.append(struct.unpack("<i", struct.pack("<I", value))[0])
                else:
                    row.append(value)
            rows[row[0]] = row
        return rows

    def to_bytes(self) -> bytes:
        records = sorted(self.records, key=lambda r: struct.unpack_from("<I", r)[0])
        header = struct.pack("<4s4I", b"WDBC", len(records), len(self.kinds), self.size, len(self.pool))
        return header + b"".join(records) + bytes(self.pool)


def merge_table(base_bytes: bytes, kinds: str, new_rows: list, owned_ids: set[int], table: str, source: str):
    """Returns (merged RawTable, MergeReport). A new row whose ID already exists in the base and is not one of ours
    (`owned_ids`: IDs this tool allocated on earlier runs) is a collision and is left out of the merge."""
    merged = RawTable(base_bytes, kinds)
    existing = merged.ids()
    collisions, added, refreshed = [], 0, 0
    for row in new_rows:
        if row[0] in existing and row[0] not in owned_ids:
            collisions.append(row[0])
            continue
        if row[0] in existing:
            refreshed += 1
        else:
            added += 1
        merged.upsert(row)
    report = MergeReport(table, len(existing), added, refreshed, collisions, source)
    report.out_of_pool_rows = merged.out_of_pool_rows
    return merged, report


# Item.dbc: ID, ClassID, SubclassID, SoundOverrideSubclassID, MaterialID, DisplayInfoID, InventoryType, SheatheType
def sheath_for(item_class: int, subclass: int, inventory_type: int) -> int:
    """Best-effort SheatheType (stock values: 1 two-hand, 2 staff, 3 one-hand, 4 shield). 0 for everything else."""
    if item_class == 4:
        return 4 if inventory_type == 14 else 0
    if item_class != 2:
        return 0
    if subclass == 10:
        return 2
    if inventory_type == 17:
        return 1
    if inventory_type in (13, 21, 22):
        return 3
    return 0


def item_dbc_row(row: dict) -> list:
    return [int(row["entry"]), int(row["class"]), int(row["subclass"]), int(row["SoundOverrideSubclass"]),
            int(row["Material"]), int(row["displayid"]), int(row["InventoryType"]),
            sheath_for(int(row["class"]), int(row["subclass"]), int(row["InventoryType"]))]
