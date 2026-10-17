# CapyPS5

Emulador experimental de PS5 para Android, basado en Kyty y en el JIT x86-64 de FEX.

> **Pre-alpha (v0.0.1).** Muy pocos juegos funcionan y casi ninguno a velocidad completa. Es una
> versión para probar y reportar fallos, no para jugar.

## Requisitos

- Android 12 o superior, arm64.
- GPU Qualcomm Adreno reciente con Vulkan 1.3 (probado en Adreno 830, AYN Odin 3).
- 12 GB de RAM o más recomendados.
- Tus propios volcados de juegos de PS5 (carpeta con `eboot.bin` y `sce_sys/`). CapyPS5 no
  incluye juegos, firmware ni claves de Sony.

## Uso

1. Instala la APK y concede el permiso de acceso a todos los archivos (la app lo pide al abrirse).
2. Copia cada juego como una carpeta dentro de `CapyPS5/` en el almacenamiento interno, o añade
   tus carpetas en **Ajustes → Carpetas de juegos**.
3. Elige un juego y pulsa **Jugar**. Con **Ⓨ** o **⋯** cambias las opciones de ese juego.
4. En partida, desliza desde el borde izquierdo (o pulsa el botón central del mando, o
   Select + Start) para abrir el menú.

Si un juego se cierra por un error, al volver a la biblioteca verás el motivo y podrás guardar el
log (en `CapyPS5/logs/`). Adjúntalo al reportar el fallo.

## Compatibilidad (v0.0.1, AYN Odin 3)

| Juego | Estado |
|---|---|
| Dead Cells | Jugable, 60 FPS |
| Blasphemous | Jugable, 60 FPS |
| Rogue Prince of Persia | Jugable, ~30 FPS, fotogramas sueltos mal |
| Unbound: Worlds Apart | Jugable, ~20 FPS |
| MARVEL Cosmic Invasion | Funciona |
| Astérix y Obélix XXL2 | ~20 FPS, un edificio lejano parpadea |
| Stray | Arranca, ~4-7 FPS |
| Castlevania: Belmont's Curse | Arranca, ~4 FPS, cargas muy largas |
| Little Nightmares | No arranca (shaders con llamadas por tabla) |

## Compilar

Ver `tools/build_android.ps1` (NDK 28.2, CMake 3.31, Ninja). `tools/sign_release.ps1` genera y
firma la APK de release.

## Licencia

CapyPS5 es software libre bajo la **GNU General Public License v2** (`LICENSE`). Los componentes de
terceros y sus licencias están en `docs/THIRD_PARTY_NOTICES.md`.
