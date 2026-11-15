# Retail armor: geosets, texture layers and physics (milestones 1B, 1C, 1D)

Guarded by `WOWDECOMP_RETAIL_ASSETS` and `WOWDECOMP_RETAIL_ARMOR_PHYSICS` (ON by default with RETAIL_ASSETS). Nothing here
needs a local Retail install: files come from the packed `patch-R*.mpq`, the decoded cache, or the CDN.

## What runs where

| Milestone | Code | Notes |
|---|---|---|
| 1B geosets/attachments | `WoW/Common/ComponentCore/RetailRace.cpp` (`AddRetailArmorParts`, `RetailHelmetHiddenGroups`, `HelmetHides`), `CharacterComponent.cpp` (`AddHelm`) | Armor M2s are bound as skin-parent collections onto the native Retail rig, so race/gender/Dracthyr/two-form rigs from `RetailRaces.csv` are honoured. Only the seven helmet hide groups are applied; other `AttachmentGeosetGroup` values are not in the 3.3.5 DBC row. |
| 1C texture layers | `Engine/Source/Retail/ArmorLayers.{h,cpp}`, `CharacterComponent.cpp` (`RetailPasteLayer`), `TextureCache.{h,cpp}` | CPU composite into the character BGRA mip chain, uploaded by D3D11 or Vulkan alike. Layers are alpha-over unless a caller passes another op (copy, multiply, overlay, screen are implemented and unit-tested; `ComponentTextureFileData` carries no per-layer blend, so none is chosen automatically). |
| 1D physics | `Engine/Source/Retail/ArmorPhysics.{h,cpp}` (solver), `WoW/Common/ComponentCore/RetailArmorPhysics.{h,cpp}` (adapter), hooks in `RetailRaceTick` / `RetailRaceForget` / CharacterSwap `OnLogoutComplete`, wind from `LivingWind.cpp` | See below. |

## 1D design

- **Solver**: every free bone is a damped angular spring about its pivot, driven by root acceleration, relative wind and
  off-axis gravity. Cloth chains pass motion down (`follow`), dangles are single springs, rigid plates are hinges with
  limits and restitution. Substepped at 120 Hz, `dt` clamped to 0.1 s (a longer hitch is dropped, not simulated),
  non-finite input is treated as zero, state is reset if it ever goes non-finite. Deterministic.
- **Bones**: a body-rig bone with no key bone id, no translation/rotation/scale tracks, not a billboard, and not already
  using procedural flag 0x80/0x100 (twist, wheels, body shape). More than 96 such bones means the heuristic is not
  describing cloth and the rig is skipped (logged). Max 64 simulated bones per wearer.
- **Output**: `CM2Model::SetBoneProceduralTransformByIndex` + bone flag 0x80, the engine's existing procedural hook; armor
  collections copy the body's final skin matrices, so capes/tabards/belts follow with no mesh work.
- **Wind**: `LivingWind::StrengthAt` (steady wind, travelling gusts, roof shelter, wall lee shadow) in the bearing's
  direction, 14 yd/s per 1.0 strength, minus the wearer's own velocity.
- **Delta time**: from `OsGetAsyncTimeMs` between ticks; first frame, stalls over 0.25 s and moves over 25 yd restart from rest.
- **Cleanup**: `RetailRaceForget` (component freed) and `ForgetAll` (CharacterSwap logout) drop state without touching
  models. A model/rig change drops state without dereferencing the old model. Disabling the CVar or removing all armor
  clears flag 0x80 on the bones of the still-current model.

### Authored physics (`.phys`) and torso lean

- **Reader**: `Engine/Source/Retail/PhysFile.{h,cpp}` parses version 4/5 files (88 of the 94 present in the pinned
  build 88d5e8ef parse; the other six are older generations or a file with no bodies and keep the heuristic). The layout
  is not published; the header comment lists what was decoded and verified across those files: bodies (dynamic flag,
  bone, rest position, shape range), shapes (capsule/sphere/polytope, friction, restitution, density), capsule and sphere
  colliders, the joint table, spherical/revolute/weld joint frames and degree limits. Every record is range-checked and
  a malformed file is refused whole. Real files are fetched by the resolver (kind `physics`), packed by PatchBuilder with
  the other originals, and read through the normal Retail source (packed MPQ, decoded cache, CDN) on a worker thread.
- **What is bound**: topology (joint table = parent/child), rest direction (authored body positions), revolute joints
  become hinge nodes with their authored limits, spherical joints with limits set a cone of their swing/twist angle,
  fixed bodies are anchors. Attached item models (shoulders, belts, helms) get these nodes on their own rig; the body rig
  and any item without a usable `.phys` keep the heuristic. `armorPhysicsAuthored 0` forces the heuristic.
- **Not bound, on purpose**: the five unlabelled floats on each body (`Body::tuning`), joint friction, density and
  colliders. Nothing in the files, or any reference I could check, says which float is stiffness, damping, gravity
  scale or wind influence, so the solver keeps its own defaults for those rather than guess. Colliders are counted and
  logged, not collided against. The hinge axis is the first row of the joint frame (unverified against the game).
- **Lean and turning**: each node samples its parent bone's animated orientation from the model's bone matrices, so
  gravity follows a leaning spine/pelvis/arm, and the frame's angular velocity and (low-passed) angular acceleration throw
  the tip (tangential + centripetal terms). Sprinting, jumping and turning come from the wearer's placement motion as before.
- **M3**: `Extensions/ASECM3/client/PhysicsBridge.h` turns decoded PHRB bodies/joints into the same description (moving =
  mass > 0, chained by skeleton parents). Joint limits are not applied (ASECM3 has not decoded their units) and nothing
  drives it from a live M3 scene yet; the hook point is `SceneFrame::animationLocals`. It is unit-tested only.

### Limits (be aware)

- The heuristic body-rig classifier remains a guess (free bones with no key id or tracks).
- Attached item rigs are simulated in the wearer's frame: the shoulder's own motion from the arm is not fed in.
- Not verified in game yet.

## CVars

| CVar | Default | Meaning |
|---|---|---|
| `armorPhysics` | 1 | master switch |
| `armorPhysicsStrength` | 1 | 0..2, scales every response |
| `armorPhysicsWind` | 1 | 0..3, wind share |
| `armorPhysicsMax` | 32 | wearers simulated per frame |
| `armorPhysicsAuthored` | 1 | use `.phys` joints/limits for attached items (0 = heuristic only) |
| `armorPhysicsDebug` | 0 | log to `Logs/RetailArmorPhysics.log` |
| `worldWind`, `worldWindStrength`, `worldWindShelter`, `worldWindLee` | existing | Living Azeroth wind inputs |

## In-game verification checklist

0. Build with `WOWDECOMP_RETAIL_ASSETS=ON`; confirm `Logs/RetailRace.log` exists after login (Retail race path active).
1. Wear a Retail-race character (a race in `RetailRaces.csv`, or Dracthyr in both forms) with a cloak and a belt/tabard.
   Pick items from your own sync output: run `RetailAssetProbe --resolve-items ... --min-expansion 10` (Midnight/TWW era
   are expansion 10/11 rows) and apply the generated `item_template` entries with `.additem`. Set ids come from the
   `*.items` list the resolver wrote; no ids are quoted here because none were verified against a live Retail table.
2. `/console armorPhysicsDebug 1`, equip the item. Expect lines such as `retail/<id>.m2: N authored nodes from phys <id>`
   (shoulders, belts, helms with a `.phys`) or `… heuristic nodes`, then once a second `character …: nodes … peak … rad`
   in `Logs/RetailArmorPhysics.log`. `no authored physics (no PFID)` is normal for most items. `rig has … free bones` means
   the body rig did not look like cloth: report the model name.
3. Run, stop: the cloak should swing out and settle in under ~2 s. `/console armorPhysics 0` snaps it back to the
   animated pose; `1` resumes (no pops of other animations).
4. Wind: `/run print(LivingAzeroth_GetWind())` in a zone with a WindProfile row; face the cloak downwind/crosswind
   and compare with `/console armorPhysicsWind 0` and `3`. Stand under a roof (calm) and in a wall's lee (reduced).
5. Despawn/swap: target-walk a second player out of range, then use CharacterSwap; no crash, no frozen offsets on the new
   character, log shows no stale pointer messages.
6. Toggle helmets (`ShowHelm`), swap chest/legs/boots/gloves; geosets under armor hide, helm hides hair/face groups per
   `HelmetGeosetData`.
7. Texture layers: a Retail chest/legs piece must show its real texture, never bright green (green = the stock paster
   was used, a bug) and never a corrupt/black sheet. Check D3D11 and the Vulkan backend.
8. Performance: `armorPhysicsMax 4` with a crowd; frame time should not change when it is lowered above the crowd size.
