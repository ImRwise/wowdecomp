#!/usr/bin/env python3
"""Registers the synced Midnight armor as item sets so the custom Transmogrify window lists them under "Sets".

The window (mod-transmog, transmog_client.cpp BuildSets) lists every ItemSet.dbc row holding armor the class can wear.
The synced Retail items are not in any stock set, so this tool groups them the way Retail names them -- every armor piece
called "<Set>'s <Piece>" belongs to "<Set>" -- and writes an ItemSet.dbc continuation (mod-wxl-dbc) with one row per set.
Only the server reads the rows: the window gets set names and pieces from the server, so the client needs no change.

    python Tools/ServerSync/midnight_sets.py [--min-retail-id 245000] [--dry-run]

Midnight = items whose description says "Retail item N" with N >= --min-retail-id and RequiredLevel >= --min-level.
A name group with several armor types splits by type; a group still holding more than 17 pieces (the PvP seasons ship
several colour variants of every slot) splits into outfits at the first repeated slot, in item-ID order. Restart the
worldserver afterwards: continuations load at start-up.
"""
import argparse, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dbc_io import Dbc, continuation_name, SCHEMAS  # noqa: E402

KINDS = SCHEMAS["ItemSet"]
MAX_PIECES = 17
ARMOR_TYPES = {1: "Cloth", 2: "Leather", 3: "Mail", 4: "Plate"}
INV_RANK = {1: 1, 3: 2, 16: 3, 5: 4, 20: 4, 4: 5, 19: 6, 9: 7, 10: 8, 6: 9, 7: 10, 8: 11}
# Chest and robe are one slot for outfit purposes.
INV_SLOT = {20: 5}

QUERY = """select entry, name, subclass, InventoryType, RequiredLevel, description, displayid
 from item_template where entry >= 1000000 and class = 4 and subclass between 1 and 4 and name like '%''s %'
 order by entry"""


def fetch(args):
    raw = subprocess.run([args.mysql, "-u" + args.user, "--host=" + args.host, args.db, "-N", "-B", "-e", QUERY],
                         check=True, capture_output=True).stdout.decode("utf-8")
    for line in raw.splitlines():
        f = line.split("\t")
        if len(f) < 7:
            continue
        match = re.search(r"Retail item (\d+)", f[5])
        yield dict(entry=int(f[0]), name=f[1], sub=int(f[2]), inv=int(f[3]), level=int(f[4]), display=int(f[6]),
                   retail=int(match.group(1)) if match else 0)


def split_outfits(pieces):
    """Chunks of `pieces` (sorted by entry) in which no slot repeats and none exceeds MAX_PIECES."""
    chunks, current, used = [], [], set()
    for item in pieces:
        slot = INV_SLOT.get(item["inv"], item["inv"])
        if slot in used or len(current) == MAX_PIECES:
            chunks.append(current)
            current, used = [], set()
        current.append(item)
        used.add(slot)
    if current:
        chunks.append(current)
    return chunks


def build_sets(items):
    # Retail names the same appearance twice (Robe/Gown, Hat/Cap: one per faction or sex). One piece per appearance.
    unique, seen = [], set()
    for item in sorted(items, key=lambda i: i["entry"]):
        if (item["name"].split("'s ", 1)[0], item["display"]) not in seen:
            seen.add((item["name"].split("'s ", 1)[0], item["display"]))
            unique.append(item)
    groups = {}
    for item in unique:
        prefix = item["name"].split("'s ", 1)[0].strip()
        if prefix:
            groups.setdefault(prefix, []).append(item)
    sets = []
    for prefix, pieces in sorted(groups.items()):
        by_type = {}
        for item in pieces:
            by_type.setdefault(item["sub"], []).append(item)
        split_by_type = len(by_type) > 1 and len(pieces) > 12
        for sub, members in sorted(by_type.items()) if split_by_type else [(None, pieces)]:
            label = prefix + (" (%s)" % ARMOR_TYPES[sub] if sub is not None else "")
            chunks = split_outfits(sorted(members, key=lambda i: i["entry"]))
            chunks = [c for c in chunks if len(c) >= 2]
            for number, chunk in enumerate(chunks, 1):
                name = label if len(chunks) == 1 else "%s %d" % (label, number)
                sets.append((name[:40], sorted(chunk, key=lambda i: (INV_RANK.get(i["inv"], 99), i["entry"]))))
    return sets


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mysql", default=r"C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--user", default="root")
    ap.add_argument("--db", default="acore_world")
    ap.add_argument("--server-data", default=r"C:\Esteria\server\data")
    ap.add_argument("--project", default="native-retail-midnight-sets")
    ap.add_argument("--tier", type=int, default=1)
    ap.add_argument("--first-id", type=int, default=10001, help="first ItemSet ID (stock sets end at 901)")
    ap.add_argument("--min-retail-id", type=int, default=245000)
    ap.add_argument("--min-level", type=int, default=80)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    items = [i for i in fetch(args) if i["retail"] >= args.min_retail_id and i["level"] >= args.min_level]
    sets = build_sets(items)
    dbc = Dbc(KINDS)
    for index, (name, pieces) in enumerate(sets):
        ids = [p["entry"] for p in pieces][:MAX_PIECES]
        row = [args.first_id + index, name] + [""] * 15 + [16712190] + ids + [0] * (MAX_PIECES - len(ids)) + [0] * 16 + [0, 0]
        dbc.upsert(row)
    print("%d Midnight armor pieces -> %d sets" % (len(items), len(sets)))
    for name, pieces in sets[:60]:
        print("  %-38s %2d pieces" % (name, len(pieces)))
    if len(sets) > 60:
        print("  ... %d more" % (len(sets) - 60))
    if args.dry_run:
        return
    out = Path(args.server_data) / "dbc-continuations" / continuation_name("ItemSet", args.project, args.tier)
    dbc.write(out)
    print("wrote %s (%d rows); restart the worldserver to load it" % (out, len(dbc.rows)))


if __name__ == "__main__":
    main()
