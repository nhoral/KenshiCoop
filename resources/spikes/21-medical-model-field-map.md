# Spike 21 - Health / medical model field map

- Type: STATIC (header) + runtime read
- Status: DONE
- Source: `kenshi/MedicalSystem.h`, `kenshi/CharBody.h`; runtime `engine::readMedical`

## Goal

Map Kenshi's complete health/medical data model so we know exactly what a co-op
medical sync would have to carry.

## The model (from MedicalSystem.h)

`MedicalSystem` is a direct member of `Character` (`c->medical`, no accessor call -
see `engine::killSubject`/`woundSubject`). Key fields (offsets verbatim):

- **Whole-body fluids:** `float blood` (0x70), `getMaxBlood()`, `float currentBleedRate`
  (0x78), `float extraBloodLossFromBodyparts` (0x74), `float hunger`/`fed` (0x60/0x64).
- **Consciousness/death:** `float knockoutTimer` (0xA0), `bool unconcious` (0x161),
  `bool dead` (0x164), `bool crippled` (0x160), `bool bloodlossTrauma` (0x163),
  `bool sub50KO` (0x162). Methods `isUnconcious()`, `isDead()`, `isCrippled()`,
  `isInBloodlossTrauma()`, `isProbablyDying()`, `knockout(skill)`,
  `knockoutForceTimer(sec)`, `pointOfCollapseBloodloss()`, `pointOfNoReturn()`.
- **Per-limb health:** four `HealthPartStatus*` (`leftLeg`/`rightLeg`/`leftArm`/
  `rightArm`, 0x80-0x98) plus `lektor<HealthPartStatus*> anatomy` (0x190) and a
  `status` map keyed by bodypart `GameData*`. Each `HealthPartStatus` has
  `float flesh` (0x40, current HP), `float _maxHealth` (0x54), `float bandaging`
  (0x48), `float juryRigging` (0x4C), `bool selfHealing/collapses/fatal`,
  `getExtraBleedingAmount()`, `isDead()`, `healthAsPercent()`,
  `derivedFleshHealthPercent` (0x60), `PartType {TORSO,LEG,ARM,HEAD}`.
- **Limb presence (sever/prosthetic):** `RobotLimbs* robotLimbs` (0xC8) with
  `LimbState states[4]` and `Item* items[4]`; `enum LimbState {ORIGINAL=0, STUMP=1,
  REPLACED=2, CRUSHED=3}`; `RobotLimbs::Limb {LEFT_ARM,RIGHT_ARM,LEFT_LEG,RIGHT_LEG}`.
  `MedicalSystem::amputate(limb, createSeveredItem, force)`, `crushLimb(limb)`,
  `getLimbState(limb)`, `hasFreshlySeveredALimb()`.
- **Wounds:** `Ogre::FastArray<Wound*> wounds` (0x178); `addWound(lowBlow, area,
  damage, material, attacker, dir, harpoon)`; `applyDamage(part, damage, ...)`.
- **First aid/healing:** `applyFirstAid(skill, equipment, frameTime, who)`,
  `applyDoctoring(...)`, `applyRigging(...)`, `scoreFirstAidNeed()`,
  `precalculateFirstAidNeedScore()`.

## Runtime confirmation

`engine::readMedical` reads these live (spike 22 run): a baked `down1` NPC showed
`blood=75.8`, all limbs `flesh=100`, `unc=0 dead=0`, `robotLimbs` null (so
`states[]` read returned -1 - a fleshy human has no RobotLimbs object until it gets
a prosthetic; limb presence for flesh limbs is tracked via the `HealthPartStatus`/
`anatomy`, not `RobotLimbs::states`).

## Findings

1. The medical model is **large and entirely local** - dozens of floats/bools per
   character, four limb structs, a wound array, a robot-limb table. Nothing here
   touches the network today.
2. Co-op medical sync would need to carry, at minimum: `blood`, `currentBleedRate`,
   `unconcious`, `dead`, `crippled`, and per-limb `flesh` + sever state - far more
   than the single `bodyState` bitfield the wire currently has.
3. `RobotLimbs::states[]` is only populated for robotic/severed limbs; use
   `HealthPartStatus::flesh` + `getLimbState()` for fleshy-limb loss, not `states[]`.

## Implications for co-op

- A faithful medical sync is a real feature, not a field add: it needs a new
  per-character medical delta packet (blood + bleed + 4x limb HP/state) on a
  throttled channel, host-authoritative.
- Cheaper interim: sync only the COARSE outcomes players notice - down/KO/dead
  pose (already partpossible via bodyState) + "lost a limb" one-shot events - and
  let each client's local medical sim fill the rest. See spikes 22-27.

## Recommended follow-ups

- Define a `MedicalDelta` wire struct and a host->join throttled medical channel.
- Decide policy: full sim sync vs coarse-event sync (spike 32-style conflict view).
