"""Binary-accurate 3.3.5a WDBC reader/writer plus the schemas Esteria needs.

A continuation file for mod-wxl-dbc is an ordinary WDBC file with the same record layout as the
base table that holds only added or replaced rows (rows are keyed by column 0). Strings live in
the trailing pool; offset 0 is the empty string.
"""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"WDBC"

# Field kinds: u = uint32, i = int32, f = float32, s = string (pool offset).
SCHEMAS = {
    # ID, ModelName[2], ModelTexture[2], InventoryIcon[2], GeosetGroup[3], Flags, SpellVisualID,
    # GroupSoundIndex, HelmetGeosetVis[2], Texture[8], ItemVisual, ParticleColorID
    "ItemDisplayInfo": "u" + "s" * 6 + "u" * 8 + "s" * 8 + "uu",
    # Item.dbc: ID, ClassID, SubclassID, SoundOverrideSubclassID, MaterialID, DisplayInfoID, InventoryType, SheatheType
    "Item": "uuiiuuuu",
    "ItemVisuals": "u" * 6,
    "ItemVisualEffects": "us",
    # 14 numeric columns, 16 locale names + flags mask, 7 trailing numeric columns
    "SpellItemEnchantment": "u" * 14 + "s" * 16 + "u" + "u" * 7,
    # ID, 16 locale names + flags mask, ItemID[17], SetSpellID[8], SetThreshold[8], RequiredSkill, RequiredSkillRank
    "ItemSet": "u" + "s" * 16 + "u" + "u" * 17 + "u" * 8 + "u" * 8 + "uu",
}
FILE_STEMS = {name.lower(): name for name in SCHEMAS}


class DbcError(ValueError):
    pass


def schema_for(table: str, override: str | None = None) -> str:
    kinds = override or SCHEMAS.get(FILE_STEMS.get(table.lower(), table))
    if not kinds or not re.fullmatch(r"[uifs]+", kinds):
        raise DbcError(f"No schema for {table}; pass --schema with one of u/i/f/s per column")
    return kinds


@dataclass
class Dbc:
    kinds: str
    rows: list = field(default_factory=list)  # each row: list of python values (str for 's')
    _index: dict = field(default_factory=dict, repr=False, compare=False)

    @property
    def field_count(self) -> int:
        return len(self.kinds)

    def ids(self) -> set[int]:
        return {row[0] for row in self.rows}

    def by_id(self) -> dict:
        return {row[0]: row for row in self.rows}

    def upsert(self, row: list) -> None:
        if len(row) != self.field_count:
            raise DbcError(f"Row has {len(row)} columns, schema has {self.field_count}")
        # An ID index keeps bulk upserts linear (a 50,000-row import was O(n^2) with a list scan).
        if len(self._index) != len(self.rows):
            self._index = {existing[0]: index for index, existing in enumerate(self.rows)}
        position = self._index.get(row[0])
        if position is not None:
            self.rows[position] = list(row)
            return
        self._index[row[0]] = len(self.rows)
        self.rows.append(list(row))

    # -- serialization -------------------------------------------------------------------------
    def to_bytes(self) -> bytes:
        """Deterministic: rows sorted by ID, strings pooled in first-use order."""
        rows = sorted(self.rows, key=lambda row: row[0])
        pool = bytearray(b"\0")
        offsets = {"": 0}
        body = bytearray()
        for row in rows:
            if len(row) != self.field_count:
                raise DbcError("Row/schema column mismatch")
            for kind, value in zip(self.kinds, row):
                if kind == "s":
                    value = value or ""
                    if value not in offsets:
                        offsets[value] = len(pool)
                        pool += value.encode("utf-8") + b"\0"
                    body += struct.pack("<I", offsets[value])
                elif kind == "f":
                    body += struct.pack("<f", value)
                elif kind == "i":
                    body += struct.pack("<i", value)
                else:
                    body += struct.pack("<I", value)
        header = struct.pack("<4s4I", MAGIC, len(rows), self.field_count, self.field_count * 4, len(pool))
        return header + bytes(body) + bytes(pool)

    @classmethod
    def from_bytes(cls, data: bytes, kinds: str) -> "Dbc":
        if len(data) < 20 or data[:4] != MAGIC:
            raise DbcError("Not a WDBC file")
        _, count, fields, record_size, string_size = struct.unpack_from("<4s4I", data)
        if fields != len(kinds) or record_size != fields * 4:
            raise DbcError(f"Layout mismatch: file has {fields} fields/{record_size} bytes, schema {len(kinds)}")
        if len(data) != 20 + count * record_size + string_size:
            raise DbcError("Truncated or oversized WDBC file")
        pool = data[20 + count * record_size:]
        result = cls(kinds)
        for index in range(count):
            raw = struct.unpack_from("<%dI" % fields, data, 20 + index * record_size)
            row = []
            for kind, value in zip(kinds, raw):
                if kind == "s":
                    if value >= len(pool):
                        raise DbcError("String offset outside pool")
                    end = pool.index(b"\0", value)
                    row.append(pool[value:end].decode("utf-8"))
                elif kind == "f":
                    row.append(struct.unpack("<f", struct.pack("<I", value))[0])
                elif kind == "i":
                    row.append(struct.unpack("<i", struct.pack("<I", value))[0])
                else:
                    row.append(value)
            result.rows.append(row)
        return result

    @classmethod
    def read(cls, path: Path, kinds: str) -> "Dbc":
        return cls.from_bytes(Path(path).read_bytes(), kinds)

    def write(self, path: Path) -> bytes:
        data = self.to_bytes()
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return data


def read_ids(path: Path) -> set[int]:
    """Column-0 IDs of any WDBC file without needing its schema."""
    with open(path, "rb") as handle:
        header = handle.read(20)
        if len(header) < 20 or header[:4] != MAGIC:
            raise DbcError(f"Not a WDBC file: {path}")
        _, count, _fields, record_size, _ = struct.unpack("<4s4I", header)
        data = handle.read(count * record_size)
    return {struct.unpack_from("<I", data, index * record_size)[0] for index in range(count)}


CONTINUATION_NAME = re.compile(r"^(?P<table>[A-Za-z0-9_]+)\.dbc(?P<tier>[1-9])-(?P<project>[A-Za-z0-9_-]+)$")


def continuation_name(table: str, project: str, tier: int = 1) -> str:
    """mod-wxl-dbc discovers `<Table>.dbc<tier>-<project>`; tier 1-9, slug of [A-Za-z0-9_-]."""
    if not 1 <= tier <= 9:
        raise DbcError("Continuation tier must be 1-9")
    name = f"{table}.dbc{tier}-{project}"
    if not CONTINUATION_NAME.match(name) or name.lower().endswith((".bak", ".backup", ".old", ".orig", ".tmp")):
        raise DbcError(f"Invalid continuation project slug: {project!r}")
    return name
