#!/usr/bin/env python3
"""Build size-capped patch-R*.mpq archives from a resolved Retail item manifest.

  patch_builder.py build --manifest resolved_manifest.json --stage <ServerSync stage dir> \\
      --cache "<client>/Data/Retail/Cache" --out build/patch-R --stormlib "<client>/StormLib.dll"

Staging layout (identical inside the archives):
  DBFilesClient\\                       ServerSync's ItemDisplayInfo.dbc<tier>-<project> continuation(s)
  EsteriaRetail\\<product>-<build>\\<locale>\\<FileDataID>.bin   validated CDN originals (no live CDN needed)
  Interface\\Icons\\RetailIcon_<id>.blp  item icons
  Custom\\                              resolved_manifest.json, sync_map.json, RetailExtras.txt
  EsteriaRetail\\<product>-<build>\\<locale>\\tables\\<FileDataID>.bin   the DB2 tables named in the manifest `tables`
                                      (the client's armor index, with no CDN access needed)

Archive naming: patch-<L>.mpq holds DBFilesClient, Custom and as many icons as fit (the highest-priority
patch); everything else (remaining icons, the bulk EsteriaRetail files) goes to patch-<L>-1.mpq,
patch-<L>-2.mpq ... Every archive stays under --max-archive-mb (default 1800, below StormLib's 2 GB limit),
so a catalogue-sized pull of tens of GB becomes ~dozens of volumes automatically.
Contents and layout are deterministic (sorted names, greedy split); MPQ bytes themselves carry StormLib
timestamps, so incremental rebuilds compare per-archive content digests, not file hashes alone.
Staging verifies every original (FNV-1a checksum) in parallel worker processes; --verify header skips the
per-byte checksum when CdnCacheTrim (--audit) already vouched for the cache.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

MAGIC = 0x31434457  # "WDC1" wrapper written by Engine/Source/Retail/DecodedCache.cpp
FNV_BASIS, FNV_PRIME, MASK = 14695981039346656037, 1099511628211, (1 << 64) - 1
MAX_ASSET = 128 * 1024 * 1024
DEFAULT_MAX_ARCHIVE = 1800 * 1024 * 1024
# Sorts after every existing patch-*.MPQ in the Esteria client (the longest Z run there is 25), so its tables win.
DEFAULT_DBC_ARCHIVE = "patch-" + "Z" * 30 + "RetailItems.mpq"
HARD_ARCHIVE_LIMIT = 1990 * 1024 * 1024  # StormLib / the 3.3.5 client cannot open an MPQ of 2 GB or more
EMBED_FULL_LIMIT = 20000                 # embed the full manifest / file list in the archive up to this many files
SCOPE = re.compile(r"^[a-z_]+-[a-f0-9]{32}$")


class BuildError(ValueError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fnv1a64(data: bytes) -> int:
    value = FNV_BASIS
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & MASK
    return value


def unwrap_original(path: Path, verify: bool = True) -> bytes:
    """Decoded-cache entries are `<magic u32><size u32><fnv1a64 u64><payload>`; verify then strip.
    `verify=False` still checks magic and length, but skips the (slow, pure Python) per-byte checksum."""
    raw = Path(path).read_bytes()
    if len(raw) < 16:
        raise BuildError(f"Truncated cache entry: {path}")
    magic, size, checksum = struct.unpack_from("<IIQ", raw)
    payload = raw[16:]
    if magic != MAGIC or size != len(payload) or size > MAX_ASSET:
        raise BuildError(f"Invalid cache entry header: {path}")
    if verify and fnv1a64(payload) != checksum:
        raise BuildError(f"Cache entry checksum mismatch: {path}")
    return payload


def index_cache(cache: Path, product: str, build: str, locale: str, policy: str = "strict") -> dict[int, Path]:
    """FileDataID -> decoded entry, for the requested product-build and locale only.
    policy "strict" = original files; "tables" = leniently decoded DB2 tables (`tables` or `tables-<keyring>`)."""
    scope_name = f"{product}-{build}"
    found: dict[int, Path] = {}
    for path in sorted(Path(cache).rglob("id-*.bin")):
        parts = path.relative_to(cache).parts
        if len(parts) < 3 or parts[-3] != locale:
            continue
        if not (parts[-2] == "strict" if policy == "strict" else parts[-2].startswith("tables")):
            continue
        scope = next((p for p in parts if SCOPE.match(p)), None)
        if scope != scope_name:
            continue
        stem = path.stem.removeprefix("id-")
        if stem.isdecimal():
            found.setdefault(int(stem), path)
    return found


def audit_cache(audit: Path | None, cache: Path) -> None:
    """Run CdnCacheTrim (read-only integrity pass) before anything is packed."""
    if audit is None:
        return
    result = subprocess.run([str(audit), str(cache)], capture_output=True, text=True)
    if result.returncode:
        raise BuildError(f"Cache audit failed:\n{result.stdout}{result.stderr}")


class Entry:
    __slots__ = ("name", "source", "sha256", "bytes", "kind", "file_data_id")

    def __init__(self, name, source, sha256, size, kind, file_data_id=0):
        self.name, self.source, self.sha256, self.bytes, self.kind, self.file_data_id = name, source, sha256, size, kind, file_data_id


def stage_file(job) -> tuple[int, str]:
    """Worker (runs in a pool process): verify one original and write it under the work tree.
    Returns (payload size, sha256)."""
    source, targets, verify = job
    payload = unwrap_original(Path(source), verify)
    for target in targets:
        Path(target).parent.mkdir(parents=True, exist_ok=True)
        Path(target).write_bytes(payload)
    return len(payload), sha256_bytes(payload)


def run_jobs(jobs_list: list, workers: int = 0) -> list[tuple[int, str]]:
    """Map stage_file over `jobs_list` in order. Small jobs and workers == 1 run inline; the FNV-1a check is
    pure Python (~10 MB/s per core), so a multi-GB catalogue uses a process pool."""
    if workers <= 0:
        workers = min(8, os.cpu_count() or 1)
    if workers == 1 or len(jobs_list) < 64:
        return [stage_file(job) for job in jobs_list]
    from concurrent.futures import ProcessPoolExecutor
    with ProcessPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(stage_file, jobs_list, chunksize=16))


def stage(manifest: dict, sync_map: dict | None, stage_dir: Path | None, cache: Path, work: Path, locale: str,
          icons_dir: Path | None = None, extra: list[Path] | None = None, verify: bool = True,
          workers: int = 0) -> tuple[list[Entry], list[str]]:
    """Materialise staged files under `work` and return entries sorted by archive path."""
    product, build = manifest["product"], manifest["build"]
    scope = f"{product}-{build}"
    originals = index_cache(cache, product, build, locale)
    tables = index_cache(cache, product, build, locale, "tables") if manifest.get("tables") else {}
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    entries: dict[str, Entry] = {}
    lowered: set[str] = set()
    problems: list[str] = []
    queued: list[tuple[str, str, int, Path, Path]] = []  # key, kind, file id, cache source, staged target

    def claim(key: str) -> None:
        if key.lower() in lowered:
            raise BuildError(f"Duplicate archive path: {key}")
        lowered.add(key.lower())

    def queue(key: str, kind: str, file_data_id: int, source: Path) -> None:
        claim(key)
        queued.append((key, kind, file_data_id, source, work / key.replace("\\", "/")))

    def add(name: str, data: bytes | None, source: Path | None, kind: str, file_data_id: int = 0):
        key = name.replace("/", "\\")
        claim(key)
        target = work / key.replace("\\", "/")
        target.parent.mkdir(parents=True, exist_ok=True)
        if data is not None:
            target.write_bytes(data)
            digest, size = sha256_bytes(data), len(data)
        else:
            shutil.copyfile(source, target)
            digest, size = sha256_file(target), target.stat().st_size
        entries[key] = Entry(key, target, digest, size, kind, file_data_id)

    kinds = {f["file_data_id"]: f["kind"] for f in manifest.get("files", [])}
    for file_id in sorted(kinds):
        source = originals.get(file_id)
        if source is None:
            problems.append(f"{kinds[file_id]}:{file_id}: not in the decoded cache for {scope}/{locale}")
        elif kinds[file_id] == "icon":
            # Icons are stored once, where the DBC points (Interface\Icons), not duplicated under EsteriaRetail.
            queue(f"Interface\\Icons\\RetailIcon_{file_id}.blp", "icon-ui", file_id, source)
        else:
            queue(f"EsteriaRetail\\{scope}\\{locale}\\{file_id}.bin", kinds[file_id], file_id, source)
    # DB2 tables the client's armor index reads. Packing them lets it start with no CDN access at all.
    for table in sorted(manifest.get("tables", []), key=lambda t: t["file_data_id"]):
        source = tables.get(table["file_data_id"])
        if source is None:
            problems.append(f"table:{table.get('name', '?')}:{table['file_data_id']}: not in the decoded cache for {scope}/{locale}")
        else:
            queue(f"EsteriaRetail\\{scope}\\{locale}\\tables\\{table['file_data_id']}.bin", "table", table["file_data_id"], source)

    results = run_jobs([(str(src), [str(target)], verify) for _, _, _, src, target in queued], workers)
    for (key, kind, file_data_id, _, target), (size, digest) in zip(queued, results):
        entries[key] = Entry(key, target, digest, size, kind, file_data_id)

    if icons_dir:
        for icon in sorted(Path(icons_dir).glob("RetailIcon_*.blp")):
            name = f"Interface\\Icons\\{icon.name}"
            if name.lower() not in lowered:
                add(name, None, icon, "icon-ui")
    if stage_dir:
        db_dir = Path(stage_dir) / "DBFilesClient"
        if db_dir.is_dir():
            for dbc in sorted(db_dir.iterdir()):
                # mod-wxl-dbc continuations (<Table>.dbc<tier>-<project>) are read by the server only; the 3.3.5a client
                # ignores them, so shipping one gives a false sense of coverage. Only complete tables go in.
                if dbc.is_file() and not re.search(r"\.dbc\d+-", dbc.name, re.IGNORECASE):
                    add(f"DBFilesClient\\{dbc.name}", None, dbc, "dbc")
        # Typed texture layers of the armor models (type 3 and up), read by the client next to ItemDisplayInfo.
        armor_csv = Path(stage_dir) / "Esteria" / "RetailArmorTextures.csv"
        if armor_csv.is_file():
            add("Esteria\\RetailArmorTextures.csv", None, armor_csv, "armor-textures")
    for root in extra or []:
        for path in sorted(p for p in Path(root).rglob("*") if p.is_file()):
            add(str(path.relative_to(root)).replace("/", "\\"), None, path, "extra")

    # Metadata. A catalogue manifest has hundreds of thousands of files (>100 MB of JSON): past
    # EMBED_FULL_LIMIT only a digest summary is embedded; the full manifest stays with the build output.
    files = manifest.get("files", [])
    lines = [f"{f['file_data_id']};{f['kind']};{f['sha256']}" for f in sorted(files, key=lambda f: f["file_data_id"])]
    if len(files) <= EMBED_FULL_LIMIT:
        add("Custom\\resolved_manifest.json", json.dumps(manifest, indent=2, sort_keys=True).encode() + b"\n", None, "metadata")
        listing = lines
    else:
        counts: dict[str, int] = {}
        for f in files:
            counts[f["kind"]] = counts.get(f["kind"], 0) + 1
        summary = {"schema": 1, "product": product, "build": build, "region": manifest.get("region"), "locale": locale,
                   "items": len(manifest.get("items", [])), "displays": len(manifest.get("displays", [])),
                   "files": len(files), "files_by_kind": counts, "full": False,
                   "manifest_sha256": sha256_bytes(json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode())}
        add("Custom\\resolved_manifest.json", json.dumps(summary, indent=2, sort_keys=True).encode() + b"\n", None, "metadata")
        listing = [f"# {len(files)} files; list sha256 {sha256_bytes(chr(10).join(lines).encode())} (full list not embedded)"]
    if sync_map:
        if len(sync_map.get("items", [])) <= EMBED_FULL_LIMIT:
            add("Custom\\sync_map.json", json.dumps(sync_map, indent=2, sort_keys=True).encode() + b"\n", None, "metadata")
        else:
            summary = {k: v for k, v in sync_map.items() if k != "items"}
            summary.update(items=len(sync_map["items"]), full=False, items_sha256=sha256_bytes(
                json.dumps(sync_map["items"], sort_keys=True, separators=(",", ":")).encode()))
            add("Custom\\sync_map.json", json.dumps(summary, indent=2, sort_keys=True).encode() + b"\n", None, "metadata")
    add("Custom\\RetailExtras.txt", ("\n".join(["# FileDataID;kind;sha256 of the CDN original", f"# {scope} locale {locale}", *listing]) + "\n").encode(), None, "metadata")
    return sorted(entries.values(), key=lambda e: e.name.lower()), problems


def plan_archives(entries: list[Entry], letter: str, max_bytes: int, priority_name: str | None = None,
                  numbered: bool = False) -> list[tuple[str, list[Entry]]]:
    """Every archive stays under `max_bytes` (a single original is at most 128 MB, so none can exceed StormLib's limit).
    patch-<L>.mpq takes DBFilesClient, Custom and extras (always) plus icons while they fit; the remaining icons and
    the EsteriaRetail bulk fill patch-<L>-1, -2, ... greedily in name order.

    `priority_name`: the 3.3.5a client takes each DBFilesClient table from the highest-sorting archive, so complete tables
    (ItemDisplayInfo.dbc, Item.dbc) must be in an archive that sorts after every other copy: they go alone into this archive.
    `numbered`: name every volume patch-<L>-1, -2, ... (no bare patch-<L>.mpq, which collides with an existing Patch-<L>.MPQ
    on a case-insensitive file system)."""
    def is_icon(e): return e.name.startswith("Interface\\Icons\\")
    def is_bulk(e): return e.name.startswith("EsteriaRetail\\")
    def is_table(e): return e.name.lower().startswith("dbfilesclient\\")
    priority = [e for e in entries if is_table(e)] if priority_name else []
    if priority_name:
        entries = [e for e in entries if not is_table(e)]
    head = [e for e in entries if not is_icon(e) and not is_bulk(e)]
    head_bytes = sum(e.bytes for e in head)
    if head_bytes > HARD_ARCHIVE_LIMIT:
        raise BuildError(f"DBFilesClient/Custom/extras alone are {head_bytes} bytes, over the MPQ limit")
    first, total, overflow = list(head), head_bytes, []
    for entry in (e for e in entries if is_icon(e)):
        if not overflow and total + entry.bytes <= max_bytes:
            first.append(entry)
            total += entry.bytes
        else:
            overflow.append(entry)
    first.sort(key=lambda e: e.name.lower())
    plan = [(f"patch-{letter}-1.mpq" if numbered else f"patch-{letter}.mpq", first)]
    group, total, serial = [], 0, (1 if numbered else 0)
    for entry in overflow + [e for e in entries if is_bulk(e)]:
        if entry.bytes > MAX_ASSET:
            raise BuildError(f"Oversized original: {entry.name}")
        if group and total + entry.bytes > max_bytes:
            serial += 1
            plan.append((f"patch-{letter}-{serial}.mpq", group))
            group, total = [], 0
        group.append(entry)
        total += entry.bytes
    if group:
        plan.append((f"patch-{letter}-{serial + 1}.mpq", group))
    if priority:
        plan.append((priority_name, sorted(priority, key=lambda e: e.name.lower())))
    return plan


def content_digest(entries: list[Entry]) -> str:
    return sha256_bytes("\n".join(f"{e.name}\t{e.sha256}" for e in entries).encode())


class StormArchiver:
    def __init__(self, dll: Path):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "MountStudio"))
        from worker import Storm  # noqa: E402
        self.storm = Storm(dll)

    def pack(self, path: Path, entries: list[Entry]) -> None:
        self.storm.pack(path, [(e.name, e.source, e.sha256) for e in entries])


class ListingArchiver:
    """Test/dry-run archiver: writes a deterministic listing instead of an MPQ."""

    def pack(self, path: Path, entries: list[Entry]) -> None:
        path.write_text("\n".join(f"{e.name}\t{e.sha256}" for e in entries) + "\n", encoding="utf-8")


def build(manifest_path: Path, out: Path, cache: Path, archiver, stage_dir: Path | None = None, letter: str = "R",
          locale: str = "2", max_archive: int = DEFAULT_MAX_ARCHIVE, audit: Path | None = None,
          icons_dir: Path | None = None, extra: list[Path] | None = None, force: bool = False,
          allow_missing: bool = False, verify: bool = True, workers: int = 0, keep_stage: bool = True,
          priority_name: str | None = None, numbered: bool = False) -> dict:
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    if manifest.get("schema") != 1:
        raise BuildError("Unsupported resolved_manifest.json schema")
    if not re.fullmatch(r"[A-Za-z0-9]", letter):
        raise BuildError("--patch-letter must be a single letter or digit")
    sync_map = None
    if stage_dir and (Path(stage_dir) / "sync_map.json").exists():
        sync_map = json.loads((Path(stage_dir) / "sync_map.json").read_text(encoding="utf-8"))
    audit_cache(audit, cache)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    entries, problems = stage(manifest, sync_map, stage_dir, Path(cache), out / "stage", locale, icons_dir, extra,
                              verify, workers)
    if problems and not allow_missing:
        raise BuildError("Cannot build; missing originals:\n  " + "\n  ".join(problems))
    plan = plan_archives(entries, letter, max_archive, priority_name, numbered)
    previous = {}
    if (out / "manifest.json").exists() and not force:
        previous = {a["name"]: a for a in json.loads((out / "manifest.json").read_text(encoding="utf-8")).get("archives", [])}
    archives, built, reused = [], [], []
    for name, group in plan:
        digest = content_digest(group)
        target = out / name
        old = previous.get(name)
        if old and old.get("content_digest") == digest and target.exists() and sha256_file(target) == old["sha256"]:
            reused.append(name)
            record = dict(old)
        else:
            if target.exists():
                target.unlink()
            archiver.pack(target, group)
            built.append(name)
            record = {"name": name, "sha256": sha256_file(target), "bytes": target.stat().st_size, "entries": len(group),
                      "content_digest": digest}
        archives.append(record)
    for stale in out.glob(f"patch-{letter}*.mpq"):
        if stale.name not in {a["name"] for a in archives}:
            stale.unlink()
    where = {e.name: name for name, group in plan for e in group}
    items = []
    if sync_map:
        items = [{"item_id": i["item_id"], "item_template_entry": i["item_template_entry"],
                  "retail_display_info_id": i.get("retail_display_info_id"),
                  "target_item_display_info_id": i["target_item_display_info_id"], "file_data_ids": i.get("file_data_ids", [])}
                 for i in sync_map.get("items", [])]
    result = {
        "schema": 1,
        "cdn": {"product": manifest["product"], "build": manifest["build"], "region": manifest.get("region"), "locale": locale},
        "archives": archives,
        "items": items,
        "entries": [{"name": e.name, "archive": where[e.name], "kind": e.kind, "file_data_id": e.file_data_id,
                     "sha256": e.sha256, "bytes": e.bytes} for e in entries],
        "missing": problems,
    }
    # A catalogue-sized build record is large: stop pretty-printing it past the embed limit.
    pretty = len(entries) <= EMBED_FULL_LIMIT
    (out / "manifest.json").write_text(json.dumps(result, indent=2 if pretty else None, sort_keys=True,
                                                  separators=None if pretty else (",", ":")) + "\n", encoding="utf-8")
    (out / "patch-checksums.txt").write_text("".join(f"{a['sha256']} *{a['name']}\n" for a in archives), encoding="utf-8")
    result["_built"], result["_reused"] = built, reused
    if not keep_stage:
        shutil.rmtree(out / "stage", ignore_errors=True)  # a full catalogue stages tens of GB twice over
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build")
    b.add_argument("--manifest", type=Path, required=True, help="resolved_manifest.json from RetailAssetProbe")
    b.add_argument("--stage", type=Path, help="ServerSync stage dir (DBFilesClient + sync_map.json)")
    b.add_argument("--cache", type=Path, required=True, help="decoded cache root (e.g. <client>/Data/Retail/Cache)")
    b.add_argument("--out", type=Path, required=True)
    b.add_argument("--stormlib", type=Path, help="StormLib.dll; required unless --dry-run")
    b.add_argument("--audit", type=Path, help="CdnCacheTrim.exe integrity checker (recommended)")
    b.add_argument("--patch-letter", default="R")
    b.add_argument("--locale-id", default="2", help="decoded-cache locale folder (2 = enUS mask)")
    b.add_argument("--max-archive-mb", type=int, default=DEFAULT_MAX_ARCHIVE // (1024 * 1024))
    b.add_argument("--icons-dir", type=Path)
    b.add_argument("--extra", type=Path, action="append", help="extra directory merged into the archives (e.g. EsteriaShell output)")
    b.add_argument("--force", action="store_true", help="rebuild even if archives are up to date")
    b.add_argument("--verify", choices=("full", "header"), default="full",
                   help="full: FNV-1a check of every original (slow, parallel); header: magic and length only")
    b.add_argument("--jobs", type=int, default=0, help="staging worker processes (default: min(8, CPUs))")
    b.add_argument("--no-keep-stage", action="store_true", help="delete <out>/stage after packing to save disk")
    b.add_argument("--allow-missing", action="store_true")
    b.add_argument("--dbc-archive", default=DEFAULT_DBC_ARCHIVE,
                   help="archive that receives the complete DBFilesClient tables; the client takes each table from the "
                        "highest-sorting archive, so the name must sort after every other patch (default %(default)s)")
    b.add_argument("--no-dbc-archive", action="store_true", help="keep DBFilesClient in patch-<L>.mpq (it then loses to later patches)")
    b.add_argument("--number-all", action="store_true",
                   help="name volumes patch-<L>-1, -2, ... (no bare patch-<L>.mpq; avoids clashing with an existing Patch-<L>.MPQ)")
    b.add_argument("--dry-run", action="store_true", help="write listings instead of MPQs")
    args = parser.parse_args(argv)
    try:
        if args.dry_run:
            archiver = ListingArchiver()
        elif args.stormlib:
            archiver = StormArchiver(args.stormlib)
        else:
            raise BuildError("--stormlib is required (or use --dry-run)")
        result = build(args.manifest, args.out, args.cache, archiver, args.stage, args.patch_letter, args.locale_id,
                       args.max_archive_mb * 1024 * 1024, args.audit, args.icons_dir, args.extra, args.force, args.allow_missing,
                       verify=args.verify == "full", workers=args.jobs, keep_stage=not args.no_keep_stage,
                       priority_name=None if args.no_dbc_archive else args.dbc_archive, numbered=args.number_all)
    except BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for archive in result["archives"]:
        state = "built" if archive["name"] in result["_built"] else "up to date"
        print(f"{archive['name']}: {archive['entries']} entries, {archive['bytes']} bytes ({state})")
    if result["missing"]:
        print(f"warning: {len(result['missing'])} originals missing", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
