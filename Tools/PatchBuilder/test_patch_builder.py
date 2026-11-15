import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import patch_builder as pb  # noqa: E402

BUILD = "ab" * 16


def wrapped(payload: bytes) -> bytes:
    return struct.pack("<IIQ", pb.MAGIC, len(payload), pb.fnv1a64(payload)) + payload


class Fixture:
    def __init__(self, root: Path, sizes=None):
        self.root = root
        sizes = sizes or {1: 10, 2: 10, 3: 10}
        self.cache = root / "Cache"
        decoded = self.cache / "Retail" / f"wow-{BUILD}" / "decoded" / "2" / "strict"
        decoded.mkdir(parents=True)
        self.payloads = {}
        for file_id, size in sizes.items():
            payload = bytes([file_id % 251]) * size
            self.payloads[file_id] = payload
            (decoded / f"id-{file_id}.bin").write_bytes(wrapped(payload))
        self.stage = root / "stage"
        (self.stage / "DBFilesClient").mkdir(parents=True)
        (self.stage / "DBFilesClient" / "ItemDisplayInfo.dbc").write_bytes(b"WDBC" + bytes(16))
        (self.stage / "sync_map.json").write_text(json.dumps({"schema": 1, "items": [
            {"item_id": 100, "item_template_entry": 1000000, "retail_display_info_id": 9,
             "target_item_display_info_id": 700001, "file_data_ids": [1, 2]}]}))
        files = [{"file_data_id": i, "kind": "icon" if i == 3 else "model", "sha256": pb.sha256_bytes(p), "bytes": len(p)}
                 for i, p in self.payloads.items()]
        self.manifest = {"schema": 1, "product": "wow", "region": "us", "build": BUILD, "locale": 2, "files": files,
                         "items": [], "displays": []}
        self.manifest_path = root / "resolved_manifest.json"
        self.manifest_path.write_text(json.dumps(self.manifest))

    def build(self, out=None, **kwargs):
        return pb.build(self.manifest_path, out or self.root / "out", self.cache, pb.ListingArchiver(), self.stage, **kwargs)


class UnwrapTests(unittest.TestCase):
    def test_unwrap_validates(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp, "good.bin"); good.write_bytes(wrapped(b"hello"))
            self.assertEqual(pb.unwrap_original(good), b"hello")
            bad = Path(tmp, "bad.bin"); bad.write_bytes(wrapped(b"hello")[:-1] + b"X")
            with self.assertRaises(pb.BuildError):
                pb.unwrap_original(bad)
            short = Path(tmp, "short.bin"); short.write_bytes(b"123")
            with self.assertRaises(pb.BuildError):
                pb.unwrap_original(short)
            empty = Path(tmp, "empty.bin"); empty.write_bytes(wrapped(b""))
            self.assertEqual(pb.unwrap_original(empty), b"")

    def test_fnv_known_vector(self):
        self.assertEqual(pb.fnv1a64(b""), 14695981039346656037)
        self.assertEqual(pb.fnv1a64(b"a"), 0xaf63dc4c8601ec8c)


class BuildTests(unittest.TestCase):
    def test_layout_manifest_and_checksums(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            result = fx.build()
            out = Path(tmp, "out")
            names = {e["name"] for e in result["entries"]}
            self.assertIn(f"EsteriaRetail\\wow-{BUILD}\\2\\1.bin", names)
            self.assertIn("DBFilesClient\\ItemDisplayInfo.dbc", names)
            self.assertIn("Interface\\Icons\\RetailIcon_3.blp", names)
            self.assertIn("Custom\\RetailExtras.txt", names)
            self.assertIn("Custom\\sync_map.json", names)
            self.assertEqual([a["name"] for a in result["archives"]], ["patch-R.mpq", "patch-R-1.mpq"])
            manifest = json.loads((out / "manifest.json").read_text())
            self.assertEqual(manifest["cdn"], {"product": "wow", "build": BUILD, "region": "us", "locale": "2"})
            self.assertEqual(manifest["items"][0]["item_template_entry"], 1000000)
            self.assertEqual(manifest["items"][0]["target_item_display_info_id"], 700001)
            by_name = {e["name"]: e for e in manifest["entries"]}
            self.assertEqual(by_name[f"EsteriaRetail\\wow-{BUILD}\\2\\2.bin"]["sha256"], pb.sha256_bytes(fx.payloads[2]))
            checks = (out / "patch-checksums.txt").read_text().splitlines()
            self.assertEqual(len(checks), 2)
            for line in checks:
                digest, name = line.split(" *")
                self.assertEqual(digest, pb.sha256_file(out / name))
            extras = (out / "stage" / "Custom" / "RetailExtras.txt").read_text()
            self.assertIn(f"1;model;{pb.sha256_bytes(fx.payloads[1])}", extras)

    def test_continuation_fragments_are_never_staged_and_tables_win(self):
        # The client ignores <Table>.dbcN-<project>; complete tables go alone into the top-priority archive.
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            (fx.stage / "DBFilesClient" / "Item.dbc").write_bytes(b"WDBC" + bytes(16))
            (fx.stage / "DBFilesClient" / "ItemDisplayInfo.dbc1-retail-armor").write_bytes(b"WDBC" + bytes(16))
            priority = pb.DEFAULT_DBC_ARCHIVE
            result = fx.build(priority_name=priority, numbered=True)
            names = {e["name"]: e["archive"] for e in result["entries"]}
            self.assertNotIn("DBFilesClient\\ItemDisplayInfo.dbc1-retail-armor", names)
            self.assertEqual(names["DBFilesClient\\ItemDisplayInfo.dbc"], priority)
            self.assertEqual(names["DBFilesClient\\Item.dbc"], priority)
            archives = [a["name"] for a in result["archives"]]
            self.assertEqual(archives[0], "patch-R-1.mpq")
            self.assertNotIn("patch-R.mpq", archives)
            self.assertEqual(archives[-1], priority)
            others = {a for a in archives if a != priority}
            self.assertTrue(all(priority.lower() > o.lower() for o in others))
            # nothing but the tables lives in the priority archive
            self.assertEqual({n for n, a in names.items() if a == priority},
                             {"DBFilesClient\\ItemDisplayInfo.dbc", "DBFilesClient\\Item.dbc"})
            # and it sorts after every real patch name in the Esteria client
            for real in ("patch-Z.MPQ", "Patch-ZZZZZZZZZZZZZZZZZZZZZZZZZHousingDev.MPQ", "patch-ZzzzzzzzzzzEsteriaShell.MPQ"):
                self.assertGreater(priority.lower(), real.lower())

    def test_split_is_greedy_and_named(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp), {1: 60, 2: 60, 3: 60, 4: 60})
            result = fx.build(letter="Q", max_archive=100)
            names = [a["name"] for a in result["archives"]]
            self.assertEqual(names, ["patch-Q.mpq", "patch-Q-1.mpq", "patch-Q-2.mpq", "patch-Q-3.mpq", "patch-Q-4.mpq"])
            self.assertTrue(all(a["entries"] == 1 for a in result["archives"][1:]))

    def test_deterministic_and_incremental(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            first = fx.build()
            self.assertEqual(first["_built"], ["patch-R.mpq", "patch-R-1.mpq"])
            digest = [a["content_digest"] for a in first["archives"]]
            second = fx.build()
            self.assertEqual(second["_built"], [])
            self.assertEqual(second["_reused"], ["patch-R.mpq", "patch-R-1.mpq"])
            self.assertEqual(digest, [a["content_digest"] for a in second["archives"]])
            # a changed original rebuilds only the archive holding it
            decoded = fx.cache / "Retail" / f"wow-{BUILD}" / "decoded" / "2" / "strict" / "id-2.bin"
            decoded.write_bytes(wrapped(b"changed!!!"))
            fx.manifest["files"][1]["sha256"] = pb.sha256_bytes(b"changed!!!")
            fx.manifest_path.write_text(json.dumps(fx.manifest))
            third = fx.build()
            self.assertEqual(sorted(third["_built"]), ["patch-R-1.mpq", "patch-R.mpq"])  # RetailExtras.txt lists the hash too
            self.assertEqual(fx.build(force=True)["_reused"], [])

    def test_untouched_archive_is_reused(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp), {1: 60, 2: 60})
            fx.build(max_archive=100)
            (fx.stage / "DBFilesClient" / "ItemDisplayInfo.dbc").write_bytes(b"WDBC" + bytes(20))
            again = fx.build(max_archive=100)
            self.assertEqual(again["_built"], ["patch-R.mpq"])
            self.assertEqual(sorted(again["_reused"]), ["patch-R-1.mpq", "patch-R-2.mpq"])

    def test_missing_original_and_bad_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            fx.manifest["files"].append({"file_data_id": 99, "kind": "texture", "sha256": "0" * 64, "bytes": 1})
            fx.manifest_path.write_text(json.dumps(fx.manifest))
            with self.assertRaises(pb.BuildError):
                fx.build()
            result = fx.build(allow_missing=True)
            self.assertEqual(len(result["missing"]), 1)
            with self.assertRaises(pb.BuildError):
                fx.build(letter="RR")
            fx.manifest["schema"] = 2
            fx.manifest_path.write_text(json.dumps(fx.manifest))
            with self.assertRaises(pb.BuildError):
                fx.build()

    def test_wrong_build_cache_is_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            fx.manifest["build"] = "cd" * 16
            fx.manifest_path.write_text(json.dumps(fx.manifest))
            with self.assertRaises(pb.BuildError):
                fx.build()

    def test_extra_directory_merged_and_duplicates_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            extra = Path(tmp, "extra", "Interface", "AddOns")
            extra.mkdir(parents=True)
            (extra / "x.lua").write_text("x")
            result = fx.build(extra=[Path(tmp, "extra")])
            self.assertIn("Interface\\AddOns\\x.lua", {e["name"] for e in result["entries"]})
            dup = Path(tmp, "dup", "DBFilesClient")
            dup.mkdir(parents=True)
            (dup / "ItemDisplayInfo.dbc").write_text("x")
            with self.assertRaises(pb.BuildError):
                fx.build(extra=[Path(tmp, "dup")])


class ScaleTests(unittest.TestCase):
    def test_tables_are_packed_and_missing_ones_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            tables = fx.cache / "Retail" / f"wow-{BUILD}" / "decoded" / "2" / "tables"
            tables.mkdir(parents=True)
            (tables / "id-1266429.bin").write_bytes(wrapped(b"DB2-DATA"))
            fx.manifest["tables"] = [{"file_data_id": 1266429, "name": "itemdisplayinfo"}]
            fx.manifest_path.write_text(json.dumps(fx.manifest))
            result = fx.build()
            names = {e["name"]: e for e in result["entries"]}
            entry = names[f"EsteriaRetail\\wow-{BUILD}\\2\\tables\\1266429.bin"]
            self.assertEqual((entry["kind"], entry["sha256"]), ("table", pb.sha256_bytes(b"DB2-DATA")))
            fx.manifest["tables"].append({"file_data_id": 777, "name": "helmetgeosetdata"})
            fx.manifest_path.write_text(json.dumps(fx.manifest))
            with self.assertRaises(pb.BuildError):
                fx.build()
            self.assertIn("table:helmetgeosetdata:777", fx.build(allow_missing=True)["missing"][0])

    def test_icons_are_stored_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            names = [e["name"] for e in Fixture(Path(tmp)).build()["entries"]]
            self.assertIn("Interface\\Icons\\RetailIcon_3.blp", names)
            self.assertNotIn(f"EsteriaRetail\\wow-{BUILD}\\2\\3.bin", names)

    def test_small_archive_is_capped_and_icons_overflow(self):
        entries = [pb.Entry("DBFilesClient\\a.dbc", Path("a"), "0", 10, "dbc"), pb.Entry("Custom\\m.json", Path("m"), "0", 10, "metadata")]
        entries += [pb.Entry(f"Interface\\Icons\\RetailIcon_{i}.blp", Path("i"), "0", 30, "icon-ui") for i in range(10)]
        entries += [pb.Entry(f"EsteriaRetail\\wow-{BUILD}\\2\\{i}.bin", Path("b"), "0", 40, "model") for i in range(5)]
        plan = pb.plan_archives(sorted(entries, key=lambda e: e.name.lower()), "R", 100)
        self.assertEqual(plan[0][0], "patch-R.mpq")
        self.assertEqual(sum(e.bytes for e in plan[0][1]), 80)            # 2 head entries (20) + 2 icons; a third would pass 100
        for _, group in plan:
            self.assertLessEqual(sum(e.bytes for e in group), 100)
        self.assertEqual(sum(len(g) for _, g in plan), len(entries))
        self.assertEqual([n for n, _ in plan], ["patch-R.mpq"] + [f"patch-R-{i}.mpq" for i in range(1, len(plan))])
        with self.assertRaises(pb.BuildError):
            pb.plan_archives([pb.Entry("Custom\\big", Path("x"), "0", pb.HARD_ARCHIVE_LIMIT + 1, "metadata")], "R", 100)

    def test_catalogue_split_stays_under_the_mpq_limit(self):
        # 40 GB of 100 MB originals: every volume must stay under the default cap, far below 2 GB.
        entries = [pb.Entry(f"EsteriaRetail\\wow-{BUILD}\\2\\{i}.bin", Path("b"), "0", 100 * 1024 * 1024, "model") for i in range(400)]
        plan = pb.plan_archives(entries, "R", pb.DEFAULT_MAX_ARCHIVE)
        self.assertGreater(len(plan), 20)
        self.assertTrue(all(sum(e.bytes for e in g) <= pb.DEFAULT_MAX_ARCHIVE for _, g in plan))
        self.assertTrue(all(sum(e.bytes for e in g) < 2 * 1024 ** 3 for _, g in plan))

    def test_parallel_staging_matches_inline_and_detects_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            sizes = {i: 50 + i for i in range(1, 121)}
            fx = Fixture(Path(tmp), sizes)
            serial = fx.build(out=Path(tmp, "serial"), workers=1)
            parallel = fx.build(out=Path(tmp, "parallel"), workers=2)
            self.assertEqual([a["content_digest"] for a in serial["archives"]], [a["content_digest"] for a in parallel["archives"]])
            bad = fx.cache / "Retail" / f"wow-{BUILD}" / "decoded" / "2" / "strict" / "id-77.bin"
            raw = bytearray(bad.read_bytes())
            raw[-1] ^= 0xFF
            bad.write_bytes(bytes(raw))
            with self.assertRaises(pb.BuildError):
                fx.build(out=Path(tmp, "bad"), workers=2)
            with self.assertRaises(pb.BuildError):
                fx.build(out=Path(tmp, "bad2"), workers=1)
            fx.build(out=Path(tmp, "header"), workers=1, verify=False)  # header-only mode does not re-hash payloads

    def test_large_manifest_embeds_only_a_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            old = pb.EMBED_FULL_LIMIT
            pb.EMBED_FULL_LIMIT = 2
            try:
                result = fx.build()
            finally:
                pb.EMBED_FULL_LIMIT = old
            embedded = json.loads((Path(tmp, "out", "stage", "Custom", "resolved_manifest.json")).read_text())
            self.assertFalse(embedded["full"])
            self.assertEqual(embedded["files"], 3)
            self.assertEqual(len(embedded["manifest_sha256"]), 64)
            extras = (Path(tmp, "out", "stage", "Custom", "RetailExtras.txt")).read_text()
            self.assertIn("full list not embedded", extras)
            self.assertNotIn(";model;", extras)
            self.assertTrue(result["archives"])

    def test_many_entries_dedupe_is_linear(self):
        import time
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp), {i: 8 for i in range(1, 3001)})
            start = time.time()
            fx.build(workers=1, verify=False)
            self.assertLess(time.time() - start, 30.0)   # the duplicate check used to rebuild a set per file

    def test_no_keep_stage_removes_the_work_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            fx.build(keep_stage=False)
            self.assertFalse(Path(tmp, "out", "stage").exists())
            self.assertTrue(Path(tmp, "out", "manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
