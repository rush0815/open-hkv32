# TODO

Offene Ideen und Punkte für später. Erledigtes wird gestrichen oder entfernt.

## Kalibrierung

- [ ] **Referenzfühler im Kalibrierlauf glätten**, sobald der Raumfühler in einem Gehäuse sitzt.
  Der Referenzfühler hängt frei im Raum und reagiert schnell (vor allem ein SHT40). Der Raumfühler im
  Gehäuse ist dagegen träge. Kurze Luftzüge (Tür, Vorbeilaufen) sieht die Referenz sofort und
  voll, den internen Fühler erreichen sie kaum. Das landet als Restfehler in der Ausgleichsrechnung.
  Abhilfe: Die Referenz vor dem Fit mit einer kurzen Zeitkonstante tiefpassfiltern, etwa 2–5 min,
  ähnlich träge wie die Luft im Gehäuse. Die Zeitkonstante ließe sich auch wie τ über ein kleines
  Raster mitbestimmen.
  Solange der Raumfühler offen hängt, bringt das nichts, weil beide Fühler gleich schnell sind.
