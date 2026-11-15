"""ID allocation, item mapping, SQL generation and live DB helpers for esteria_sync.py."""
from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from dbc_io import CONTINUATION_NAME, Dbc, read_ids, schema_for

# ---------------------------------------------------------------------------------------------
# ID allocation
# ---------------------------------------------------------------------------------------------


def merge_ranges(ranges) -> list[list[int]]:
    """Sorted, merged, inclusive [lo, hi] ranges."""
    merged: list[list[int]] = []
    for lo, hi in sorted((int(a), int(b)) for a, b in ranges):
        if hi < lo:
            raise ValueError(f"empty ID range [{lo}, {hi}]")
        if merged and lo <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return merged


class IdAllocator:
    """Stable, collision-free allocation. Keys already in `mapping` keep their ID forever so reruns are
    idempotent; new keys take the next free ID at or above `floor`, skipping `used` IDs and the inclusive
    `reserved` ranges (stock 3.3.5, Conquest of Azeroth, existing custom items...).

    Allocation is O(1) amortised: a cursor only moves forward, so 50,000+ allocations stay linear."""

    def __init__(self, used: set[int], floor: int, mapping: dict[str, int] | None = None, ceiling: int = 0xFFFFFFFE,
                 reserved=()):
        self.used = set(used)
        self.floor = floor
        self.ceiling = ceiling
        self.reserved = merge_ranges(reserved)
        self.mapping = dict(mapping or {})
        self.conflicts = sorted(key for key, value in self.mapping.items() if value in self.used or self._reserved_end(value) is not None)
        self.used.update(self.mapping.values())
        self._cursor = floor

    def _reserved_end(self, value: int) -> int | None:
        import bisect
        index = bisect.bisect_right(self.reserved, [value, 0xFFFFFFFF]) - 1
        if index >= 0 and self.reserved[index][0] <= value <= self.reserved[index][1]:
            return self.reserved[index][1]
        return None

    def allocate(self, key: str) -> int:
        if key in self.mapping:
            return self.mapping[key]
        candidate = self._cursor
        while True:
            if candidate in self.used:
                candidate += 1
                continue
            end = self._reserved_end(candidate)
            if end is None:
                break
            candidate = end + 1
        if candidate > self.ceiling:
            raise OverflowError(f"ID space exhausted allocating {key}")
        self.used.add(candidate)
        self.mapping[key] = candidate
        self._cursor = candidate + 1
        return candidate


def existing_ids(table: str, base_dir: Path | None, continuation_dir: Path | None, own_project: str | None = None) -> set[int]:
    """IDs occupied by the base DBC and by every *other* project's continuation of the table."""
    used: set[int] = set()
    if base_dir and (Path(base_dir) / f"{table}.dbc").exists():
        used |= read_ids(Path(base_dir) / f"{table}.dbc")
    if continuation_dir and Path(continuation_dir).is_dir():
        for path in Path(continuation_dir).iterdir():
            match = CONTINUATION_NAME.match(path.name)
            if match and match["table"].lower() == table.lower() and match["project"] != own_project:
                used |= read_ids(path)
    return used


def load_state(path: Path) -> dict:
    if Path(path).exists():
        return json.loads(Path(path).read_text(encoding="utf-8"))
    return {"schema": 1, "ItemDisplayInfo": {}, "item_template": {}}


def save_state(path: Path, state: dict) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------------------------
# resolved_manifest.json -> DBC rows / item_template rows
# ---------------------------------------------------------------------------------------------


def client_path(file_data_id: int, extension: str) -> str:
    """Name the 3.3.5a client resolves through the Retail source (`retail/<FileDataID>.<ext>`)."""
    return f"retail/{int(file_data_id)}{extension}"


def model_texture_layers(display: dict, model_index: int) -> dict[int, int]:
    """{M2 texture type: FileDataID} for one model of a display. Retail names each replaceable layer through
    ItemDisplayInfoModelMatRes (TextureType 2 object skin, 3 second layer...); rows without a type (the display's own
    ModelMaterialResourcesID) are the object skin. A layer made of several files keeps the lowest FileDataID."""
    layers: dict[int, int] = {}
    untyped = 0
    rows = (display.get("model_textures") or [[], []])[model_index:model_index + 1]
    for row in rows[0] if rows else []:
        kind = row.get("texture_type", -1)
        fid = int(row["file_data_id"])
        if kind is None or kind < 0:
            untyped = untyped or fid
        elif kind not in layers or fid < layers[kind]:
            layers[kind] = fid
    if untyped and 2 not in layers:
        layers[2] = untyped
    return layers


def section_textures(display: dict) -> list[int]:
    """ItemDisplayInfo.Texture[8], indexed by ComponentSection (0 arm upper ... 7 foot). The manifest lists the section
    materials in no particular order, so the old positional copy put belt layers on the arms; with `section_textures` each
    file goes to its own section (the first one wins when a section has several)."""
    out = [0] * 8
    listed = display.get("section_textures")
    if listed is None:   # manifests written before the resolver reported sections
        for index, fid in enumerate(list(display.get("material_file_ids", []))[:8]):
            out[index] = int(fid)
        return out
    for entry in listed:
        section = int(entry["section"])
        if 0 <= section < 8 and not out[section]:
            out[section] = int(entry["file_data_id"])
    return out


def armor_texture_rows(display: dict, target_id: int) -> list[str]:
    """Sidecar rows `displayId,modelIndex,textureType,retail/<id>.blp` for every typed layer other than the object skin,
    which ModelTexture[model] already carries. The client binds them to the armor model's matching M2 texture slot."""
    rows = []
    for model_index in (0, 1):
        for kind, fid in sorted(model_texture_layers(display, model_index).items()):
            if kind != 2:
                rows.append(f"{target_id},{model_index},{kind},{client_path(fid, '.blp')}")
    return rows


def model_for_slot(display: dict, slot: int) -> int:
    """FileDataID of the model ItemDisplayInfo.ModelName[slot] should name. Retail lists every positional variant under both
    slots (a shoulder set: position 0 = left pauldron, 1 = right pauldron), so the resolver's single pick put the left
    pauldron on both shoulders. Where rows carry a position, slot N takes the row with position N; the client attaches
    slot 0 to the left shoulder (attachment 6) and slot 1 to the right (attachment 5)."""
    chosen = list(display.get("model_file_ids", [])) + [0, 0]
    rows = (display.get("models") or [[], []])[slot:slot + 1]
    positional = [r for r in (rows[0] if rows else []) if r.get("position", -1) in (0, 1)]
    if not positional:
        return int(chosen[slot])
    match = [r for r in positional if r["position"] == slot]
    if not match:
        return int(chosen[slot])
    for row in match:   # keep the resolver's race/gender pick when it is one of the positional rows
        if row["file_data_id"] == chosen[slot]:
            return int(row["file_data_id"])
    return int(match[0]["file_data_id"])


def display_row(display: dict, target_id: int) -> list:
    """ItemDisplayInfo.dbc row (see dbc_io.SCHEMAS) from a manifest `displays[]` entry."""
    models = [model_for_slot(display, 0), model_for_slot(display, 1)]
    textures = [model_texture_layers(display, 0).get(2, 0), model_texture_layers(display, 1).get(2, 0)]
    if not any(textures):   # manifests written before the resolver reported texture types
        textures = list(display.get("texture_file_ids", []))[:2] + [0, 0]
    geosets = list(display.get("geoset_groups", []))[:3] + [0, 0, 0]
    helmet = list(display.get("helmet_geoset_vis", []))[:2] + [0, 0]
    materials = section_textures(display)
    icon = display.get("icon_name", "")
    return ([target_id]
            + [client_path(models[0], ".m2") if models[0] else "", client_path(models[1], ".m2") if models[1] else ""]
            + [client_path(textures[0], ".blp") if textures[0] else "", client_path(textures[1], ".blp") if textures[1] else ""]
            + [icon, ""]
            + geosets[:3]
            + [int(display.get("flags", 0)), 0, 0]
            + helmet[:2]
            + [client_path(m, ".blp") if m else "" for m in materials[:8]]
            + [0, 0])


ITEM_COLUMNS = ["entry", "class", "subclass", "SoundOverrideSubclass", "name", "displayid", "Quality", "Flags",
                "BuyPrice", "SellPrice", "InventoryType", "AllowableClass", "AllowableRace", "ItemLevel",
                "RequiredLevel", "Material", "armor", "bonding", "description", "VerifiedBuild"]


def item_row(item: dict, entry: int, display_id: int, build: int | None, keep_class_restriction: bool = False) -> dict:
    return {
        "entry": entry,
        "class": int(item.get("class_id", 4)),
        "subclass": int(item.get("subclass_id", 0)),
        "SoundOverrideSubclass": -1,
        "name": str(item.get("name", f"Retail Item {item['item_id']}")),
        "displayid": display_id,
        "Quality": int(item.get("quality", 1)),
        "Flags": 0,
        "BuyPrice": int(item.get("buy_price", 0)),
        "SellPrice": int(item.get("sell_price", 0)),
        "InventoryType": int(item.get("inventory_type", 0)),
        # Retail class restrictions name the class the set was made for; Esteria classes differ, so by default any class may wear it.
        "AllowableClass": int(item.get("allowable_class", -1)) if keep_class_restriction else -1,
        "AllowableRace": int(item.get("allowable_race", -1)),
        "ItemLevel": int(item.get("item_level", 1)),
        "RequiredLevel": int(item.get("required_level", 0)),
        "Material": int(item.get("material", 0)),
        "armor": int(item.get("armor", 0)),
        "bonding": int(item.get("bonding", 0)),
        "description": f"Retail item {item['item_id']}",
        "VerifiedBuild": build,
    }


def sql_value(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, (int, float)):
        return repr(value)
    escaped = str(value).replace("\\", "\\\\").replace("'", "''").replace("\n", "\\n").replace("\r", "")
    return f"'{escaped}'"


def _insert(table: str, columns: list[str], rows: list[dict]) -> str:
    cols = ", ".join(f"`{c}`" for c in columns)
    values = ",\n".join("(" + ", ".join(sql_value(r[c]) for c in columns) + ")" for r in rows)
    return f"INSERT INTO `{table}` ({cols}) VALUES\n{values};\n"


def migration_sql(rows: list[dict], vendor: int | None = None, comment: str = "") -> str:
    """Idempotent pending_db_world migration: DELETE then INSERT for every table touched."""
    if not rows:
        return "--\n"
    rows = sorted(rows, key=lambda r: r["entry"])
    entries = ", ".join(str(r["entry"]) for r in rows)
    out = ["-- " + comment if comment else "--",
           f"DELETE FROM `item_template` WHERE `entry` IN ({entries});",
           _insert("item_template", ITEM_COLUMNS, rows).rstrip("\n")]
    if vendor:
        vendor_rows = [{"entry": vendor, "slot": 0, "item": r["entry"], "maxcount": 0, "incrtime": 0,
                        "ExtendedCost": 0, "VerifiedBuild": None} for r in rows]
        out.append(f"DELETE FROM `npc_vendor` WHERE `entry` = {vendor} AND `item` IN ({entries});")
        out.append(_insert("npc_vendor", ["entry", "slot", "item", "maxcount", "incrtime", "ExtendedCost",
                                          "VerifiedBuild"], vendor_rows).rstrip("\n"))
    return "\n".join(out) + "\n"


def retire_sql(entries: list[int]) -> str:
    """Migration that removes item_template entries this tool allocated earlier and has since moved to new IDs."""
    if not entries:
        return ""
    lines = ["-- Retail item import: entries moved because the client's supplemental Item table owns those IDs"]
    for start in range(0, len(entries), 500):
        part = ",".join(str(e) for e in entries[start:start + 500])
        lines.append(f"DELETE FROM `item_template` WHERE `entry` IN ({part});")
    return "\n".join(lines) + "\n"


def migration_filename(slug: str, when: datetime | None = None) -> str:
    when = when or datetime.now(timezone.utc)
    stamp = when.strftime("%Y%m%d%H%M%S") + f"{when.microsecond // 1000:03d}"
    slug = re.sub(r"[^a-z0-9]+", "_", slug.lower()).strip("_") or "retail_items"
    return f"rev_{stamp}_{slug}.sql"


CHUNK_SIZE = 2000


def chunked(rows: list, size: int = CHUNK_SIZE) -> list[list]:
    size = max(1, int(size))
    return [rows[i:i + size] for i in range(0, len(rows), size)]


def migration_chunks(rows: list[dict], vendor: int | None = None, comment: str = "", size: int = CHUNK_SIZE) -> list[str]:
    """One idempotent migration per `size` items, each in its own transaction, so a 50,000-item import is
    ~25 small files (never one multi-hundred-MB statement) and a failed file rolls back whole."""
    rows = sorted(rows, key=lambda r: r["entry"])
    parts = chunked(rows, size)
    out = []
    for index, part in enumerate(parts):
        label = f"{comment} [{index + 1}/{len(parts)}]" if comment else f"[{index + 1}/{len(parts)}]"
        body = migration_sql(part, vendor=vendor, comment=label)
        out.append("START TRANSACTION;\n" + body + "COMMIT;\n")
    return out


def migration_filenames(slug: str, count: int, when: datetime | None = None) -> list[str]:
    """`count` ascending, unique names: the updater applies pending files in name order."""
    from datetime import timedelta
    when = when or datetime.now(timezone.utc)
    return [migration_filename(slug, when + timedelta(milliseconds=i)) for i in range(count)]


# Directories that the migration writer must never target (AzerothCore rules, AGENTS.md).
_FORBIDDEN = [("data", "sql", "base"), ("data", "sql", "archive")]


def check_sql_target(directory: Path) -> Path:
    parts = [p.lower() for p in Path(directory).resolve().parts]
    for index in range(len(parts) - 2):
        if tuple(parts[index:index + 3]) in _FORBIDDEN:
            raise PermissionError(f"Refusing to write into immutable SQL tree: {directory}")
        if parts[index:index + 3] == ["data", "sql", "updates"] and index + 3 < len(parts) and \
                re.fullmatch(r"db_(world|auth|characters)", parts[index + 3]):
            raise PermissionError(f"Refusing to write into merged updates tree: {directory}")
    if not (parts[-1].startswith("pending_db_") or parts[-1] in ("db_world", "db-world")):
        raise PermissionError(f"SQL target must be a pending_db_* (or module db_world) folder: {directory}")
    return Path(directory)


# ---------------------------------------------------------------------------------------------
# Live database access
# ---------------------------------------------------------------------------------------------


class MysqlCli:
    """Minimal adapter over the `mysql` client so no Python driver is required."""

    def __init__(self, host="127.0.0.1", port=3306, user="root", password="", database="acore_world", binary="mysql"):
        self.args = [binary, f"--host={host}", f"--port={port}", f"--user={user}", "--batch", "--raw",
                     "--default-character-set=utf8mb4", database]
        self.env = {"MYSQL_PWD": password} if password else {}

    def _run(self, sql: str) -> str:
        import os
        # SQL goes over stdin: a Windows command line tops out at ~32k characters and one 2,000-row batch is far bigger.
        proc = subprocess.run(self.args, input=sql, capture_output=True, text=True, encoding="utf-8",
                              env={**os.environ, **self.env})
        if proc.returncode:
            raise RuntimeError(proc.stderr.strip() or "mysql failed")
        return proc.stdout

    def query(self, sql: str) -> list[dict]:
        lines = self._run(sql).splitlines()
        if not lines:
            return []
        header = lines[0].split("\t")
        return [dict(zip(header, [None if v == "NULL" else v for v in line.split("\t")])) for line in lines[1:]]

    def execute(self, sql: str) -> None:
        self._run(sql)


def backup_rows(db, entries: list[int]) -> str:
    """REPLACE statements that restore the current state of the affected rows."""
    if not entries:
        return ""
    out = []
    for part in chunked(list(entries), 1000):
        rows = db.query("SELECT * FROM `item_template` WHERE `entry` IN (%s)" % ", ".join(map(str, part)))
        for row in rows:
            cols = ", ".join(f"`{c}`" for c in row)
            vals = ", ".join(sql_value(v) for v in row.values())
            out.append(f"REPLACE INTO `item_template` ({cols}) VALUES ({vals});")
    return "\n".join(out) + "\n" if out else "-- no pre-existing rows\n"


def upsert_sql(rows: list[dict]) -> str:
    columns = ITEM_COLUMNS
    cols = ", ".join(f"`{c}`" for c in columns)
    values = ",\n".join("(" + ", ".join(sql_value(r[c]) for c in columns) + ")" for r in rows)
    updates = ", ".join(f"`{c}` = new.`{c}`" for c in columns if c != "entry")
    return f"INSERT INTO `item_template` ({cols}) VALUES\n{values} AS new\nON DUPLICATE KEY UPDATE {updates};"


def apply_upsert(db, rows: list[dict], size: int = CHUNK_SIZE, progress=None) -> int:
    """Upsert in chunked transactions. Each chunk commits on its own; a failing chunk rolls back (the client
    disconnects mid-transaction) and raises, leaving earlier chunks applied. A rerun is idempotent."""
    done = 0
    for part in chunked(sorted(rows, key=lambda r: r["entry"]), size):
        db.execute("START TRANSACTION;\n" + upsert_sql(part) + "\nCOMMIT;")
        done += len(part)
        if progress:
            progress(done, len(rows))
    return done


# ---------------------------------------------------------------------------------------------
# Item entry collision sources (live DB and the SQL trees)
# ---------------------------------------------------------------------------------------------

_ENTRY_NUMBER = re.compile(r"(?:^|[\s(,])(\d{6,10})(?=\s*[,)])", re.M)


def live_used_entries(db, floor: int) -> set[int]:
    """Every item_template entry at or above `floor` in the live world DB."""
    rows = db.query("SELECT `entry` FROM `item_template` WHERE `entry` >= %d" % int(floor))
    return {int(r["entry"]) for r in rows}


def scan_sql_entries(directories, floor: int, skip_slug: str = "retail_items", max_bytes: int = 256 * 1024 * 1024) -> set[int]:
    """Conservative scan of `.sql` files that mention item_template: every number >= `floor` that looks like an
    entry is treated as taken. False positives only reserve a few extra IDs; they can never cause a collision.
    Files this tool wrote itself (`*_<skip_slug>*.sql`) are skipped, since the state file already maps them."""
    found: set[int] = set()
    for directory in directories:
        if not directory or not Path(directory).is_dir():
            continue
        for path in Path(directory).rglob("*.sql"):
            if skip_slug and skip_slug in path.name:
                continue
            try:
                if path.stat().st_size > max_bytes:
                    continue
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "item_template" not in text.lower():
                continue
            for match in _ENTRY_NUMBER.finditer(text):
                value = int(match.group(1))
                if value >= floor:
                    found.add(value)
    return found


@dataclass
class PlanRow:
    entry: int
    name: str
    displayid: int
    action: str


def plan_live(db, rows: list[dict], owned: set[int]) -> list[PlanRow]:
    """Compare against the live table. Rows we do not own and that already exist are conflicts."""
    existing = {}
    for part in chunked(rows, 1000):
        found = db.query("SELECT `entry`, `name`, `displayid` FROM `item_template` WHERE `entry` IN (%s)"
                         % ", ".join(str(r["entry"]) for r in part))
        existing.update({int(r["entry"]): r for r in found})
    plan = []
    for row in rows:
        current = existing.get(row["entry"])
        if current is None:
            action = "INSERT"
        elif row["entry"] not in owned:
            action = "CONFLICT"
        elif current["name"] == row["name"] and int(current["displayid"]) == row["displayid"]:
            action = "UNCHANGED"
        else:
            action = "UPDATE"
        plan.append(PlanRow(row["entry"], row["name"], row["displayid"], action))
    return plan


def format_plan(plan: list[PlanRow], limit: int = 200) -> str:
    lines = [f"{'entry':>9}  {'displayid':>9}  {'action':<9}  name"]
    shown = plan if len(plan) <= limit else [p for p in plan if p.action == "CONFLICT"][:limit] + plan[:limit]
    lines += [f"{p.entry:>9}  {p.displayid:>9}  {p.action:<9}  {p.name}" for p in shown]
    if len(plan) > limit:
        lines.append(f"... {max(0, len(plan) - len(shown))} more row(s) not shown (conflicts are listed first)")
    counts: dict[str, int] = {}
    for p in plan:
        counts[p.action] = counts.get(p.action, 0) + 1
    lines.append("total: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())) if plan else "total: 0")
    return "\n".join(lines)
