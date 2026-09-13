# Smart Align Post

**Herramienta open source para alineación temporal de micrófonos de sonido directo, con motor de análisis offline y futura implementación VST3 multiplataforma.**

> Proyecto independiente inspirado en flujos de trabajo profesionales como Sound Radix Auto-Align Post. No es una copia ni está afiliado a Sound Radix.

## ¿Qué problema busca resolver?

En postproducción de sonido directo es habitual combinar un **boom** con uno o más **corbateros (lavalier)**. La distancia entre los micrófonos y la fuente cambia durante la actuación: cuando el intérprete se mueve, gira la cabeza o cambia su posición, también puede cambiar la relación temporal entre las grabaciones.

Smart Align Post busca analizar esas relaciones de forma **offline y precisa**, para mejorar la coherencia temporal y de fase entre los micrófonos.

Ejemplo de uso previsto:

```text
BOOM / MASTER
LAV 1
LAV 2
LAV 3...
```

## Modos de alineación

### STATIC — alineación fija

El sistema analiza el material y calcula un único desplazamiento temporal para cada micrófono fuente.

```text
BOOM  ─────────────────────────────
LAV1  ──── +4,3 ms ────────────────
LAV2  ──── +6,1 ms ────────────────
```

La corrección permanece fija durante todo el material analizado.

### DYNAMIC — alineación dinámica

El sistema analiza el material por ventanas y calcula una curva de desplazamiento temporal. La corrección puede cambiar suavemente cuando existe evidencia suficiente de que la relación acústica entre los micrófonos ha cambiado.

El diseño incluye mecanismos para evitar movimientos erráticos:

- estimación de confianza;
- rechazo de valores poco confiables;
- suavizado temporal;
- limitación de la velocidad de cambio;
- conservación del último valor confiable.

## Flujo de trabajo previsto

La experiencia final que buscamos es:

```text
Seleccionar grabaciones
        ↓
Elegir MASTER
        ↓
Elegir STATIC o DYNAMIC
        ↓
CALCULAR
        ↓
Revisar resultados
        ↓
APLICAR
        ↓
Escuchar
        ↓
Deshacer si es necesario
```

El objetivo es que el análisis sea **offline**, no un proceso de alineación en tiempo real. Esto permite dedicar más tiempo de cálculo a encontrar una solución robusta.

## Estado actual del proyecto

### Ya implementado

- Motor independiente en C++17.
- Estimación de desplazamiento mediante correlación normalizada.
- Limitación del rango de búsqueda.
- Cálculo de confianza.
- Puerta de confianza.
- Suavizado temporal para modo dinámico.
- Limitación de la velocidad de variación.
- Resultados STATIC y DYNAMIC.
- Estructura inicial de VST3.
- Proyecto CMake multiplataforma.
- Pruebas automatizadas del motor DSP.
- Preparación para integración con REAPER.

### Todavía en desarrollo

La primera entrega pública **no es todavía un Auto-Align completo**.

El VST3 actual funciona como una base de integración del motor, pero todavía no implementa el flujo final de selección de items del timeline, análisis offline completo y aplicación de las correcciones.

Esto es intencional. Un VST3 estándar recibe audio y parámetros del host, pero no dispone de una API portátil para acceder directamente a los items seleccionados en el timeline de cada DAW. Por eso el proyecto separa:

1. **Motor de alineación** — independiente del DAW.
2. **VST3** — interfaz y procesamiento estándar multiplataforma.
3. **Integración con cada DAW** — capa específica cuando sea necesaria.

## Arquitectura

```text
                 SMART ALIGN ENGINE
                        │
        ┌───────────────┼────────────────┐
        │               │                │
       VST3           REAPER          otros DAW
        │               │                │
      audio          integración      integración
      + UI            específica       específica
```

El motor DSP no debe depender de REAPER. Esto permite que el proyecto pueda crecer hacia otros DAW y, eventualmente, otras plataformas de plugin.

## Estructura del repositorio

```text
SmartAlignPost/
├── src/                 # Motor DSP y código VST3
├── tests/               # Pruebas automatizadas del motor
├── reaper/              # Prototipos de integración con REAPER
├── docs/                # Diseño y documentación técnica
├── .github/workflows/   # Compilación y pruebas automáticas
├── CMakeLists.txt
├── LICENSE
├── THIRD_PARTY_LICENSES.md
└── CONTRIBUTING.md
```

## Compilación

El proyecto utiliza CMake.

El SDK oficial de VST3 de Steinberg es una dependencia externa y no se copia dentro de este repositorio. El SDK oficial documenta compilación mediante CMake para Windows, macOS y Linux.

Para el desarrollo local, la configuración del proyecto descargará/obtendrá el SDK externo cuando sea necesario.

### Pruebas del motor DSP

Las pruebas del motor no necesitan REAPER ni una instalación del plugin. Su objetivo es comprobar primero que el algoritmo matemático funciona correctamente.

```text
cmake -S . -B build-dsp -DSAP_BUILD_VST3=OFF -DSAP_BUILD_TESTS=ON
cmake --build build-dsp --config Release
ctest --test-dir build-dsp --output-on-failure
```

## REAPER

REAPER es el primer DAW de referencia para el desarrollo porque permite probar rápidamente el flujo de trabajo de producción de sonido.

La integración prevista será:

1. seleccionar los items de audio;
2. indicar cuál es el MASTER;
3. identificar las fuentes;
4. analizar los archivos completos offline;
5. calcular la corrección STATIC o DYNAMIC;
6. mostrar resultados y confianza;
7. aplicar la corrección sin destruir los originales;
8. permitir escuchar y deshacer.

## Relación con Auto-Align Post

Auto-Align Post de Sound Radix es una referencia conceptual importante para este proyecto, especialmente por sus conceptos de alineación estática y dinámica.

Smart Align Post es un proyecto independiente y open source. No utiliza código propietario de Sound Radix.

Las futuras funciones avanzadas se estudiarán de forma independiente, incluyendo la posibilidad de investigar correcciones espectrales de fase después de que la alineación temporal básica sea sólida.

## Licencia

El código propio del proyecto está publicado bajo **MIT**. Consultar `LICENSE`.

El SDK de VST3 de Steinberg es una dependencia independiente y conserva sus propias condiciones de licencia. Consultar `THIRD_PARTY_LICENSES.md` y la documentación oficial del SDK.

## Estado del desarrollo

**Versión:** 0.1 — base experimental pública.

El proyecto está en desarrollo activo. Las primeras versiones priorizan la validación del algoritmo y la calidad del análisis antes de añadir funciones avanzadas de interfaz.

## Contribuciones

Las sugerencias, pruebas con material real y reportes de errores son bienvenidos. Consultar `CONTRIBUTING.md`.
