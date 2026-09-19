# Smart Align Post — REAPER Quickstart

## Preparación

1. Colocá SmartAlignPostPrototype.exe en la misma carpeta que el script Lua.
2. En REAPER, seleccioná un item de audio perteneciente al track BOOM.
3. Seleccioná además al menos un item perteneciente a cada track de corbateros que quieras declarar como SOURCE.

El primer item seleccionado define el MASTER TRACK.

## Procesar una selección

Abrí reaper/Smart Align Post - PHASE BATCH.lua y presioná ANALYZE SELECTION.

Se analizarán solamente los SOURCE items seleccionados. El MASTER correspondiente se obtiene automáticamente por solapamiento temporal.

## Procesar todo un proyecto

Seleccioná un item del MASTER y al menos un item de cada SOURCE track.

Presioná ANALYZE PROJECT.

El script recorrerá todos los items existentes en esos SOURCE tracks y buscará para cada uno el item del MASTER con mayor solapamiento.

Esto permite procesar muchas escenas/tomas de una sola vez.

## Interpretación

Cada resultado muestra STATIC o DYNAMIC, cantidad de anchors, confidence y delay al inicio, medio y final.

DYNAMIC solamente aparece cuando el motor detecta una variación temporal que justifica una trayectoria.

## Aplicar

Presioná APPLY ALL.

Los casos por debajo de la confidence mínima no se aplican.

Todos los cambios se registran en un único Undo.

## Importante

El algoritmo está diseñado para medir el delay acústico entre MASTER y SOURCE. No utiliza la energía como sustituto de la medición temporal: la energía sólo selecciona regiones donde tiene sentido hacer la medición de fase.
