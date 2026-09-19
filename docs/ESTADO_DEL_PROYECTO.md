# Estado del proyecto — PHASE ALIGNMENT CORE

## Rediseño actual

El motor anterior de DYNAMIC acumulaba tracking por ventanas, landmarks, refinamiento waveform, recaptura GCC, smoothing y consolidación en Lua.

Esa arquitectura fue retirada.

## Núcleo vigente

El motor actual usa:

- anchors guiados por energía;
- GCC-PHAT como estimador primario del delay;
- refinamiento sub-muestra por waveform;
- comparación entre ambas estimaciones;
- trayectoria dinámica sólo cuando existe evidencia suficiente;
- normalización a tiempo de proyecto para D_PLAYRATE.

## Integración vigente

La integración REAPER se concentra en reaper/Smart Align Post - PHASE BATCH.lua.

El flujo admite selección puntual o análisis de todos los items de los SOURCE tracks declarados.

## Criterio de desarrollo

Primero se valida la alineación temporal/fásica con tests deterministas.

Después se optimiza el flujo batch.

No se añade complejidad al algoritmo mientras la precisión de la medición primaria no esté demostrada.
