import argparse
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import client_dbc  # noqa: E402
import dbc_io  # noqa: E402
import esteria_sync  # noqa: E402
import sync_core as core  # noqa: E402

REAL_DBC = Path(os.environ.get("ESTERIA_REAL_DBC", r"C:\Esteria\server\data\dbc\ItemDisplayInfo.dbc"))


def parse_like_module(name):
    """Port of ModWxlDbc::ParseContinuationFileName (WxlDbcScan.cpp)."""
    if name.lower().endswith((".bak", ".backup", ".old", ".orig", ".tmp")):
        return None
    pos = name.find(".dbc")
    if pos < 0 or pos + 4 >= len(name) or not "1" <= name[pos + 4] <= "9":
        return None
    if pos + 5 >= len(name) or name[pos + 5] != "-":
        return None
    project = name[pos + 6:]
    if not project or any(not (c.isalnum() or c in "-_") for c in project):
        return None
    return name[:pos] + ".dbc", int(name[pos + 4]), project


def sample_display(display_id, name="item"):
    return core.display_row({"id": 1, "model_file_ids": [123], "texture_file_ids": [456], "geoset_groups": [1, 2, 3],
                             "helmet_geoset_vis": [4, 5], "material_file_ids": [7, 8], "icon_name": name}, display_id)


class DbcTests(unittest.TestCase):
    def test_header_and_layout(self):
        dbc = dbc_io.Dbc(dbc_io.schema_for("ItemDisplayInfo"))
        dbc.upsert(sample_display(700001))
        data = dbc.to_bytes()
        magic, rows, fields, record, pool = struct.unpack_from("<4s4I", data)
        self.assertEqual((magic, rows, fields, record), (b"WDBC", 1, 25, 100))
        self.assertEqual(len(data), 20 + 100 + pool)
        self.assertEqual(data[20 + 100], 0)  # pool starts with the empty string

    def test_round_trip_is_byte_identical(self):
        dbc = dbc_io.Dbc(dbc_io.schema_for("ItemDisplayInfo"))
        for i in (700003, 700001, 700002):
            dbc.upsert(sample_display(i, f"icon{i}"))
        first = dbc.to_bytes()
        again = dbc_io.Dbc.from_bytes(first, dbc.kinds)
        self.assertEqual(again.to_bytes(), first)
        self.assertEqual([r[0] for r in again.rows], [700001, 700002, 700003])
        self.assertEqual(again.by_id()[700001][1], "retail/123.m2")

    def test_float_and_signed(self):
        dbc = dbc_io.Dbc("uif")
        dbc.upsert([1, -5, 1.5])
        back = dbc_io.Dbc.from_bytes(dbc.to_bytes(), "uif")
        self.assertEqual(back.rows[0], [1, -5, 1.5])

    def test_rejects_bad_files(self):
        with self.assertRaises(dbc_io.DbcError):
            dbc_io.Dbc.from_bytes(b"XXXX" + bytes(16), "u")
        good = dbc_io.Dbc("us")
        good.upsert([1, "a"])
        with self.assertRaises(dbc_io.DbcError):
            dbc_io.Dbc.from_bytes(good.to_bytes(), "uuu")
        with self.assertRaises(dbc_io.DbcError):
            dbc_io.Dbc.from_bytes(good.to_bytes()[:-1], "us")

    @unittest.skipUnless(REAL_DBC.exists(), "no server ItemDisplayInfo.dbc present")
    def test_real_base_file(self):
        real = dbc_io.Dbc.read(REAL_DBC, dbc_io.schema_for("ItemDisplayInfo"))
        self.assertGreater(len(real.rows), 50000)
        rewritten = dbc_io.Dbc.from_bytes(real.to_bytes(), real.kinds)
        self.assertEqual(sorted(real.rows), sorted(rewritten.rows))
        self.assertEqual(dbc_io.read_ids(REAL_DBC), real.ids())

    def test_upsert_replaces(self):
        dbc = dbc_io.Dbc("us")
        dbc.upsert([5, "a"]); dbc.upsert([5, "b"])
        self.assertEqual(dbc.rows, [[5, "b"]])


class ContinuationTests(unittest.TestCase):
    def test_names_match_module_parser(self):
        name = dbc_io.continuation_name("ItemDisplayInfo", "retail-armor", 1)
        self.assertEqual(name, "ItemDisplayInfo.dbc1-retail-armor")
        self.assertEqual(parse_like_module(name), ("ItemDisplayInfo.dbc", 1, "retail-armor"))
        self.assertEqual(parse_like_module(dbc_io.continuation_name("Spell", "x_y", 9))[1], 9)
        for bad in ("bad slug", "x.bak", "a/b"):
            with self.assertRaises(dbc_io.DbcError):
                dbc_io.continuation_name("Spell", bad)
        with self.assertRaises(dbc_io.DbcError):
            dbc_io.continuation_name("Spell", "ok", 0)

    def test_allocator_avoids_base_and_other_projects(self):
        with tempfile.TemporaryDirectory() as tmp:
            base, cont = Path(tmp, "dbc"), Path(tmp, "cont")
            other = dbc_io.Dbc("us"); other.upsert([700001, "x"]); other.upsert([700002, "y"])
            other.write(cont / "ItemDisplayInfo.dbc1-other")
            mine = dbc_io.Dbc("us"); mine.upsert([700003, "z"])
            mine.write(cont / "ItemDisplayInfo.dbc1-mine")
            baseline = dbc_io.Dbc("us"); baseline.upsert([700000, "b"])
            baseline.write(base / "ItemDisplayInfo.dbc")
            used = core.existing_ids("ItemDisplayInfo", base, cont, own_project="mine")
            self.assertEqual(used, {700000, 700001, 700002})
            alloc = core.IdAllocator(used, 700000)
            self.assertEqual(alloc.allocate("a"), 700003)  # own project's IDs are reusable
            self.assertEqual(alloc.allocate("b"), 700004)
            self.assertEqual(alloc.allocate("a"), 700003)  # stable
            resumed = core.IdAllocator(used, 700000, alloc.mapping)
            self.assertEqual(resumed.allocate("c"), 700005)

    def test_ceiling(self):
        alloc = core.IdAllocator(set(), 10, ceiling=10)
        alloc.allocate("a")
        with self.assertRaises(OverflowError):
            alloc.allocate("b")


class SqlTests(unittest.TestCase):
    ITEM = {"item_id": 100, "name": "Dragon's Helm", "class_id": 4, "subclass_id": 4, "inventory_type": 1,
            "quality": 4, "item_level": 80, "display_info_id": 9}

    def test_migration_is_idempotent_and_linted(self):
        row = core.item_row(self.ITEM, 1000000, 700001, 12345)
        sql = core.migration_sql([row], vendor=555)
        self.assertLess(sql.index("DELETE FROM `item_template`"), sql.index("INSERT INTO `item_template`"))
        self.assertLess(sql.index("DELETE FROM `npc_vendor`"), sql.index("INSERT INTO `npc_vendor`"))
        self.assertIn("'Dragon''s Helm'", sql)
        self.assertNotIn(";;", sql)
        self.assertNotIn("\n\n\n", sql)
        self.assertTrue(sql.endswith(";\n"))

    def test_filename(self):
        from datetime import datetime, timezone
        name = core.migration_filename("Retail Items!", datetime(2026, 11, 14, 1, 2, 3, 456000, tzinfo=timezone.utc))
        self.assertEqual(name, "rev_20261114010203456_retail_items.sql")

    def test_forbidden_targets(self):
        for bad in ("data/sql/base/db_world", "data/sql/archive/db_world", "data/sql/updates/db_world",
                    "data/sql/updates/db_characters", "somewhere/else"):
            with self.assertRaises(PermissionError, msg=bad):
                core.check_sql_target(Path("/repo") / bad)
        core.check_sql_target(Path("/repo/data/sql/updates/pending_db_world"))
        core.check_sql_target(Path("/repo/modules/mod-custom-server/data/sql/db_world"))


class FakeDb:
    def __init__(self, rows=None):
        self.rows = rows or {}
        self.executed = []

    def query(self, sql):
        if "SELECT *" in sql:
            return [dict(r) for r in self.rows.values()]
        return [{"entry": str(k), "name": v["name"], "displayid": str(v["displayid"])} for k, v in self.rows.items()]

    def execute(self, sql):
        self.executed.append(sql)


MANIFEST = {"schema": 1, "build": "88d5e8ef05ec5a8446cdec93d142d7c6", "build_number": 63000,
            "items": [{"item_id": 100, "name": "Dragon's Helm", "class_id": 4, "subclass_id": 4, "inventory_type": 1,
                       "quality": 4, "item_level": 80, "display_info_id": 9, "file_data_ids": [123, 456]}],
            "displays": [{"id": 9, "model_file_ids": [123], "texture_file_ids": [456], "geoset_groups": [1, 0, 0],
                          "helmet_geoset_vis": [2, 0], "material_file_ids": [], "icon_name": "INV_Helm_01"}]}


class ScaleTests(unittest.TestCase):
    def test_allocator_is_linear_and_skips_used_and_reserved(self):
        import time
        alloc = core.IdAllocator({1000005, 1000006}, 1000000, reserved=[[1000010, 1000019], [1000015, 1000030]])
        got = [alloc.allocate(f"k{i}") for i in range(12)]
        self.assertEqual(got, [1000000, 1000001, 1000002, 1000003, 1000004, 1000007, 1000008, 1000009, 1000031, 1000032,
                               1000033, 1000034])
        big = core.IdAllocator(set(range(1000000, 1010000, 2)), 1000000)
        start = time.time()
        ids = [big.allocate(f"i{n}") for n in range(60000)]
        self.assertLess(time.time() - start, 3.0)               # was O(n^2): minutes at this size
        self.assertEqual(len(set(ids)), 60000)
        self.assertTrue(all(i not in set(range(1000000, 1010000, 2)) for i in ids))
        self.assertEqual(big.allocate("i7"), ids[7])             # stable
        self.assertEqual(core.merge_ranges([[5, 9], [1, 3], [4, 4], [20, 30]]), [[1, 9], [20, 30]])
        with self.assertRaises(ValueError):
            core.merge_ranges([[9, 1]])
        tight = core.IdAllocator(set(), 10, ceiling=10)
        tight.allocate("a")
        with self.assertRaises(OverflowError):
            tight.allocate("b")

    def test_mapped_ids_taken_by_others_are_reported(self):
        alloc = core.IdAllocator({5}, 1, {"a": 5, "b": 6}, reserved=[[6, 6]])
        self.assertEqual(alloc.conflicts, ["a", "b"])
        self.assertEqual(core.IdAllocator({7}, 1, {"a": 5}).conflicts, [])

    def rows(self, count, start=1000000):
        return [core.item_row({"item_id": n, "name": f"Item {n}", "class_id": 4}, start + n, 700000 + n, 63000) for n in range(count)]

    def test_chunked_migrations_are_transactional_and_cover_every_row(self):
        rows = self.rows(4500)
        chunks = core.migration_chunks(rows, size=2000, comment="bulk")
        self.assertEqual(len(chunks), 3)
        for chunk in chunks:
            self.assertTrue(chunk.startswith("START TRANSACTION;\n") and chunk.endswith("COMMIT;\n"))
        joined = "".join(chunks)
        self.assertEqual(joined.count("INSERT INTO `item_template`"), 3)
        for sample in (1000000, 1002345, 1004499):
            self.assertEqual(joined.count(f"\n({sample}, "), 1)
        vendor = core.migration_chunks(rows[:3], vendor=77, size=2)
        self.assertEqual(len(vendor), 2)
        self.assertIn("npc_vendor", vendor[0])
        self.assertEqual(core.migration_chunks([], size=10), [])

    def test_migration_filenames_are_unique_and_ordered(self):
        from datetime import datetime, timezone
        names = core.migration_filenames("retail_items", 30, datetime(2026, 11, 14, 1, 2, 3, 999000, tzinfo=timezone.utc))
        self.assertEqual(len(set(names)), 30)
        self.assertEqual(names, sorted(names))
        self.assertTrue(all(n.startswith("rev_2026111401020") and n.endswith("_retail_items.sql") for n in names))

    def test_apply_upsert_batches_in_transactions(self):
        db = FakeDb()
        seen = []
        total = core.apply_upsert(db, self.rows(5001), 2000, progress=lambda n, t: seen.append((n, t)))
        self.assertEqual(total, 5001)
        self.assertEqual(len(db.executed), 3)
        self.assertEqual(seen[-1], (5001, 5001))
        for sql in db.executed:
            self.assertTrue(sql.startswith("START TRANSACTION;") and sql.rstrip().endswith("COMMIT;"))
            self.assertIn("ON DUPLICATE KEY UPDATE", sql)

    def test_backup_and_plan_query_in_slices(self):
        class Counting(FakeDb):
            def __init__(self):
                super().__init__()
                self.queries = []

            def query(self, sql):
                self.queries.append(sql)
                return []

        db = Counting()
        rows = self.rows(2500)
        core.plan_live(db, rows, set())
        self.assertEqual(len(db.queries), 3)
        self.assertTrue(all(len(q) < 20000 for q in db.queries))
        db.queries.clear()
        core.backup_rows(db, [r["entry"] for r in rows])
        self.assertEqual(len(db.queries), 3)

    def test_sql_scan_ignores_own_files_and_low_numbers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "rev_1_other.sql").write_text("INSERT INTO `item_template` VALUES (1000042, 1), (12345, 2);")
            (root / "rev_2_retail_items.sql").write_text("INSERT INTO `item_template` VALUES (1000043, 1);")
            (root / "rev_3_creature.sql").write_text("INSERT INTO `creature` VALUES (1000044, 1);")
            self.assertEqual(core.scan_sql_entries([root, root / "missing", None], 1000000), {1000042})

    def test_bulk_dbc_upsert_is_fast_and_replaces(self):
        import time
        dbc = dbc_io.Dbc(dbc_io.schema_for("ItemDisplayInfo"))
        start = time.time()
        for n in range(50000):
            dbc.upsert(sample_display(700000 + n, f"i{n}"))
        self.assertLess(time.time() - start, 6.0)
        dbc.upsert(sample_display(700010, "replaced"))
        self.assertEqual(len(dbc.rows), 50000)
        self.assertEqual(dbc.by_id()[700010][5], "replaced")
        again = dbc_io.Dbc.from_bytes(dbc.to_bytes(), dbc.kinds)
        self.assertEqual(len(again.rows), 50000)

    def test_plan_output_is_bounded(self):
        plan = [core.PlanRow(i, f"n{i}", 1, "INSERT") for i in range(50000)] + [core.PlanRow(7, "x", 1, "CONFLICT")]
        text = core.format_plan(plan, limit=100)
        self.assertLess(len(text.splitlines()), 220)
        self.assertIn("CONFLICT", text)
        self.assertIn("INSERT=50000", text)


class EndToEndTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        (root / "server/data/dbc").mkdir(parents=True)
        base = dbc_io.Dbc(dbc_io.schema_for("ItemDisplayInfo"))
        base.upsert(sample_display(700000, "base"))
        base.write(root / "server/data/dbc/ItemDisplayInfo.dbc")
        (root / "manifest.json").write_text(json.dumps(MANIFEST))
        (root / "cfg.json").write_text(json.dumps({
            "server_root": str(root / "EsteriaWoW"), "server_data_dir": str(root / "server/data"),
            "stage_dir": str(root / "stage"), "state_file": str(root / "state.json"),
            "id_floor": {"ItemDisplayInfo": 700000, "item_template": 1000000}}))
        self.root = root

    def tearDown(self):
        self.tmp.cleanup()

    def args(self, **extra):
        values = dict(manifest=self.root / "manifest.json", stage=None, vendor=None, dry_run=False,
                      dry_run_live=False, apply_live=False, force=False, backup_dir=None, mysql=None, db_client=None)
        values.update(extra)
        return argparse.Namespace(**values)

    def config(self):
        return esteria_sync.load_config(str(self.root / "cfg.json"))

    def test_dry_run_writes_nothing(self):
        self.assertEqual(esteria_sync.sync(self.args(dry_run=True), self.config()), 0)
        self.assertFalse((self.root / "stage").exists())
        self.assertFalse((self.root / "EsteriaWoW").exists())

    def test_full_sync_and_rerun_is_stable(self):
        self.assertEqual(esteria_sync.sync(self.args(), self.config()), 0)
        cont = self.root / "server/data/dbc-continuations/ItemDisplayInfo.dbc1-retail-armor"
        rows = dbc_io.Dbc.read(cont, dbc_io.schema_for("ItemDisplayInfo")).rows
        self.assertEqual([r[0] for r in rows], [700001])  # 700000 is taken by the base DBC
        self.assertEqual(rows[0][1], "retail/123.m2")
        self.assertEqual(cont.read_bytes(), (self.root / "stage/DBFilesClient/ItemDisplayInfo.dbc1-retail-armor").read_bytes())
        sql_dir = self.root / "EsteriaWoW/data/sql/updates/pending_db_world"
        files = list(sql_dir.glob("rev_*_retail_items.sql"))
        self.assertEqual(len(files), 1)
        self.assertIn("1000000", files[0].read_text())
        self.assertIn("700001", files[0].read_text())
        sync_map = json.loads((self.root / "stage/sync_map.json").read_text())
        self.assertEqual(sync_map["items"][0]["item_template_entry"], 1000000)
        self.assertEqual(sync_map["items"][0]["target_item_display_info_id"], 700001)
        self.assertEqual(esteria_sync.sync(self.args(), self.config()), 0)
        self.assertEqual(len(list(sql_dir.glob("rev_*.sql"))), 1)  # unchanged SQL is not duplicated
        self.assertEqual(cont.read_bytes()[:4], b"WDBC")

    def test_apply_live_backs_up_and_upserts(self):
        db = FakeDb()
        self.assertEqual(esteria_sync.sync(self.args(apply_live=True, db_client=db), self.config()), 0)
        self.assertEqual(len(db.executed), 1)
        self.assertIn("ON DUPLICATE KEY UPDATE", db.executed[0])
        self.assertTrue(list((self.root / "backups").glob("item_template-*.sql")))

    def test_foreign_live_row_is_allocated_around(self):
        db = FakeDb({1000000: {"entry": "1000000", "name": "Someone else's", "displayid": "1"}})
        self.assertEqual(esteria_sync.sync(self.args(apply_live=True, db_client=db), self.config()), 0)
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["item_template"], {"retail:100": 1000001})  # 1000000 belongs to someone else
        self.assertNotIn("(1000000,", "".join(db.executed))                  # and is never written
        self.assertIn("1000001", db.executed[-1])

    def test_entries_owned_by_the_client_supplement_are_retired_and_remapped(self):
        # The client merges Data/Retail/<tables>/item.dbc onto Item.dbc and aborts on a duplicate ID, so an entry it defines
        # can never be ours: the old one is retired (DELETE) and the item gets a fresh entry.
        registry = self.root / "supplement"
        registry.mkdir()
        item = dbc_io.Dbc(dbc_io.schema_for("Item"))
        item.upsert([1000000, 4, 4, -1, 6, 1, 6, 0])
        item.write(registry / "item.dbc")
        (self.root / "state.json").write_text(json.dumps(
            {"schema": 1, "ItemDisplayInfo": {}, "item_template": {"retail:100": 1000000}, "owned_entries": [1000000]}))
        config = self.config()
        config["client_supplement_dir"] = str(registry)
        self.assertEqual(esteria_sync.sync(self.args(), config), 0)
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["item_template"], {"retail:100": 1000001})
        self.assertEqual(state["retired_entries"], [1000000])
        sql = "".join(p.read_text() for p in (self.root / "EsteriaWoW/data/sql/updates/pending_db_world").glob("rev_*.sql"))
        self.assertIn("DELETE FROM `item_template` WHERE `entry` IN (1000000);", sql)
        # a rerun keeps the mapping and the retire statement (idempotent)
        self.assertEqual(esteria_sync.sync(self.args(), config), 0)
        self.assertEqual(json.loads((self.root / "state.json").read_text())["item_template"], {"retail:100": 1000001})

    def test_previously_mapped_entry_taken_by_foreign_row_aborts(self):
        # The state file maps our item to 1000000, but we do not own it and the live table already has it.
        (self.root / "state.json").write_text(json.dumps(
            {"schema": 1, "ItemDisplayInfo": {}, "item_template": {"retail:100": 1000000}, "owned_entries": []}))
        db = FakeDb({1000000: {"entry": "1000000", "name": "Someone else's", "displayid": "1"}})
        self.assertEqual(esteria_sync.sync(self.args(apply_live=True, db_client=db), self.config()), 1)
        self.assertEqual(db.executed, [])

    def test_sql_tree_entries_are_never_reused(self):
        sql = self.root / "EsteriaWoW/data/sql/updates/pending_db_world"
        sql.mkdir(parents=True)
        (sql / "rev_1_custom.sql").write_text("DELETE FROM `item_template` WHERE `entry` IN (1000000, 1000001);\n"
                                              "INSERT INTO `item_template` (`entry`,`name`) VALUES\n(1000002, 'x'),\n(1000003, 'y');\n")
        self.assertEqual(esteria_sync.sync(self.args(), self.config()), 0)
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["item_template"], {"retail:100": 1000004})

    def test_reserved_ranges_are_skipped(self):
        cfg = json.loads((self.root / "cfg.json").read_text())
        cfg["reserved_ranges"] = {"item_template": [[1000000, 1000099]], "ItemDisplayInfo": [[700001, 700009]]}
        (self.root / "cfg.json").write_text(json.dumps(cfg))
        self.assertEqual(esteria_sync.sync(self.args(), self.config()), 0)
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["item_template"], {"retail:100": 1000100})
        self.assertEqual(state["ItemDisplayInfo"], {"retail:9": 700010})

    def test_conflict_with_foreign_row_when_forced_is_backed_up(self):
        # A foreign row in an entry we already map is overwritten only with --force, and is backed up first.
        (self.root / "state.json").write_text(json.dumps(
            {"schema": 1, "ItemDisplayInfo": {}, "item_template": {"retail:100": 1000000}, "owned_entries": [1000000]}))
        db = FakeDb({1000000: {"entry": "1000000", "name": "Someone else's", "displayid": "1"}})
        self.assertEqual(esteria_sync.sync(self.args(apply_live=True, db_client=db, force=True), self.config()), 0)
        backup = next((self.root / "backups").glob("item_template-*.sql")).read_text()
        self.assertIn("Someone else''s", backup)

    def test_continuation_merges_across_runs(self):
        esteria_sync.sync(self.args(), self.config())
        second = json.loads(json.dumps(MANIFEST))
        second["items"][0].update(item_id=200, display_info_id=10)
        second["displays"][0]["id"] = 10
        (self.root / "manifest.json").write_text(json.dumps(second))
        esteria_sync.sync(self.args(), self.config())
        cont = self.root / "server/data/dbc-continuations/ItemDisplayInfo.dbc1-retail-armor"
        ids = [r[0] for r in dbc_io.Dbc.read(cont, dbc_io.schema_for("ItemDisplayInfo")).rows]
        self.assertEqual(ids, [700001, 700002])


class ClientTableTests(unittest.TestCase):
    """The 3.3.5a client reads complete tables only: new rows are merged into its winning copy."""

    def base(self, ids):
        table = dbc_io.Dbc(dbc_io.schema_for("ItemDisplayInfo"))
        for i in ids:
            table.upsert([i] + [""] * 6 + [0] * 8 + [""] * 8 + [0, 0])
        return table.to_bytes()

    def row(self, i, icon="RetailIcon_1"):
        return [i, "retail/1.m2", "", "retail/2.blp", "", icon, ""] + [0] * 8 + [""] * 8 + [0, 0]

    def test_merge_adds_new_rows_and_keeps_every_base_row(self):
        kinds = dbc_io.schema_for("ItemDisplayInfo")
        merged, report = client_dbc.merge_table(self.base([1, 2, 3]), kinds, [self.row(700000), self.row(700001)], set(),
                                                "ItemDisplayInfo", "patch-Z.MPQ")
        self.assertEqual(sorted(merged.ids()), [1, 2, 3, 700000, 700001])
        self.assertEqual((report.base_rows, report.added, report.replaced_own, report.collisions), (3, 2, 0, []))

    def test_foreign_row_with_the_same_id_is_a_collision_and_is_not_overwritten(self):
        kinds = dbc_io.schema_for("ItemDisplayInfo")
        merged, report = client_dbc.merge_table(self.base([1, 700000]), kinds, [self.row(700000, "Mine")], set(),
                                                "ItemDisplayInfo", "patch-Z.MPQ")
        self.assertEqual(report.collisions, [700000])
        self.assertEqual(merged.by_id()[700000][5], "")          # the other project's row is untouched

    def test_own_rows_from_an_earlier_run_are_refreshed_not_flagged(self):
        kinds = dbc_io.schema_for("ItemDisplayInfo")
        merged, report = client_dbc.merge_table(self.base([1, 700000]), kinds, [self.row(700000, "Updated")], {700000},
                                                "ItemDisplayInfo", "ours")
        self.assertEqual((report.collisions, report.replaced_own, report.added), ([], 1, 0))
        self.assertEqual(merged.by_id()[700000][5], "Updated")

    def test_rows_with_out_of_pool_strings_are_kept_bit_for_bit(self):
        kinds = dbc_io.schema_for("ItemDisplayInfo")
        data = bytearray(self.base([1, 2]))
        # corrupt row 2's ModelTexture[0] offset (column 3) to point far past the pool, as another pack's row does
        record = 20 + 1 * 100
        struct.pack_into("<I", data, record + 3 * 4, 5000000)
        merged, report = client_dbc.merge_table(bytes(data), kinds, [self.row(700000)], set(), "ItemDisplayInfo", "x")
        out = merged.to_bytes()
        self.assertEqual(report.out_of_pool_rows, [2])
        self.assertEqual(report.collisions, [])
        self.assertIn(struct.pack("<I", 5000000), out[20:20 + 3 * 100])          # untouched
        self.assertEqual(sorted(merged.ids()), [1, 2, 700000])
        self.assertEqual(merged.by_id()[700000][5], "RetailIcon_1")
        # the pool only grew: every old offset still means what it did
        self.assertEqual(out[20 + 3 * 100:20 + 3 * 100 + 1], bytes([0]))

    def test_item_dbc_row_and_sheath(self):
        row = client_dbc.item_dbc_row({"entry": 1002000, "class": 4, "subclass": 4, "SoundOverrideSubclass": -1,
                                       "Material": 6, "displayid": 700001, "InventoryType": 6})
        self.assertEqual(row, [1002000, 4, 4, -1, 6, 700001, 6, 0])
        self.assertEqual(client_dbc.sheath_for(2, 10, 17), 2)     # staff
        self.assertEqual(client_dbc.sheath_for(2, 8, 17), 1)      # two-hand
        self.assertEqual(client_dbc.sheath_for(2, 7, 13), 3)      # one-hand
        self.assertEqual(client_dbc.sheath_for(4, 6, 14), 4)      # shield
        item = dbc_io.Dbc(dbc_io.schema_for("Item"))
        item.upsert(row)
        self.assertEqual(len(item.to_bytes()), 20 + 32 + 1)       # header + one 32-byte record + empty string pool

    def test_archive_order_matches_the_client_loader(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("patch-Z.MPQ", "Patch-R.MPQ", "patch-R-1.mpq", "common.MPQ", "patch.MPQ", "readme.txt",
                         "patch-" + "Z" * 30 + "RetailItems.mpq"):
                Path(tmp, name).write_bytes(b"")
            order = [p.name for p in client_dbc.archive_order(Path(tmp))]
            self.assertEqual(order[0], "patch-" + "Z" * 30 + "RetailItems.mpq")
            self.assertEqual(order[1:4], ["patch-Z.MPQ", "Patch-R.MPQ", "patch-R-1.mpq"][:3])
            self.assertEqual(order[-2:], ["patch.MPQ", "common.MPQ"])

class TypedTextureTests(unittest.TestCase):
    """Armor models name typed texture layers and per-section materials; the sync must keep both."""

    DISPLAY = {"id": 721459, "flags": 0, "model_file_ids": [6756137, 6756057], "texture_file_ids": [7386429, 7493521],
               "material_file_ids": [7386212],
               "section_textures": [{"section": 5, "file_data_id": 7386212, "material_id": 1}],
               "model_textures": [
                   [{"file_data_id": 7386429, "texture_type": -1}, {"file_data_id": 7386429, "texture_type": 2},
                    {"file_data_id": 7386432, "texture_type": 3}],
                   [{"file_data_id": 7493521, "texture_type": 2}, {"file_data_id": 7493522, "texture_type": 3}]]}

    def test_layers_are_keyed_by_texture_type(self):
        self.assertEqual(core.model_texture_layers(self.DISPLAY, 0), {2: 7386429, 3: 7386432})
        self.assertEqual(core.model_texture_layers(self.DISPLAY, 1), {2: 7493521, 3: 7493522})

    def test_untyped_row_is_the_object_skin_only_when_no_type_two_row_exists(self):
        display = {"model_textures": [[{"file_data_id": 5, "texture_type": -1}], []]}
        self.assertEqual(core.model_texture_layers(display, 0), {2: 5})

    def test_section_textures_land_in_their_own_section(self):
        belt = {"section_textures": [{"section": 5, "file_data_id": 7658895}, {"section": 4, "file_data_id": 7658896},
                                     {"section": 5, "file_data_id": 9}]}
        self.assertEqual(core.section_textures(belt), [0, 0, 0, 0, 7658896, 7658895, 0, 0])

    def test_shoulders_take_the_left_model_in_slot_zero_and_the_right_in_slot_one(self):
        pauldron = {"model_file_ids": [7321534, 7321534],
                    "models": [[{"file_data_id": 7321534, "position": 0}, {"file_data_id": 7321535, "position": 1}],
                               [{"file_data_id": 7321534, "position": 0}, {"file_data_id": 7321535, "position": 1}]]}
        self.assertEqual((core.model_for_slot(pauldron, 0), core.model_for_slot(pauldron, 1)), (7321534, 7321535))
        helm = {"model_file_ids": [7321495, 0], "models": [[{"file_data_id": 7321495, "position": -1}], []]}
        self.assertEqual((core.model_for_slot(helm, 0), core.model_for_slot(helm, 1)), (7321495, 0))
        self.assertEqual(core.display_row(pauldron, 700000)[1:3], ["retail/7321534.m2", "retail/7321535.m2"])

    def test_class_restriction_is_dropped_unless_kept(self):
        item = {"item_id": 5, "name": "x", "allowable_class": 64}
        self.assertEqual(core.item_row(item, 1000000, 700000, None)["AllowableClass"], -1)
        self.assertEqual(core.item_row(item, 1000000, 700000, None, keep_class_restriction=True)["AllowableClass"], 64)

    def test_display_row_and_sidecar(self):
        row = core.display_row(self.DISPLAY, 700000)
        self.assertEqual(row[3:5], ["retail/7386429.blp", "retail/7493521.blp"])   # ModelTexture = the object skin
        self.assertEqual(row[15 + 5], "retail/7386212.blp")                           # Texture[5]
        self.assertEqual(core.armor_texture_rows(self.DISPLAY, 700000),
                         ["700000,0,3,retail/7386432.blp", "700000,1,3,retail/7493522.blp"])



class MidnightSetTests(unittest.TestCase):
    @staticmethod
    def piece(entry, name, inv, display, sub=1):
        return {"entry": entry, "name": name, "inv": inv, "display": display, "sub": sub, "level": 90, "retail": 250000}

    def test_sets_group_by_name_and_drop_duplicate_appearances(self):
        import midnight_sets
        items = [self.piece(1, "Vale Warden's Hood", 1, 10), self.piece(2, "Vale Warden's Cap", 1, 10),
                 self.piece(3, "Vale Warden's Robe", 20, 11), self.piece(4, "Vale Warden's Belt", 6, 12)]
        sets = midnight_sets.build_sets(items)
        self.assertEqual([name for name, _ in sets], ["Vale Warden"])
        self.assertEqual([p["entry"] for p in sets[0][1]], [1, 3, 4])   # head, chest, waist; the Cap shares the Hood's look

    def test_repeated_slots_start_a_new_outfit(self):
        import midnight_sets
        items = []
        for outfit in range(3):
            items += [self.piece(100 + outfit * 2, "Rival's Helm", 1, 20 + outfit), self.piece(101 + outfit * 2, "Rival's Belt", 6, 30 + outfit)]
        sets = midnight_sets.build_sets(items)
        self.assertEqual(len(sets), 3)
        self.assertTrue(all(len(pieces) == 2 for _, pieces in sets))

    def test_item_set_schema_matches_the_stock_table(self):
        self.assertEqual(len(dbc_io.SCHEMAS["ItemSet"]), 53)


if __name__ == "__main__":
    unittest.main()
