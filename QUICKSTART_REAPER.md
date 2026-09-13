# Smart Align Post — entrega 1

## Objetivo de esta entrega

Esta primera entrega sirve para poner el proyecto público en GitHub, comprobar que compila en CI y validar el motor DSP antes de construir la integración offline con REAPER.

**No intentes todavía usarla como Auto-Align Post terminado.** El VST3 incluido es un shell/pass-through y el motor DSP se prueba de forma independiente.

## Entrega 1 — GitHub

1. Crear un repositorio público llamado `SmartAlignPost`.
2. Subir el contenido de esta carpeta (no hace falta subir el ZIP).
3. Mantener `extern/vst3sdk` como submodule; no subir el SDK entero al repositorio.
4. Verificar que GitHub Actions termina en verde.

El SDK oficial de Steinberg se obtiene con submodules y utiliza CMake para compilar VST3. 

## Entrega 2 — prueba del motor DSP

En tu PC:

```bash
cmake -S . -B build-dsp -DSAP_BUILD_VST3=OFF -DSAP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp --config Release
ctest --test-dir build-dsp --output-on-failure
```

Resultado esperado:

```text
100% tests passed
```

Esta prueba usa una señal sintética con un retardo conocido y verifica que STATIC recupere el desplazamiento y que DYNAMIC produzca una curva.

## Entrega 3 — VST3 en REAPER

Cuando CI compile correctamente:

1. Construir el VST3 con el SDK.
2. Instalar el `.vst3` en la carpeta VST3 del sistema.
3. En REAPER: `Preferences → Plug-ins → VST → Re-scan`.
4. Buscar `Smart Align Post`.
5. Insertarlo en una pista de prueba.
6. Confirmar que el plugin carga y pasa audio sin alterar.

Todavía no esperes el botón final `CALCULATE/APPLY` ni la selección automática de items.

## Entrega 4 — material real

Cuando lleguemos al puente offline, usaremos primero una toma real sencilla:

```text
BOOM.wav
LAV1.wav
```

Después:

```text
BOOM.wav
LAV1.wav
LAV2.wav
```

Idealmente una escena donde el actor se mueva respecto al boom. Eso permitirá comparar STATIC contra DYNAMIC y ajustar el algoritmo con material real.

## Regla importante

No subas grabaciones de producción al repositorio público. El código y los tests deben permanecer libres de material con derechos, voces identificables o información de rodaje.
