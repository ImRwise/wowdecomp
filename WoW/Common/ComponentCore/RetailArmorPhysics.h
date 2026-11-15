#ifndef WOW_COMMON_COMPONENTCORE_RETAILARMORPHYSICS_H
#define WOW_COMMON_COMPONENTCORE_RETAILARMORPHYSICS_H

// Not in the original binary. Secondary motion (capes, tabards, belts, dangling plates and chains) for Retail armor
// worn on a native Retail character rig. The solver is Engine/Source/Retail/ArmorPhysics; this adapter finds the
// body rig's free bones (no key bone id, no animation tracks, not billboards, not already driven by something else),
// samples the Living Azeroth wind at the wearer, and writes the result as procedural bone transforms
// (CM2Model::SetBoneProceduralTransformByIndex, bone flag 0x80). Armor collections skin to those body bones, so the
// armor follows without any mesh work.
//
// CVars (saved, "Esteria" video options family):
//   armorPhysics           1  master switch (0 resets every bone to its animated pose)
//   armorPhysicsStrength   1  scales every response (0..2)
//   armorPhysicsWind       1  scales the wind's share (0..3), 0 = wearer motion and gravity only
//   armorPhysicsMax       32  most wearers simulated per frame (1..256); the rest keep their animated pose
//   armorPhysicsDebug      0  1 logs per-character bone counts and peak angles about once a second
//
// Everything is main-thread. Compiled only with WOWDECOMP_RETAIL_ASSETS and WOWDECOMP_RETAIL_ARMOR_PHYSICS.

#include <cstdint>

class CCharacterComponent;
class CM2Model;

namespace RetailArmorPhysics {

void RegisterCVars();

// Wind at a world position, yards/second, world frame. Returns false when there is no wind (outside the world, no
// WindProfile row). Set by the Living Azeroth module; a null provider means still air.
using WindProvider = bool (*)(const float position[3], float outVelocity[3]);
void SetWindProvider(WindProvider provider);

// Called once per frame for every active Retail character, after its collections were synchronised. `wearer` is the
// body model; `hasArmor` says whether any armor collection is attached (nothing to move otherwise).
void Tick(CCharacterComponent* character, CM2Model* wearer, bool hasArmor, uint32_t nowMs);

// Drops a character's state without touching its model: the component is going away (FreeComponent).
void Forget(CCharacterComponent* character);

// Drops every character's state (CharacterSwap, world teardown). Models are not touched: they are being replaced.
void ForgetAll();

// Number of characters currently simulated (diagnostics, tests).
uint32_t ActiveCount();

}  // namespace RetailArmorPhysics

#endif
