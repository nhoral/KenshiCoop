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

// Aplica un delta REMOTO sobre la cartera local: avanza la base por el delta
// (para que el proximo moneyLocalDelta no lo confunda con un cambio local) y
// devuelve el nuevo valor que hay que escribir en la cartera. Un cambio local
// aun sin publicar sobrevive: queda como (cur - known) tras esta llamada y se
// publica en el siguiente moneyLocalDelta.
inline int moneyApplyDelta(MoneyState& s, int cur, int delta) {
    s.known += delta;   // la base sigue al valor compartido
    return cur + delta; // nuevo valor de la cartera local
}

} // namespace coop

#endif // COOP_MONEY_RECONCILE_H
