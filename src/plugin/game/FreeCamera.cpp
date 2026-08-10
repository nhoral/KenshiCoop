// FreeCamera.cpp - modo de cámara LIBRE local (feature de capturas / vídeo).
//
// Kenshi 1.0.68 añadió un free-camera nativo (tecla ';'), pero ese código NO
// existe en el binario 1.0.65 sobre el que corre KenshiCoop: aquí se reimplementa
// desde cero sobre la infraestructura que sí tenemos.
//
// Estrategia (independiente de versión): NO dependemos de ningún RVA de función
// de CameraClass (los del header vendored no corresponden a 1.0.65). Usamos solo
//   - los OFFSETS DE MIEMBRO de CameraClass (estables entre versiones): el
//     Ogre::Camera* vive en +0x68 (cam->camera), su SceneNode en +0x70 (cam->node),
//     y el estado orbital yaw/pitch/altitude en +0x18/+0x1C/+0x60;
//   - la API pública de Ogre (OgreMain_x64.lib ya está enlazada).
//
// MECÁNICA: la Ogre::Camera cuelga del SceneNode +0x70 (confirmado en runtime:
// getParentSceneNode() == cam->node). En vez de RE-ESTRUCTURAR el grafo de escena
// (detachObject es frágil: ABI de Ogre + carrera con el hilo de render), cada
// frame - DESPUÉS de que el update() orbital del motor haya corrido - sobre-
// escribimos la POSICIÓN y ORIENTACIÓN de ese node. La cámara, que cuelga de él,
// va donde la ponemos. Es exactamente lo que el propio motor hace en su update(),
// así que es tan seguro respecto al hilo de render como el juego base. Mientras
// nuestro tick corra cada frame, la cámara se queda libre; al desactivar, el
// update() del motor reencuadra el node sobre el personaje al frame siguiente.
//
// 100% CLIENTE/VISUAL: no toca ningún canal de sync ni se replica al peer. Cada
// jugador activa su propia cámara libre de forma independiente.
//
// Control (teclado puro, para no pelearse con el cursor RTS de Kenshi):
//   F3            toggle on/off
//   W/S/A/D       avanzar/retroceder/strafe en el plano de la cámara
//   Q/E           bajar/subir en el eje vertical
//   Flechas       orientar (yaw/pitch)
//   Shift         turbo (x4 velocidad)
// El delta de movimiento usa reloj REAL (GetTickCount), así la cámara se mueve
// aunque el juego esté en pausa - lo natural para preparar una captura.

#include "EngineInternal.h"
#include <ogre/OgreSceneNode.h> // SceneNode::setPosition/setOrientation
#include <ogre/OgreCamera.h>    // Camera::getDerivedPosition/getDerivedOrientation
#include "../core/FreeCamMath.h"

namespace coop {
namespace engine {

namespace {

// ---- Estado (main-thread only; una cámara libre por cliente) ----------------
bool             s_active   = false; // ¿cámara libre activa ahora mismo?
bool             s_keyDown  = false; // último estado de F3 (para el flanco de subida)
Ogre::Camera*    s_camera   = 0;     // la Ogre::Camera del cliente (== cam->camera al activar)
Ogre::SceneNode* s_node     = 0;     // su SceneNode (cam->node, +0x70) - el que movemos
FcVec3           s_pos       = { 0.0f, 0.0f, 0.0f }; // posición de la cámara libre
float            s_yaw       = 0.0f; // orientación de la cámara libre (radianes)
float            s_pitch     = 0.0f;
unsigned long    s_lastMs    = 0;    // marca de tiempo del frame anterior (delta real)
// Estado orbital guardado al entrar, restaurado al salir para que la vista normal
// no quede en un ángulo raro (el update() del motor reencuadra desde estos).
float            s_savedYaw  = 0.0f;
float            s_savedPitch = 0.0f;
float            s_savedAlt  = 0.0f;
// Transform LOCAL del node guardado al entrar. El motor NO reencuadra el node por
// sí solo tras salir (solo lo mueve ante input de cámara), así que restauramos su
// posición/orientación exactas para devolver la vista normal sin residuos.
Ogre::Vector3    s_savedNodePos;
Ogre::Quaternion s_savedNodeOrient;

const int   FREECAM_KEY = VK_F3;   // F2 ya lo usa el panel co-op; F3 queda libre
const float BASE_SPEED  = 40.0f;   // unidades por segundo
const float TURBO_MUL   = 4.0f;    // multiplicador con Shift
const float LOOK_SPEED  = 1.6f;    // rad/s de giro con las flechas

// ¿tecla física pulsada AHORA? (bit alto de GetAsyncKeyState).
inline bool keyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Obtiene el CameraClass* vivo del cliente (gw->player->camera) validado como
// inicializado. Devuelve también el Ogre::Camera* actual para poder detectar un
// cambio de mundo (recarga) sin volver a derreferenciar fuera del SEH del caller.
// Sin SEH propio: el caller (freeCameraTick) provee el marco __try.
bool readCameraClass(GameWorld* gw, CameraClass** outCam, Ogre::Camera** outOgre) {
    *outCam = 0; *outOgre = 0;
    if (!gw || !g_camIsInitFn) return false;
    PlayerInterface* pl = gw->player;
    if (!pl) return false;
    CameraClass* cam = pl->camera;
    if (!cam || !g_camIsInitFn(cam)) return false;
    *outCam  = cam;
    *outOgre = cam->camera; // miembro +0x68
    return true;
}

// Reconstruye yaw/pitch (nuestra convención, ver FreeCamMath.h) desde la
// orientación actual de la cámara, para que al activar no haya salto.
void seedYawPitchFromCamera(Ogre::Camera* oc) {
    Ogre::Quaternion q = oc->getDerivedOrientation();
    Ogre::Vector3 dir = q * Ogre::Vector3::NEGATIVE_UNIT_Z; // la cámara mira a -Z local
    float dy = dir.y;
    if (dy >  1.0f) dy =  1.0f;
    if (dy < -1.0f) dy = -1.0f;
    s_pitch = (float)asin((double)dy);
    // fcForward: x = sin(yaw)*cos(pitch), -z = cos(yaw)*cos(pitch) => yaw = atan2(x,-z)
    s_yaw = (float)atan2((double)dir.x, (double)(-dir.z));
}

// ACTIVA la cámara libre: memoriza la cámara + su node y el estado orbital, y
// siembra pos/yaw/pitch desde la vista actual. NO reestructura el grafo (no
// detach). Sin SEH propio (caller lo provee).
void enterFree(CameraClass* cam) {
    Ogre::Camera* oc = cam->camera;         // +0x68
    Ogre::SceneNode* node = cam->node;      // +0x70 (el node al que cuelga la cámara)
    if (!oc || !node) { coop::logLine("[freecam] ABORT oc/node==0"); return; }

    // Posición/orientación actuales (para arrancar sin salto).
    Ogre::Vector3 wp = oc->getDerivedPosition();
    seedYawPitchFromCamera(oc);
    s_pos.x = wp.x; s_pos.y = wp.y; s_pos.z = wp.z;

    // Guarda el estado orbital para restaurarlo al salir (miembros estables).
    s_savedYaw   = cam->yaw;      // +0x18
    s_savedPitch = cam->pitch;    // +0x1C
    s_savedAlt   = cam->altitude; // +0x60
    // Guarda el transform LOCAL del node para restaurarlo exactamente al salir.
    s_savedNodePos    = node->getPosition();
    s_savedNodeOrient = node->getOrientation();

    s_node   = node;
    s_camera = oc;
    s_lastMs = GetTickCount();
    s_active = true;
    { char b[112]; _snprintf(b, sizeof(b)-1,
        "[freecam] ON (camara libre; desacoplada del personaje) pos=%.1f,%.1f,%.1f",
        wp.x, wp.y, wp.z); b[sizeof(b)-1]='\0'; coop::logLine(b); }
}

// DESACTIVA: restaura el estado orbital guardado; el update() del motor reencuadra
// el node sobre el personaje al frame siguiente. 'cam' puede ser 0 (mundo
// recargado): en ese caso solo limpiamos estado.
void exitFree(CameraClass* cam) {
    // Restaura el transform del node a su estado exacto pre-freecam (el motor no
    // lo reencuadra solo), y los miembros orbitales por si el motor los relee.
    if (s_node) {
        s_node->setPosition(s_savedNodePos);
        s_node->setOrientation(s_savedNodeOrient);
    }
    if (cam) {
        cam->yaw      = s_savedYaw;
        cam->pitch    = s_savedPitch;
        cam->altitude = s_savedAlt;
    }
    s_active = false;
    s_camera = 0;
    s_node   = 0;
    coop::logLine("[freecam] OFF (camara restaurada al personaje)");
}

// Un frame de cámara libre: lee el teclado, integra posición/orientación con el
// delta de reloj REAL y sobre-escribe el transform del node de la cámara. Sin SEH
// propio. Corre POST-motor, así que pisa lo que el update() orbital acaba de poner.
void updateFree() {
    if (!s_camera || !s_node) return;

    unsigned long now = GetTickCount();
    float dt = (float)(now - s_lastMs) / 1000.0f;
    s_lastMs = now;
    if (dt <= 0.0f) return;
    if (dt > 0.25f) dt = 0.25f; // clampa picos de lag / primer frame

    // Orientación con las flechas.
    float dYaw = 0.0f, dPitch = 0.0f;
    if (keyHeld(VK_LEFT))  dYaw   -= LOOK_SPEED * dt;
    if (keyHeld(VK_RIGHT)) dYaw   += LOOK_SPEED * dt;
    if (keyHeld(VK_UP))    dPitch += LOOK_SPEED * dt;
    if (keyHeld(VK_DOWN))  dPitch -= LOOK_SPEED * dt;
    fcApplyLook(&s_yaw, &s_pitch, dYaw, dPitch);

    // Traslación con WASD + Q/E.
    FcInput in;
    in.fwd   = keyHeld('W');
    in.back  = keyHeld('S');
    in.left  = keyHeld('A');
    in.right = keyHeld('D');
    in.up    = keyHeld('E');
    in.down  = keyHeld('Q');
    in.turbo = keyHeld(VK_SHIFT);
    s_pos = fcStep(s_pos, s_yaw, s_pitch, in, BASE_SPEED, TURBO_MUL, dt);

    // Sobre-escribe el transform del node (la cámara cuelga de él). Construimos la
    // orientación desde ejes con UP vertical fijo (no getRotationTo, que introduce
    // roll/ladeo): la cámara mira -Z local, así que sus ejes locales en world son
    // right/up/(-forward). Esto mantiene el horizonte nivelado al girar.
    FcVec3 f = fcForward(s_yaw, s_pitch);
    Ogre::Vector3 fwd(f.x, f.y, f.z);
    Ogre::Vector3 right = fwd.crossProduct(Ogre::Vector3::UNIT_Y);
    if (right.squaredLength() < 1e-6f) right = Ogre::Vector3::UNIT_X; // mirada casi vertical
    right.normalise();
    Ogre::Vector3 up = right.crossProduct(fwd);
    up.normalise();
    Ogre::Quaternion q(right, up, -fwd); // ejes X=right, Y=up, Z=-forward
    s_node->setPosition(Ogre::Vector3(s_pos.x, s_pos.y, s_pos.z));
    s_node->setOrientation(q);
}

} // namespace

// Punto de entrada por-frame (llamado desde mainLoop_hook, POST-motor, para que
// nuestra transformación sea la última que muestrea el renderer). 'enabled' es el
// master-switch de config (KENSHICOOP_FREE_CAMERA). Todo el cuerpo va bajo SEH:
// una cámara/nodo inesperado degrada a no-op en vez de crashear el juego.
void freeCameraTick(GameWorld* gw, bool enabled) {
    __try {
        // Deshabilitada por config: si quedó activa, intenta restaurarla.
        if (!enabled) {
            if (s_active) {
                CameraClass* cam = 0; Ogre::Camera* oc = 0;
                readCameraClass(gw, &cam, &oc);
                exitFree((cam && oc == s_camera) ? cam : 0);
            }
            s_keyDown = false;
            return;
        }

        // Flanco de subida de F3.
        bool k = keyHeld(FREECAM_KEY);
        bool edge = (k && !s_keyDown);
        s_keyDown = k;

        CameraClass* cam = 0;
        Ogre::Camera* oc = 0;
        if (!readCameraClass(gw, &cam, &oc)) {
            // Sin cámara válida (menú de título / world-swap en curso): si
            // estábamos activos, suelta el estado SIN tocar la cámara vieja
            // (puntero potencialmente colgante). El usuario re-activa con F3.
            if (s_active) { s_active = false; s_camera = 0; s_node = 0; }
            return;
        }

        // ¿El mundo se recargó bajo nosotros? (la Ogre::Camera es otra). Suelta el
        // estado: la cámara nueva no está bajo nuestro control, solo reseteamos.
        if (s_active && oc != s_camera) {
            s_active = false; s_camera = 0; s_node = 0;
        }

        if (edge) {
            if (!s_active) enterFree(cam);
            else           exitFree(cam);
        }

        if (s_active) updateFree();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Falla con gracia: abandona el modo libre sin arrastrar punteros dudosos.
        s_active = false; s_camera = 0; s_node = 0;
    }
}

} // namespace engine
} // namespace coop
