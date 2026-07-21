// MoneyReconcile.h - reconciliacion de la cartera COMPARTIDA por delta (pura,
// sin dependencias de juego/Win32).
//
// El dinero del jugador en Kenshi es UNA sola cartera por faccion
// (Faction::factionOwnerships::money), compartida por todo el squad y por AMBOS
// jugadores en co-op (los dos controlan la misma faccion del save compartido).
// Ver la cadena confirmada por RE en EngineWorld.cpp (readPlayerWallet).
//
// Un pool compartido con DOS escritores no se puede sincronizar con valores
// absolutos: si los dos gastan antes de recibirse (1000 -> host 750, join 900),
// el ultimo valor absoluto que llega pisa al otro y se pierde dinero. La forma
// correcta es replicar el DELTA de cada lado y APLICARLO (sumar) en el peer, de
// modo que 1000 - 250 (host) - 100 (join) = 650 converge en ambos. El canal
// PKT_MONEY es reliable+ordered (ENet), asi que cada delta se entrega una sola
// vez: no hay doble aplicacion y no hace falta safety-resend (a diferencia del
// canal absoluto per-tab anterior).
//
// MoneyState.known es la "linea base": el ultimo valor de la cartera que este
// cliente considera ya sincronizado con el peer. Se siembra en el primer sample
// (sin emitir) y avanza tanto cuando publicamos un delta local como cuando
// aplicamos un delta remoto (echo-guard: un delta aplicado nunca se re-detecta
// como cambio local y se reenvia). Tested en src/prototest/main.cpp
// (testMoneyReconcile); usado por Replicator::publishMoney/applyMoney.

#ifndef COOP_MONEY_RECONCILE_H
#define COOP_MONEY_RECONCILE_H

namespace coop {

struct MoneyState {
    int  known;  // baseline: ultima cartera compartida conocida por este cliente
    bool seeded; // false hasta el primer sample (evita emitir un delta espurio)
    MoneyState() : known(0), seeded(false) {}
};

// Detecta el delta LOCAL a publicar comparando la cartera actual con la linea
// base. Siembra en el primer sample (devuelve false, no se emite nada). En
// samples posteriores devuelve true y rellena *outDelta solo si la cartera
// cambio por accion local; avanza la base para no reenviar. outDelta puede ser
// negativo (gasto) o positivo (ingreso), nunca 0 cuando devuelve true.
inline bool moneyLocalDelta(MoneyState& s, int cur, int* outDelta) {
    if (!s.seeded) { s.seeded = true; s.known = cur; return false; }
    if (cur == s.known) return false;
    if (outDelta) *outDelta = cur - s.known;
    s.known = cur;
    return true;
}

// Aplica un delta REMOTO sobre la cartera local: avanza la base y devuelve el
// nuevo valor que hay que escribir en la cartera. La cartera de Kenshi nunca es
// negativa, asi que el resultado se clampa a >=0 AQUI (unico sitio con el clamp)
// y la base avanza por el delta REALMENTE aplicado (want - cur), NO por el delta
// teorico. Si avanzasemos la base por el delta completo mientras la cartera se
// clampa a 0, 'known' quedaria negativo y divergido de la cartera real (0): el
// siguiente moneyLocalDelta veria cur(0) - known(<0) = delta positivo espurio y
// publicaria dinero de la nada al peer (desync permanente). En el caso no-clamp
// (want>=0) want-cur == delta, identico al comportamiento anterior, por lo que
// un cambio local aun sin publicar sigue sobreviviendo como (cur - known) y se
// publica en el siguiente moneyLocalDelta.
inline int moneyApplyDelta(MoneyState& s, int cur, int delta) {
    int want = cur + delta;      // valor teorico tras el delta remoto
    if (want < 0) want = 0;      // la cartera compartida nunca baja de 0
    s.known += (want - cur);     // la base sigue al valor REALMENTE escrito
    return want;                 // nuevo valor de la cartera local (ya clampado)
}

} // namespace coop

#endif // COOP_MONEY_RECONCILE_H
