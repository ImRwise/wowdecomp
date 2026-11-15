#!/usr/bin/env python3
"""EsteriaWoW database + DBC sync for Retail items.

  esteria_sync.py resolve --items 12345,67890 --out resolved_manifest.json     (headless RetailAssetProbe)
  esteria_sync.py sync --manifest resolved_manifest.json [--dry-run] [--apply-live]

`sync` reads resolved_manifest.json (written by RetailAssetProbe --resolve-items) and
  1. allocates collision-free ItemDisplayInfo IDs and item_template entries (stable across reruns),
  2. writes an ItemDisplayInfo.dbc<tier>-<project> mod-wxl-dbc continuation to the staging folder
     (DBFilesClient\\, for PatchBuilder) and to the server continuation directory,
  3. writes an idempotent migration to data/sql/updates/pending_db_world/,
  4. optionally upserts item_template into the live acore_world after backing up affected rows.
Base DBCs, data/sql/base, data/sql/archive and data/sql/updates/db_* are never touched.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dbc_io import Dbc, continuation_name, schema_for  # noqa: E402
import client_dbc  # noqa: E402
import sync_core as core  # noqa: E402

DEFAULT_CONFIG = {
    "server_root": os.environ.get("ESTERIA_SERVER_ROOT", "../EsteriaWoW"),
    "server_data_dir": None,           # worldserver DataDir; default <server_root>/assets
    "base_dbc_dir": None,              # default <server_data_dir>/dbc, then <server_root>/assets/dbc
    "continuation_subdir": "dbc-continuations",
    "sql_dir": None,                   # default <server_root>/data/sql/updates/pending_db_world
    "project": "retail-armor",
    "tier": 1,
    "id_floor": {"ItemDisplayInfo": 700000, "item_template": 1000000},
    # Inclusive [lo, hi] ranges the allocator must never hand out (stock 3.3.5, Conquest of Azeroth, existing
    # EsteriaWoW custom items...). On top of these it also avoids every live item_template entry and every
    # entry mentioned in the pending/updates SQL, so a bulk import cannot collide with them.
    "reserved_ranges": {"ItemDisplayInfo": [], "item_template": []},
    "sql_scan_dirs": None,             # default: pending_db_world and updates/db_world next to it
    "chunk_size": 2000,                # items per migration file / live transaction
    "keep_class_restriction": False,   # True copies Retail's AllowableClass mask; False lets every class use synced items
    "stage_dir": "build/retail-sync/stage",
    "state_file": "build/retail-sync/sync_state.json",
    "probe_exe": "build/retail/Release/RetailAssetProbe.exe",
    "retail_ini": None,
    # The client reads complete DBFilesClient tables only (no .dbcN- continuations). With client_data_dir set, sync merges the
    # new rows into the client's winning ItemDisplayInfo.dbc / Item.dbc (read from its Data archives with StormLib) and stages
    # the full tables for PatchBuilder. stormlib defaults to <client_data_dir>/../StormLib.dll.
    "client_data_dir": None,
    "stormlib": None,
    # The client also merges supplemental tables (Data/Retail/RetailCasc.ini `tables=`) onto Item.dbc / ItemDisplayInfo.dbc at load
    # and aborts on any ID that already exists. Those IDs are therefore off limits. None = read the folder from RetailCasc.ini.
    "client_supplement_dir": None,
    # Our own full-table archive is never used as the merge base (the base is whatever the client has without it).
    "client_priority_archive": "patch-" + "Z" * 30 + "RetailItems.mpq",
    "db": {"host": "127.0.0.1", "port": 3306, "user": "root", "password": "", "database": "acore_world"},
}


def load_config(path: str | None) -> dict:
    config = json.loads(json.dumps(DEFAULT_CONFIG))
    if path:
        user = json.loads(Path(path).read_text(encoding="utf-8"))
        for key, value in user.items():
            if isinstance(value, dict) and isinstance(config.get(key), dict):
                config[key].update(value)
            else:
                config[key] = value
    root = Path(config["server_root"])
    data = Path(config["server_data_dir"]) if config["server_data_dir"] else root / "assets"
    config["_data"] = data
    config["_continuations"] = data / config["continuation_subdir"]
    base = Path(config["base_dbc_dir"]) if config["base_dbc_dir"] else data / "dbc"
    config["_base_dbc"] = base if base.exists() else root / "assets" / "dbc"
    config["_sql"] = Path(config["sql_dir"]) if config["sql_dir"] else root / "data/sql/updates/pending_db_world"
    return config


def resolve(args, config) -> int:
    probe = Path(args.probe or config["probe_exe"])
    ini = args.retail_ini or config["retail_ini"]
    if not ini:
        print("error: --retail-ini (RetailCasc.ini or a cdn_config.json) is required", file=sys.stderr)
        return 2
    bulk = [f for f in ("all_sets", "all_armor", "all_weapons", "all_catalog") if getattr(args, f)]
    if not args.items and not bulk:
        print("error: give --items or one of --all-sets/--all-armor/--all-weapons/--all-catalog", file=sys.stderr)
        return 2
    command = [str(probe), str(ini), "--resolve-items"]
    if args.items:
        command.append(args.items)
    command += ["--out", str(args.out)]
    for flag in ("product", "region", "cache", "build", "icons_dir", "min_expansion", "max_expansion", "workers", "journal"):
        if getattr(args, flag) is not None:
            command += [f"--{flag.replace('_', '-')}", str(getattr(args, flag))]
    for flag in bulk + [f for f in ("skip_uncollectible", "every_item", "graph_only") if getattr(args, f)]:
        command.append(f"--{flag.replace('_', '-')}")
    return subprocess.run(command).returncode


def supplement_ids(config, table: str) -> set[int]:
    """IDs the client's supplemental registry adds to `table` (item / itemdisplayinfo) when it loads Item.dbc etc."""
    folder = config.get("client_supplement_dir")
    if not folder and config.get("client_data_dir"):
        ini = Path(config["client_data_dir"]) / "Retail" / "RetailCasc.ini"
        if ini.is_file():
            for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
                key, _, value = line.partition("=")
                if key.strip().lower() == "tables" and value.strip():
                    folder = Path(config["client_data_dir"]) / "Retail" / value.strip()
    if not folder:
        return set()
    path = Path(folder) / f"{table.lower()}.dbc"
    return core.read_ids(path) if path.is_file() else set()


def client_tables(config, dbc, rows, display_owned_before, items_owned_before):
    """Full client-side ItemDisplayInfo.dbc and Item.dbc with the new rows merged into the client's current winning copies.
    Returns (files {archive name: bytes}, report lines, collisions)."""
    data_dir = Path(config["client_data_dir"])
    stormlib = Path(config["stormlib"]) if config.get("stormlib") else data_dir.parent / "StormLib.dll"
    files, lines, collisions = {}, [], 0
    jobs = [("ItemDisplayInfo", schema_for("ItemDisplayInfo"), dbc.rows, display_owned_before),
            ("Item", schema_for("Item"), [client_dbc.item_dbc_row(r) for r in rows], items_owned_before)]
    for table, kinds, new_rows, owned in jobs:
        own = {config["client_priority_archive"].lower()} if config.get("client_priority_archive") else None
        base, archive = client_dbc.find_winning(stormlib, data_dir, f"DBFilesClient\\{table}.dbc", skip=own)
        if base is None:
            raise RuntimeError(f"no DBFilesClient\\{table}.dbc found in any archive under {data_dir}")
        merged, report = client_dbc.merge_table(base, kinds, new_rows, owned, table, archive.name)
        lines.append(report.line())
        collisions += len(report.collisions)
        files[f"{table}.dbc"] = merged.to_bytes()
    return files, lines, collisions


def sync(args, config) -> int:
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    if manifest.get("schema") != 1:
        print("error: unsupported resolved_manifest.json schema", file=sys.stderr)
        return 2
    project, tier = config["project"], int(config["tier"])
    state_path = Path(config["state_file"])
    state = core.load_state(state_path)
    owned_before = {int(v) for v in state.get("owned_entries", [])}
    displays = {d["id"]: d for d in manifest.get("displays", [])}
    items = manifest.get("items", [])
    for item in items:  # InventoryIcon lives on the display row in 3.3.5a; Retail keeps the icon on the item
        for display_id in [item.get("display_info_id")] + list(item.get("variant_display_info_ids", [])):
            if display_id in displays and not displays[display_id].get("icon_name"):
                displays[display_id]["icon_name"] = item.get("icon_name", "")

    # --- DBC ------------------------------------------------------------------------------
    table = "ItemDisplayInfo"
    kinds = schema_for(table)
    display_owned_before = {int(v) for v in state.get(table, {}).values()}
    used = core.existing_ids(table, config["_base_dbc"], config["_continuations"], own_project=project)
    used |= supplement_ids(config, table)
    display_alloc = core.IdAllocator(used, config["id_floor"][table], state[table],
                                     reserved=config["reserved_ranges"].get(table, []))
    if display_alloc.conflicts:
        print(f"error: {len(display_alloc.conflicts)} previously allocated ItemDisplayInfo ID(s) are now taken by "
              f"stock/other-project rows or reserved ranges (e.g. {display_alloc.conflicts[0]}); fix the state file or ranges",
              file=sys.stderr)
        return 1
    cont_file = continuation_name(table, project, tier)
    own_path = Path(config["_continuations"]) / cont_file
    dbc = Dbc.read(own_path, kinds) if own_path.exists() else Dbc(kinds)
    armor_rows: list[str] = []   # typed texture layers the ItemDisplayInfo columns cannot carry (see sync_core)
    for retail_display in sorted({i["display_info_id"] for i in items if i.get("display_info_id")}):
        if retail_display not in displays:
            print(f"warning: display {retail_display} missing from manifest; skipped", file=sys.stderr)
            continue
        target = display_alloc.allocate(f"retail:{retail_display}")
        dbc.upsert(core.display_row(displays[retail_display], target))
        armor_rows += core.armor_texture_rows(displays[retail_display], target)
    state[table] = display_alloc.mapping

    # --- item_template ---------------------------------------------------------------------
    floor = int(config["id_floor"]["item_template"])
    db = None
    if args.apply_live or args.dry_run_live:
        d = config["db"]
        db = args.db_client or core.MysqlCli(d["host"], d["port"], d["user"], d["password"], d["database"],
                                             binary=args.mysql or "mysql")
    # Entries that must not be reused: the live table, every entry in the pending/updates SQL, and the
    # configured reserved ranges. Rows this tool already owns are excluded (their IDs are stable by mapping).
    taken: set[int] = set()
    live_ok = False
    if db is not None:
        try:
            taken |= core.live_used_entries(db, floor)
            live_ok = True
        except Exception as error:
            if args.apply_live:
                print(f"error: live database unavailable ({error}); refusing to allocate item entries blind", file=sys.stderr)
                return 1
            print(f"note: live database unavailable ({error}); checking SQL trees only", file=sys.stderr)
    sql_root = Path(config["_sql"])
    scan_dirs = config["sql_scan_dirs"] or [sql_root, sql_root.parent / "db_world"]
    taken |= core.scan_sql_entries(scan_dirs, floor)
    taken -= owned_before
    blocked = supplement_ids(config, "Item")
    taken |= blocked
    retired = {int(v) for v in state.get("retired_entries", [])}
    clash = sorted(key for key, value in state["item_template"].items() if value in blocked)
    for key in clash:   # an entry the client's supplement also defines cannot be ours: retire it and allocate a fresh one
        retired.add(state["item_template"].pop(key))
    if clash:
        print(f"note: {len(clash)} previously allocated item entr(ies) collide with the client's supplemental Item table; "
              f"retiring them and allocating new entries (e.g. {sorted(retired)[0]})", file=sys.stderr)
    state["retired_entries"] = sorted(retired)
    taken |= retired
    item_alloc = core.IdAllocator(taken, floor, state["item_template"], reserved=config["reserved_ranges"].get("item_template", []))
    if item_alloc.conflicts:
        print(f"error: {len(item_alloc.conflicts)} previously allocated item entr(ies) now collide with live/SQL/reserved "
              f"entries (e.g. {item_alloc.conflicts[0]}: {state['item_template'][item_alloc.conflicts[0]]})", file=sys.stderr)
        return 1
    build = int(manifest.get("build_number", 0)) or None
    rows, sync_map = [], []
    for item in sorted(items, key=lambda i: i["item_id"]):
        display_target = display_alloc.mapping.get(f"retail:{item.get('display_info_id')}", 0)
        entry = item_alloc.allocate(f"retail:{item['item_id']}")
        rows.append(core.item_row(item, entry, display_target, build, bool(config.get("keep_class_restriction", False))))
        sync_map.append({"item_id": item["item_id"], "item_template_entry": entry,
                         "retail_display_info_id": item.get("display_info_id"),
                         "target_item_display_info_id": display_target,
                         "file_data_ids": item.get("file_data_ids", []),
                         "cdn_build": manifest.get("build")})
    state["item_template"] = item_alloc.mapping
    chunk_size = int(config["chunk_size"])
    comment = f"Retail item import ({project})"
    chunks = core.migration_chunks(rows, vendor=args.vendor, comment=comment, size=chunk_size)
    retire_sql = core.retire_sql(sorted(retired))
    if retire_sql:
        chunks = [retire_sql] + chunks

    stage = Path(args.stage or config["stage_dir"])
    plan = [core.PlanRow(r["entry"], r["name"], r["displayid"], "INSERT") for r in rows]
    if db is not None:
        try:
            plan = core.plan_live(db, rows, owned_before)
        except Exception as error:  # offline dry runs still print the offline plan
            print(f"note: live database unavailable ({error}); showing offline plan", file=sys.stderr)
            if not args.apply_live:
                db = None
    print(core.format_plan(plan))
    client_files, client_collisions = {}, 0
    if config.get("client_data_dir"):
        try:
            client_files, client_lines, client_collisions = client_tables(config, dbc, rows, display_owned_before, owned_before)
        except Exception as error:  # a missing StormLib or unreadable archive must not look like success
            print(f"error: cannot build the client tables from {config['client_data_dir']}: {error}", file=sys.stderr)
            return 1
        print("\nCLIENT  : full tables for the client MPQ (the client ignores .dbcN- continuations)")
        for line in client_lines:
            print("          " + line)
    else:
        print("\nwarning : client_data_dir is not set: the staged client DBC is a continuation fragment, which the 3.3.5a client\n"
              "          does not read (new items show ? icons and no model). Set client_data_dir in sync_config.json.", file=sys.stderr)
    print(f"\nDBC     : {cont_file}  ({len(dbc.rows)} rows) -> {stage / 'DBFilesClient'} and {config['_continuations']}")
    print(f"SQL     : {len(chunks)} migration file(s) of up to {chunk_size} item(s) -> {config['_sql']}")
    if client_collisions and not args.force:
        print(f"error: {client_collisions} new client DBC row(s) collide with rows this tool does not own (use --force)", file=sys.stderr)
        return 1
    if args.dry_run:
        print("\n--dry-run: nothing written. First migration file:")
        preview = chunks[0].splitlines() if chunks else []
        print("\n".join(preview[:40]) + (f"\n... ({len(preview) - 40} more line(s))" if len(preview) > 40 else ""))
        return 0
    if any(p.action == "CONFLICT" for p in plan) and not args.force:
        print("error: item_template entries already used by rows this tool does not own (use --force to overwrite)",
              file=sys.stderr)
        return 1

    # --- writes ----------------------------------------------------------------------------
    stage_bytes = dbc.to_bytes()
    sidecar = stage / "Esteria" / "RetailArmorTextures.csv"
    sidecar.parent.mkdir(parents=True, exist_ok=True)
    sidecar.write_text("# displayId,modelIndex,textureType,file (typed M2 texture layers of Retail armor models)\n"
                       + "".join(row + "\n" for row in sorted(set(armor_rows))), encoding="utf-8", newline="\n")
    if client_files:
        (stage / "DBFilesClient").mkdir(parents=True, exist_ok=True)
        for name, content in client_files.items():
            (stage / "DBFilesClient" / name).write_bytes(content)
        stale = stage / "DBFilesClient" / cont_file   # a fragment from an older run must not ship
        if stale.exists():
            stale.unlink()
    else:
        dbc.write(stage / "DBFilesClient" / cont_file)
    (Path(config["_continuations"])).mkdir(parents=True, exist_ok=True)
    own_path.write_bytes(stage_bytes)
    sql_dir = core.check_sql_target(config["_sql"])
    sql_dir.mkdir(parents=True, exist_ok=True)
    # Reuse a file whose content is already identical (idempotent reruns); everything else is a new,
    # later-timestamped file, so the updater replays older versions first and the newest wins.
    known = {p.read_text(encoding="utf-8"): p for p in sql_dir.glob("rev_*_retail_items.sql")}
    fresh = [chunk for chunk in chunks if chunk not in known]
    names = core.migration_filenames("retail_items", len(fresh))
    written = 0
    for chunk, name in zip(fresh, names):
        (sql_dir / name).write_text(chunk, encoding="utf-8", newline="\n")
        written += 1
    print(f"SQL: wrote {written} new file(s), kept {len(chunks) - written} identical existing file(s)")
    stage.mkdir(parents=True, exist_ok=True)
    (stage / "sync_map.json").write_text(json.dumps(
        {"schema": 1, "project": project, "continuation": cont_file, "items": sync_map,
         "dbc_sha256": hashlib.sha256(stage_bytes).hexdigest(),
         "client_tables": {name: hashlib.sha256(content).hexdigest() for name, content in client_files.items()}}, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if args.apply_live:
        backup_dir = Path(args.backup_dir or stage.parent / "backups")
        backup_dir.mkdir(parents=True, exist_ok=True)
        backup = core.backup_rows(db, [r["entry"] for r in rows])
        digest = hashlib.sha256("".join(chunks).encode()).hexdigest()[:12]
        backup_path = backup_dir / f"item_template-{digest}.sql"
        backup_path.write_text(backup, encoding="utf-8")
        print(f"backed up affected rows -> {backup_path}")
        if retire_sql:
            db.execute(retire_sql)
        done = core.apply_upsert(db, rows, chunk_size,
                                 progress=lambda n, total: print(f"  upserted {n}/{total}", file=sys.stderr))
        print(f"upserted {done} item_template rows in {len(core.chunked(rows, chunk_size))} transaction(s)")
    state["owned_entries"] = sorted(set(state.get("owned_entries", [])) | {r["entry"] for r in rows})
    core.save_state(state_path, state)
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", help="sync_config.json")
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("resolve", help="run RetailAssetProbe --resolve-items headlessly")
    r.add_argument("--items", help="comma-separated item IDs (or display:<id> / set:<id>); optional with --all-*")
    r.add_argument("--out", type=Path, default=Path("resolved_manifest.json"))
    r.add_argument("--probe"); r.add_argument("--retail-ini")
    r.add_argument("--product"); r.add_argument("--region"); r.add_argument("--cache")
    r.add_argument("--build"); r.add_argument("--icons-dir")
    r.add_argument("--all-sets", action="store_true", help="every transmog set (TransmogSet + TransmogSetItem)")
    r.add_argument("--all-armor", action="store_true", help="every armor appearance: all slots and armor types")
    r.add_argument("--all-weapons", action="store_true", help="weapons, shields, off-hands, bows/guns/crossbows, wands")
    r.add_argument("--all-catalog", action="store_true", help="--all-sets --all-armor --all-weapons")
    r.add_argument("--min-expansion", type=int); r.add_argument("--max-expansion", type=int)
    r.add_argument("--skip-uncollectible", action="store_true",
                   help="heuristic: drop Poor-quality items and items without an ItemSparse row")
    r.add_argument("--every-item", action="store_true", help="keep every item, not one per ItemAppearance")
    r.add_argument("--workers", type=int, help="parallel CDN workers (default 8)")
    r.add_argument("--journal", help="checkpoint file (default: cdn_fetch_state-<build>.json beside --out)")
    r.add_argument("--graph-only", action="store_true", help="resolve the dependency graph without downloading textures/skins")
    s = sub.add_parser("sync", help="write DBC continuation + pending_db_world SQL (+ live upsert)")
    s.add_argument("--manifest", type=Path, required=True)
    s.add_argument("--stage"); s.add_argument("--vendor", type=int, help="also add items to this npc_vendor entry")
    s.add_argument("--dry-run", action="store_true", help="print the plan and SQL, write nothing")
    s.add_argument("--dry-run-live", action="store_true", help="with --dry-run: also diff against the live DB")
    s.add_argument("--apply-live", action="store_true", help="upsert item_template into the live acore_world")
    s.add_argument("--force", action="store_true"); s.add_argument("--backup-dir"); s.add_argument("--mysql")
    args = parser.parse_args(argv)
    args.db_client = getattr(args, "db_client", None)
    config = load_config(args.config)
    return resolve(args, config) if args.command == "resolve" else sync(args, config)


if __name__ == "__main__":
    sys.exit(main())
