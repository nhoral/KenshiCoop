// Shim: the vendored KenshiLib ogre headers reference OgreConfig.h but do not
// ship it (first hit: Building.h -> Math/Simple/OgreAabb.h -> OgreArrayConfig.h).
// Only the SIMD selection macros are consulted; force the plain-C path so
// Ogre::Aabb is the 24-byte two-Vector3 POD - which is exactly what the dumped
// Building layout says the game uses (Building::AABB spans 0x27C..0x294).
#ifndef KENSHICOOP_SHIM_OGRE_CONFIG_H
#define KENSHICOOP_SHIM_OGRE_CONFIG_H

#define OGRE_DOUBLE_PRECISION 0
#define OGRE_USE_SIMD 0

#endif
