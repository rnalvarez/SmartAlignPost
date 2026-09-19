# Smart Align Post

Herramienta open source para alineación temporal de micrófonos de sonido directo, pensada para postproducción de ficción y documental en REAPER.

## Objetivo

El objetivo principal del proyecto es una alineación temporal que produzca una mezcla acústicamente coherente entre MASTER (boom) y SOURCE (corbatero/lavalier).

La velocidad y el flujo de trabajo importan, pero no sustituyen la precisión de la alineación.

## Nuevo núcleo: PHASE ALIGNMENT CORE

La arquitectura anterior acumulaba varias capas de tracking dinámico, landmarks, recapturas y refinamientos locales. Esa aproximación produjo demasiado coste computacional y no dio una trayectoria suficientemente estable en tomas largas.

El nuevo núcleo utiliza una cadena mucho más directa:

1. selección de regiones acústicamente informativas mediante energía;
2. GCC-PHAT para estimar el desplazamiento temporal a partir de la fase del espectro cruzado;
3. refinamiento sub-muestra mediante correlación de forma de onda;
4. comparación independiente entre ambas mediciones;
5. aceptación únicamente de puntos con suficiente confianza y continuidad temporal.

GCC-PHAT es la medición primaria. La correlación waveform no reemplaza la medición de fase: funciona como comprobación independiente para evitar aceptar picos espurios.

## STATIC, DYNAMIC y AUTO

STATIC calcula un único delay robusto a partir de varios anchors independientes.

DYNAMIC construye una trayectoria delay(t) a partir de anchors temporales de alta energía. Entre anchors no se generan mediciones innecesarias: la curva es interpolada posteriormente en la integración con REAPER.

AUTO es el modo previsto para trabajo de película. Primero obtiene una solución STATIC robusta. Sólo ejecuta tracking dinámico completo cuando mediciones independientes muestran una variación real del delay.

La intención es que la mayoría de los planos sencillos no paguen el coste del análisis dinámico.

## D_PLAYRATE

El prototipo de REAPER normaliza MASTER y SOURCE al mismo eje temporal de proyecto antes del análisis. Por lo tanto un SOURCE que tenga, por ejemplo, D_PLAYRATE=0.999 no se compara a velocidad nativa: su deriva temporal también forma parte de la medición.

Esto permite distinguir un delay acústico fijo de una diferencia temporal acumulativa.

## Flujo de trabajo en REAPER

El script principal es reaper/Smart Align Post - PHASE BATCH.lua.

El primer item seleccionado define el MASTER TRACK.

Los demás tracks representados por los items seleccionados se consideran SOURCE TRACKS.

### ANALYZE SELECTION

Analiza únicamente los SOURCE items actualmente seleccionados.

### ANALYZE PROJECT

Recorre todos los items de los SOURCE TRACKS declarados por la selección y busca automáticamente el item MASTER con mayor solapamiento temporal.

Esto permite seleccionar los tracks de boom/corbateros y procesar el proyecto completo sin tener que ir plano por plano.

### APPLY ALL

Los resultados con confidence suficiente se aplican de manera no destructiva:

- STATIC: corrección mediante D_STARTOFFS;
- DYNAMIC: time-warp mediante stretch markers;
- D_POSITION del item no se modifica;
- todos los cambios quedan dentro de un único Undo de REAPER.

Los resultados por debajo del umbral de confianza se omiten para revisión manual.

## Qué valida el motor

Las pruebas del DSP están orientadas a la necesidad real del proyecto:

- delay entero;
- delay fraccional;
- delay variable en el tiempo;
- presencia de ruido;
- detección AUTO de un plano estable;
- reproducción SOURCE con D_PLAYRATE diferente.

El criterio no es solamente producir una curva o una confidence alta: las pruebas verifican que el delay recuperado coincida con el delay conocido dentro de una tolerancia explícita.

## Arquitectura

El motor DSP está separado de la integración con REAPER.

REAPER se ocupa de descubrir items, construir pares MASTER/SOURCE, lanzar el análisis y aplicar el mapa temporal.

El ejecutable SmartAlignPostPrototype funciona como interfaz offline del motor para la integración actual.

La futura integración VST3 puede reutilizar el mismo motor, pero no forma parte del flujo offline de REAPER.

## Estado

Esta rama inaugura el rediseño PHASE ALIGNMENT CORE.

El objetivo de esta etapa es validar primero la calidad de la medición temporal/fásica con material sintético y real. El flujo batch se construye alrededor de ese núcleo, no al revés.

## Licencia

El código propio del proyecto está publicado bajo MIT.
