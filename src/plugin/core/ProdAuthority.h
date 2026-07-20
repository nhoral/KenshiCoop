// ProdAuthority.h - modelo de autoridad POR-OBJETO para la produccion/crafteo
// (protocolo 33). Funcion pura, sin dependencias de juego/Win32, testeable en
// src/prototest/main.cpp (testProdAuthority).
//
// EL PROBLEMA (antes de este fix): el protocolo 33 era HOST-autoritativo en un
// solo sentido (Plugin.cpp: "if isHost publishProd else applyProd"). Una
// maquina de crafteo colocada por el JOIN quedaba conducida por el HOST: si el
// join seleccionaba una receta, applyProd sobrescribia su estado ~1s despues
// para igualarlo al del host y la seleccion se revertia visiblemente. El join
// era un mero espectador de la economia de la base compartida.
//
// LA SOLUCION - particion natural por objeto (a diferencia del dinero, que es
// un pool compartido SIN particion): cada maquina tiene UN unico duenno, y solo
// su duenno publica su estado; el otro lado lo aplica. Como cada maquina tiene
// exactamente una autoridad, NUNCA hay dos escritores sobre la MISMA maquina,
// asi que no hay conflicto de reconciliacion (a diferencia del delta de dinero).
//
// La regla de propiedad reutiliza el precedente del protocolo 27 (buildSync ya
// es "placer-autoritativo"): produccion = quien COLOCO la maquina.
//   - Maquina "baked" (del guardado base, sin colocador): manda el HOST. Esto
//     preserva EXACTAMENTE el comportamiento host-autoritativo previo para todo
//     lo que existia en el save (incluido el caso host-solo).
//   - Maquina "placed" (colocada en sesion, protocolo 27): manda el cliente que
//     la coloco (la tiene en su ownBuilds_). Asi el JOIN conduce las maquinas
//     que EL construyo, y el host las suyas, sin solaparse.
//
// LIMITACION CONOCIDA (documentada, no un bug): una maquina "baked" preexistente
// sigue siendo host-autoritativa; el join no puede conducirla. En co-op las
// bases se construyen entre los jugadores (maquinas "placed"), que es el caso
// real cubierto; conducir una maquina baked desde el join queda como caso borde
// fuera de alcance (requeriria arbitraje de "primer-tocador", que reintroduce el
// problema de 2 escritores que este modelo evita a proposito).

#ifndef COOP_PROD_AUTHORITY_H
#define COOP_PROD_AUTHORITY_H

namespace coop {

// Decide si ESTE cliente es la AUTORIDAD de produccion de una maquina concreta.
// Parametros (todos calculables localmente, sin red):
//   isHost        - true si este cliente es el host de la sesion.
//   isPlaced      - true si la maquina se coloco en sesion (protocolo 27); false
//                   si es una maquina "baked" del guardado base.
//   placedByLocal - true si la maquina "placed" la coloco ESTE cliente (esta en
//                   su ownBuilds_); false si la coloco el peer (proxy minted).
//                   Solo tiene sentido cuando isPlaced es true.
//
// Devuelve true -> este cliente PUBLICA el estado de la maquina (y el peer lo
// aplica). Devuelve false -> este cliente APLICA lo que reciba del peer.
inline bool prodIsLocalAuthority(bool isHost, bool isPlaced, bool placedByLocal) {
    if (!isPlaced) return isHost;   // baked -> manda el host (comportamiento previo)
    return placedByLocal;           // placed -> manda quien la coloco
}

} // namespace coop

#endif // COOP_PROD_AUTHORITY_H
