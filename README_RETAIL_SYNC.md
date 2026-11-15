# Retail item sync: CDN -> server DBC/SQL -> `patch-R*.mpq`

Pulls Retail items, display data, models, textures and icons straight from Blizzard's CDN through the
client's own CASC range-download layer (`Engine/Source/Retail`). **No Retail WoW install is needed.**

```
RetailAssetProbe --resolve-items   ->  resolved_manifest.json  (+ durable CDN cache)
esteria_sync.py sync               ->  ItemDisplayInfo.dbc1-<project>, pending_db_world SQL, sync_map.json
patch_builder.py build             ->  patch-R.mpq, patch-R-1.mpq ..., manifest.json, patch-checksums.txt
```

Everything below is PowerShell, run from the `wowdecomp` checkout. `$ESTERIA` is your Esteria folder.

## 0. One-time setup

Build only the headless tools (no client, no ASECM3 regeneration):

```powershell
cmake -S . -B build/stageA -DWOWDECOMP_RETAIL_ASSETS=ON -DWOWDECOMP_ENABLE_ASEC_M3=OFF -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON
cmake --build build/stageA --config Release --target RetailAssetProbe CdnCacheTrim --parallel
```

Copy `Tools/RetailAssets/cdn_config.example.json` to `cdn_config.json` and set the pinned build
(`build` is required; for an archived build also set `cdn` and `version`). `product` is `wow`,
`wow_beta` or `wow_ptr`; `region` is `us` or `eu`; `cache` is the durable cache folder.
Copy `Tools/ServerSync/sync_config.example.json` to `sync_config.json` and point `server_root` at
`EsteriaWoW` (or set `ESTERIA_SERVER_ROOT`) and `server_data_dir` at the worldserver `DataDir`
(the folder that holds `dbc/` and `dbc-continuations/`; locally `C:/Esteria/server/data`).

## (a) Pull Retail item IDs or armor sets from the CDN

Items, `display:<ItemDisplayInfoID>` and `set:<TransmogSetID>` can be mixed; `@file.txt` reads a list.

```powershell
build\stageA\Release\RetailAssetProbe.exe cdn_config.json --resolve-items 19019,12640,set:1100 --out build\retail-sync\resolved_manifest.json
```

Useful flags: `--product wow_beta`, `--region eu`, `--cache D:\RetailCache`, `--build <32 hex>`,
`--icons-dir <dir>`, `--graph-only` (walk the tables and models, skip other downloads). Wrapper form:

```powershell
python Tools\ServerSync\esteria_sync.py --config sync_config.json resolve --items 19019,12640,set:1100 --retail-ini cdn_config.json --out build\retail-sync\resolved_manifest.json
```

### Whole-catalogue pulls (`--all-*`)

For production, select Retail's catalogue instead of listing IDs (a list is then optional and adds to it):

```powershell
build\stageA\Release\RetailAssetProbe.exe cdn_config.json --resolve-items --all-catalog --out build\retail-sync\resolved_manifest.json
```

| Flag | Selects |
|---|---|
| `--all-sets` | every transmog set (`TransmogSet.db2` + `TransmogSetItem.db2`) incl. raid tier sets |
| `--all-armor` | every armor appearance, all slots (head, shoulder, chest/robe, waist, legs, feet, wrist, hands, back, tabard, shirt) and armor types |
| `--all-weapons` | every weapon, shield and held-in-off-hand item, incl. bows, guns, crossbows, wands, thrown |
| `--all-catalog` | all three |
| `--min-expansion N` / `--max-expansion N` | keep items/sets from that `ExpansionID` range (0 = Classic ... 10 = The War Within, 11 = Midnight) |
| `--skip-uncollectible` | heuristic only: drop Poor-quality items and items with no `ItemSparse` row. It is **not** Blizzard's collectibility flag |
| `--every-item` | keep every item; by default one representative item (lowest ID) per `ItemAppearance` is kept, which is what transmog needs |

Measured on the pinned 12.x build: `--all-catalog --min-expansion 11 --max-expansion 11` selects 520 sets and
2,140 appearance-unique items and resolves to ~46,000 files.

**Parallel downloads and the checkpoint.** A single CASC source fetches one file at a time (~0.25 s), so
bulk pulls run `--workers N` (default 8) independent CASC sources, each with its own cache folder seeded
from the main one by hard links, pooled HTTP connections and 4 attempts per file with back-off. Finished
downloads are merged into the main decoded cache after every dependency wave (a crashed run keeps what the
workers had finished). Expect roughly 0.7 GB of RAM per worker (each opens its own CASC index) and a minute or two of start-up
before the first file lands; use `--workers 4` on small machines.
Progress, files/s and failures are printed per 250 files.

Every resolved file is appended to a **checkpoint** (`cdn_fetch_state-<build8>.json` beside `--out`, or
`--journal <file>`): JSON lines with size, SHA-256 and the files its M2/skeleton references. A rerun skips
every file that is both recorded and still in the cache, without re-reading, hashing or re-parsing it, so
an interrupted 10 GB pull resumes at once. Failed files are logged but never marked done, so the next run
retries only those. (It is a JSON-lines file, not a SQLite database.) A different build never reuses it.

What it walks: `Item/ItemSparse -> ItemModifiedAppearance -> ItemAppearance -> ItemDisplayInfo ->
ModelFileData (+ComponentModelFileData) -> M2 -> .skin .anim .phys .skel .bone, textures, particle models;
TextureFileData (+ComponentTextureFileData), ItemDisplayInfoMaterialRes / ModelMatRes; HelmetGeosetData; icons`.
Particle emitters and ribbons name their textures through the model's own `TXID` table (there is no separate `PTID` chunk);
`GPID` (geometry particles) and `RPID` (recursive particles) list the child models an emitter spawns, and both are walked.

Missing files make the exit code 3 and are listed under `missing`. Validate the cache afterwards (read-only):

```powershell
build\stageA\Release\CdnCacheTrim.exe "Cache\Retail"
```

## (b) Build `patch-R.mpq`

Stage the DBC first (section c), then:

```powershell
python Tools\PatchBuilder\patch_builder.py build --manifest build\retail-sync\resolved_manifest.json --stage build\retail-sync\stage --cache "Cache\Retail" --out build\patch-R --stormlib "$ESTERIA\Esteria - Client\StormLib.dll" --audit build\stageA\Release\CdnCacheTrim.exe
```

`--patch-letter X` renames the archives, `--max-archive-mb N` changes the split size (default 1800),
`--extra <dir>` merges more files (for example EsteriaShell output), `--dry-run` writes listings
instead of MPQs, `--force` ignores the incremental check. Output in `build\patch-R`:
`patch-R.mpq` (DBFilesClient, Custom, and as many icons as fit), `patch-R-1.mpq`, `-2` ... (remaining icons and
the `EsteriaRetail\<product>-<build>\<locale>\<id>.bin` originals), `manifest.json` (FileDataID, CDN build,
target `ItemDisplayInfo` ID, `item_template.entry`, SHA-256 of every entry) and `patch-checksums.txt`.
Copy the archives into `<client>\Data\`. A rerun only repacks archives whose content digest changed.

At catalogue scale: **every** archive, including `patch-R.mpq`, is kept under `--max-archive-mb` (StormLib and
the 3.3.5 client cannot open a 2 GB MPQ), so tens of GB become dozens of numbered volumes automatically.
Icons are stored once (`Interface\Icons`), not twice. Originals are verified with the FNV-1a checksum in
parallel worker processes (`--jobs N`; `--verify header` skips the per-byte check when `--audit` already ran).
The DB2 tables listed in the manifest (`tables`) are packed under `EsteriaRetail\...\<locale>\tables\<id>.bin`,
so the client's armor index loads without any CDN. Past 20,000 files the archive embeds only a digest summary
of the manifest/sync map (the full ones stay in `build\patch-R\manifest.json` and your manifest file).
`--no-keep-stage` deletes `<out>\stage` afterwards; staging otherwise holds a second copy of every original.

## (c) Sync DBCs and SQL to EsteriaWoW

Preview first; nothing is written:

```powershell
python Tools\ServerSync\esteria_sync.py --config sync_config.json sync --manifest build\retail-sync\resolved_manifest.json --dry-run
```

Write the files:

```powershell
python Tools\ServerSync\esteria_sync.py --config sync_config.json sync --manifest build\retail-sync\resolved_manifest.json
```

This (1) allocates ItemDisplayInfo IDs (from 700000) and `item_template` entries (from 1000000) that do not
collide with the base DBC or other projects' continuations, and remembers them in `state_file` so reruns
reuse them; (2) writes `ItemDisplayInfo.dbc1-<project>` to `<server_data_dir>\dbc-continuations\` (the
`mod-wxl-dbc` format: a normal WDBC file holding only the new rows; the base DBC is never edited) and the
full merged `ItemDisplayInfo.dbc` + `Item.dbc` to `<stage>\DBFilesClient\` for the MPQ; (3) writes idempotent `rev_<timestamp>_retail_items.sql` migrations
(DELETE then INSERT, one transaction per file of up to `chunk_size` = 2000 items, ascending timestamps) to
`<server_root>\data\sql\updates\pending_db_world\`, which `ac-db-import` or `dbimport` applies on the next
start. A 50,000-item import is ~25 small files; a rerun reuses files whose content is unchanged and adds
newer files for the rest (the updater replays them in order, newest wins). It refuses to write into
`data/sql/base`, `archive` or `updates/db_*`.

**ID safety at 50,000+ items.** A new `item_template` entry is never one that is already used: the allocator
skips (a) every live `item_template` entry >= the floor (when `--dry-run-live`/`--apply-live` can reach the
database; `--apply-live` refuses to allocate blind if it cannot), (b) every number that looks like an entry in
the `pending_db_world` and `updates/db_world` SQL (a conservative scan: it can only reserve extra IDs), and
(c) `reserved_ranges` from `sync_config.json`, e.g. `"reserved_ranges": {"item_template": [[1000000, 1001999]]}`
for Conquest of Azeroth or existing custom items; the same exists for `ItemDisplayInfo`. Stock 3.3.5 items
(max ~56,806) sit far below the 1,000,000 floor. If an ID this tool allocated earlier is now taken by
someone else, the run stops with an error instead of overwriting it. Allocation and the DBC upserts are
linear time.
Add `--vendor <npc_entry>` to also add the items to that vendor. Restart the worldserver to load the DBC.

To also upsert into a running database (`127.0.0.1:3306`, `acore_world`) with an automatic backup of the
affected rows (`REPLACE` statements saved next to the stage folder):

```powershell
python Tools\ServerSync\esteria_sync.py --config sync_config.json sync --manifest build\retail-sync\resolved_manifest.json --apply-live
```

The upsert runs in batches of `chunk_size` rows, each in its own transaction (a failing batch rolls back and
stops the run; earlier batches stay applied and a rerun is idempotent), and SQL is sent over stdin so batch
size is not limited by the Windows command line. Rows that already exist but were not written by this tool
abort the run; `--force` overwrites them (the backup is still taken). `--dry-run --dry-run-live` diffs against the live table. The live path uses the
`mysql` client (`--mysql <path>` if it is not on `PATH`).

## Tests

```powershell
python Tools\ServerSync\test_server_sync.py
python Tools\PatchBuilder\test_patch_builder.py
build\stageA\Release\ResolverTests.exe
```

## Limits worth knowing

* `item_template` rows carry name, quality, level, price, class/race and inventory type. Retail armor
  values and stats are scaled at runtime and are not imported; add them in SQL if you need them.
* The `ItemDisplayInfo` row points at `retail/<FileDataID>.m2|.blp` names and uses the unisex or male
  variant of each component model; every variant is still packed. A client build that resolves those
  names (the Retail source) is required. The client ignores `.dbcN-<project>` continuations (server-only), so the
  sync merges the new rows into the client's current winning `ItemDisplayInfo.dbc` and `Item.dbc` (set
  `client_data_dir` and `stormlib` in `sync_config.json`) and PatchBuilder ships those complete tables alone in
  `patch-ZZZ...ZRetailItems.mpq` (30 Z's), which sorts after every other client patch and therefore wins.
  Use `--number-all` so the volumes are `patch-R-1..N` and never collide with an existing `Patch-R.MPQ`.
* Item icons are packed as the original Retail BLP under `Interface\Icons\RetailIcon_<id>.blp`.
* MPQ bytes are not bit-reproducible (StormLib writes timestamps); contents, names and split points are.
