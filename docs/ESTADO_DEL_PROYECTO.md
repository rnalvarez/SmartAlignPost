# Estado del proyecto

## Versión actual

**0.1 — base experimental pública**

## Objetivo de esta etapa

Comprobar que el motor matemático de Smart Align Post puede estimar de forma fiable la diferencia temporal entre una señal MASTER y una señal SOURCE antes de construir la interfaz final y la integración profunda con REAPER.

## Qué está funcionando

- Motor C++ independiente del DAW.
- Estimación de delay mediante correlación normalizada.
- Modo STATIC.
- Modo DYNAMIC.
- Confianza de la estimación.
- Suavizado temporal.
- Limitación de cambios bruscos.
- Pruebas automatizadas básicas.
- Estructura inicial VST3.

## Qué todavía no debe considerarse terminado

- Selección de items desde el timeline de REAPER.
- Selección visual de MASTER y SOURCES dentro del plugin.
- Análisis completo de archivos de audio desde la interfaz del plugin.
- Aplicación offline de una curva de delay sobre el audio.
- Preview antes de aplicar.
- Aplicación no destructiva integrada con el DAW.
- Undo específico de la operación del plugin.
- Interfaz gráfica final.

## Próximo objetivo

### V1 funcional — STATIC

La primera meta funcional será conseguir:

1. seleccionar BOOM y LAV en REAPER;
2. indicar BOOM como MASTER;
3. ejecutar CALCULAR;
4. obtener el delay y la confianza;
5. aplicar una corrección fija;
6. escuchar el resultado;
7. poder deshacerlo.

Una vez que STATIC sea estable, se desarrollará DYNAMIC sobre la misma arquitectura.

## Principio de desarrollo

No se incorporarán funciones avanzadas solamente para aumentar la cantidad de características. Cada etapa deberá probarse con señales controladas y después con material real de producción de sonido.

La prioridad es:

**precisión → estabilidad → flujo de trabajo → interfaz → funciones avanzadas.**
