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

El script principal es `reaper/Smart Align Post - PHASE BATCH.lua`.

El primer item seleccionado define el MASTER TRACK. El MASTER TRACK puede contener muchos items, normalmente uno por escena/plano.

### ANALYZE SELECTION

Analiza únicamente los SOURCE items actualmente seleccionados.

Cada SOURCE item se empareja con el item del MASTER TRACK que tenga el mayor solapamiento temporal.

### ANALYZE PROJECT

Recorre todos los items de los SOURCE TRACKS declarados por la selección y construye una relación por escena:

`MASTER item (escena N) → SOURCE item`

La referencia no es el track MASTER como un bloque continuo: la unidad de análisis es el **item del MASTER correspondiente a la escena**.

Esto permite que una misma pista de lavalier tenga, a lo largo del proyecto, correcciones diferentes para cada escena.

### APPLY ALL

Los resultados con confidence suficiente se aplican manteniendo `D_POSITION`:

- STATIC: corrección mediante `D_STARTOFFS`;
- DYNAMIC: render sample-domain a un WAV corregido y reemplazo del source del take;
- `D_POSITION` del item no se modifica;
- todos los cambios quedan dentro de un único Undo de REAPER.

Los resultados por debajo del umbral de confianza se omiten para revisión manual.

## Residual BY ITEM

La arquitectura posterior al batch utiliza `reaper/Smart Align Post - RESIDUAL BY ITEM.lua`.

Este segundo paso vuelve a medir el residuo después de la alineación principal.

La relación sigue siendo por escena:

`MASTER item de la escena → SOURCE item correspondiente`

Un SOURCE item nunca hereda una corrección global de su track.

El escaneo residual:

1. identifica el MASTER scene item con mayor solapamiento;
2. mide el delay residual del SOURCE;
3. guarda en el item los datos de MASTER GUID, escena, residuo y confidence;
4. sólo considera elegible una corrección si el residuo supera 2 samples y la confidence es >= 0.72;
5. no corrige automáticamente items que cruzan escenas, tienen cobertura insuficiente o presentan un residuo DYNAMIC;
6. para un residuo estático confiable, inserta automáticamente `Smart Align Post` como **Take FX** en ese item y carga el valor cuantificado.

Los items que ya están alineados no reciben ningún FX.

## VST3 residual

El VST3 de esta arquitectura es ahora un procesador **mono de Take FX**, pensado para el residuo de un único item.

Su función no es volver a descubrir la alineación completa. El valor residual llega cuantificado desde Smart Align Post.

El parámetro `Residual Samples` representa la corrección necesaria en muestras y admite corrección fraccional. El procesador utiliza una línea de retardo con interpolación de 4 puntos y reporta su latencia al host para que REAPER pueda compensarla.

La interfaz muestra el valor residual y permite activar/desactivar la corrección. El valor no debe decidirse a oído: debe provenir del análisis de Smart Align.

La consolidación física del resultado en un WAV y su verificación final son pasos posteriores del flujo REAPER; el VST no modifica directamente el archivo fuente.

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

REAPER se ocupa de descubrir items, construir pares MASTER/SOURCE por escena, lanzar el análisis y aplicar el mapa temporal.

El ejecutable SmartAlignPostPrototype funciona como interfaz offline del motor para la integración actual.

El flujo VST residual reutiliza una corrección ya cuantificada por el motor y no sustituye al alineamiento batch.

## Estado

Esta rama desarrolla la siguiente etapa sobre el PHASE ALIGNMENT CORE:

- relación MASTER/SOURCE explícita por item de escena;
- residual analysis posterior al batch;
- inserción automática del Take FX sólo en items elegibles;
- VST3 mono para corrección residual sample-accurate.

La consolidación final y la verificación post-render siguen siendo obligatorias antes de considerar un residuo corregido como definitivo.

## Licencia

El código propio del proyecto está publicado bajo MIT.
