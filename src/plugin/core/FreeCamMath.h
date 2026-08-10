// FreeCamMath.h - lógica PURA de la cámara libre (sin dependencias de Ogre ni
// del juego). Aislada aquí para poder testearla en prototest.exe (igual que
// WorkPose.h / DeathLatch.h): dado el estado (yaw, pitch, posición) y el input
// del teclado, calcula la nueva posición y el vector de vista de la cámara.
//
// La implementación runtime (FreeCamera.cpp) usa ESTAS MISMAS funciones y luego
// vuelca el resultado en el Ogre::Camera* real, de modo que lo que valida el
// test es exactamente la matemática que corre en el juego.
//
// Convención (Ogre, right-handed, la cámara mira hacia -Z local):
//   yaw   = rotación horizontal alrededor de +Y  (0 => mirando a -Z)
//   pitch = rotación vertical                     (0 => horizontal; + => arriba)

#ifndef KENSHICOOP_FREECAM_MATH_H
#define KENSHICOOP_FREECAM_MATH_H

#include <cmath>

namespace coop {

// Vector 3D mínimo (sin depender de Ogre::Vector3 para que el test no enlace Ogre).
struct FcVec3 {
    float x, y, z;
};

// Estado del input de un frame (una tecla por eje de movimiento/rotación).
struct FcInput {
    bool fwd, back, left, right; // WASD: avanzar/retroceder/strafe
    bool up, down;               // Q/E o Space/Ctrl: subir/bajar en +Y mundo
    bool turbo;                  // Shift: multiplicador de velocidad
};

// Vector de VISTA completo (incluye pitch): hacia dónde mira la cámara. Se pasa
// tal cual a Ogre::Camera::setDirection en la implementación real.
inline FcVec3 fcForward(float yaw, float pitch) {
    float cy = std::cos(yaw),  sy = std::sin(yaw);
    float cp = std::cos(pitch), sp = std::sin(pitch);
    FcVec3 f;
    f.x = sy * cp;
    f.y = sp;
    f.z = -cy * cp;
    return f;
}

// Vector DERECHA horizontal (ignora el pitch): eje de strafe A/D. Ortogonal al
// forward horizontal, mantiene el strafe siempre pegado al plano del suelo.
inline FcVec3 fcRight(float yaw) {
    FcVec3 r;
    r.x = std::cos(yaw);
    r.y = 0.0f;
    r.z = std::sin(yaw);
    return r;
}

// Desplazamiento de un frame (aún sin escalar por velocidad/tiempo): suma de los
// ejes activos. Se normaliza a magnitud 1 cuando hay input, para que moverse en
// diagonal no vaya más rápido que en recto (y para que el test sea determinista).
inline FcVec3 fcMoveDir(float yaw, float pitch, const FcInput& in) {
    FcVec3 f = fcForward(yaw, pitch);
    FcVec3 r = fcRight(yaw);
    FcVec3 d = { 0.0f, 0.0f, 0.0f };
    if (in.fwd)   { d.x += f.x; d.y += f.y; d.z += f.z; }
    if (in.back)  { d.x -= f.x; d.y -= f.y; d.z -= f.z; }
    if (in.right) { d.x += r.x; d.y += r.y; d.z += r.z; }
    if (in.left)  { d.x -= r.x; d.y -= r.y; d.z -= r.z; }
    if (in.up)    { d.y += 1.0f; }
    if (in.down)  { d.y -= 1.0f; }
    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 1e-4f) { d.x /= len; d.y /= len; d.z /= len; }
    return d;
}

// Nueva posición tras un frame: pos + dir_normalizada * velocidad * dt, con el
// multiplicador de turbo aplicado. dtSeconds es tiempo REAL de reloj (funciona
// aunque el juego esté en pausa). baseSpeed en unidades de mundo por segundo.
inline FcVec3 fcStep(FcVec3 pos, float yaw, float pitch, const FcInput& in,
                     float baseSpeed, float turboMul, float dtSeconds) {
    FcVec3 d = fcMoveDir(yaw, pitch, in);
    float speed = baseSpeed * (in.turbo ? turboMul : 1.0f);
    float k = speed * dtSeconds;
    FcVec3 out;
    out.x = pos.x + d.x * k;
    out.y = pos.y + d.y * k;
    out.z = pos.z + d.z * k;
    return out;
}

// Aplica un delta de rotación y CLAMPEA el pitch para no dar la vuelta de campana
// (±~85°). yaw se deja libre (envuelve de forma natural en el seno/coseno).
inline void fcApplyLook(float* yaw, float* pitch, float dYaw, float dPitch) {
    const float PITCH_LIMIT = 1.48353f; // ~85 grados en radianes
    *yaw += dYaw;
    *pitch += dPitch;
    if (*pitch >  PITCH_LIMIT) *pitch =  PITCH_LIMIT;
    if (*pitch < -PITCH_LIMIT) *pitch = -PITCH_LIMIT;
}

} // namespace coop

#endif // KENSHICOOP_FREECAM_MATH_H
