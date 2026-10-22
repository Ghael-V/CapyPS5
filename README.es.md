# CapyPS5

[English](README.md) · **Español**

El primer emulador de PS5 para Android, basado en KytyPS5 y en el JIT x86-64 de FEX.

**Web:** <https://capyps5.org/es/> · **Discord:** <https://discord.com/invite/PEE7Q5TVM5>

> **Alpha (v0.0.1).** Pocos juegos funcionan y muchos no van a velocidad completa. Es una versión
> para probar y reportar fallos. La app avisa cuando hay una versión nueva.

Código fuente: <https://git.capyps5.org/CapyPS5/CapyPS5> (mirror:
<https://github.com/Ghael-V/CapyPS5>). Cada release tiene su etiqueta (`v0.0.1`, …) con el código
exacto con el que se compiló.

## Requisitos

- Android 12 o superior, arm64.
- GPU con Vulkan 1.3. **Solo está probado en Qualcomm Adreno 830 (AYN Odin 3)** con el driver del
  sistema. En otras Adreno (por ejemplo la 750 del Galaxy S24 Ultra) usa un driver Turnip, y en Mali
  prueba PanVK: la app los descarga en **Ajustes → Gráficos → Descargar drivers**.
- 12 GB de RAM o más recomendados.
- Tus propios volcados de juegos de PS5 (carpeta con `eboot.bin` y `sce_sys/`).

**CapyPS5 no incluye juegos, BIOS, firmware ni claves de Sony, y no hace falta ninguno de ellos.**
Usa solo juegos que hayas volcado tú de tu propia consola. CapyPS5 no está afiliado a Sony
Interactive Entertainment; PlayStation y PS5 son marcas de sus respectivos propietarios.

## Uso

1. Instala la APK y concede el permiso de acceso a todos los archivos (la app lo pide al abrirse).
2. Copia cada juego como una carpeta dentro de `CapyPS5/` en el almacenamiento interno, o añade
   tus carpetas en **Ajustes → Carpetas de juegos**.
3. Elige un juego y pulsa **Jugar**. Con **Ⓨ** o **⋯** cambias las opciones de ese juego.
4. En partida, desliza desde el borde izquierdo, pulsa el botón de atrás, el botón central del mando
   o Select + Start para abrir el menú.

Si un juego se cierra por un error, al volver a la biblioteca verás el motivo y podrás guardar el
log (en `CapyPS5/logs/`). Adjúntalo al reportar el fallo.

## Compatibilidad (v0.0.1, AYN Odin 3)

Los juegos de Unreal Engine usan por defecto el 25 % de resolución (configurable en Ajustes).

| Juego | Estado |
|---|---|
| Dead Cells | Jugable, 60 FPS |
| Blasphemous | Jugable, 60 FPS |
| Animal Well | Jugable, 60 FPS |
| Greak: Memories of Azur | Jugable, 60 FPS |
| The Binding of Isaac: Repentance | Jugable, 60 FPS |
| Monster Boy and the Cursed Kingdom | Jugable, ~60 FPS |
| Rogue Prince of Persia | Jugable, ~30 FPS, fotogramas sueltos mal |
| Unbound: Worlds Apart | Jugable, ~20 FPS |
| MARVEL Cosmic Invasion | Funciona |
| Born of Bread | Jugable al 25 %, 16-28 FPS; el suelo de algunas zonas desaparece |
| Earth Defense Force | ~14-16 FPS |
| Guri | ~8 FPS |
| Astérix y Obélix XXL2 | ~20 FPS, un edificio lejano parpadea |
| Stray | Arranca, ~4-7 FPS |
| Castlevania: Belmont's Curse | Arranca, ~4 FPS, cargas muy largas |
| Little Nightmares Enhanced Edition | Arranca y se juega, ~4-6 FPS (sin trazado de rayos) |
| Sonic Origins Plus | Se cierra al arrancar |
| Sackboy: A Big Adventure | Carga infinita |

## Compilar

Ver `tools/build_android.ps1` (NDK 28.2, CMake 3.31, Ninja). `tools/sign_release.ps1` genera y
firma la APK de release.

## Créditos

- **Ghael-V**, autor de CapyPS5.
- **Madelein**, por el logo de CapyPS5.
- KytyPS5 y el Kyty original de InoriRus, y el proyecto FEX, sobre los que se construye CapyPS5.

## Licencia

CapyPS5 es software libre bajo la **GNU General Public License v2** (`GPL-2.0-only`, `LICENSE`).
Cada archivo de código indica en su cabecera SPDX sus titulares de copyright y su licencia: el código
propio es © 2026 CapyPS5 Project, y el derivado conserva el crédito de KytyPS5 y del Kyty original de
InoriRus (MIT, aviso en `LICENSES/Kyty-MIT.txt`). Quien redistribuya o haga un fork debe conservar
esas cabeceras y ofrecer el código fuente. Los componentes de terceros y sus licencias están en
`docs/THIRD_PARTY_NOTICES.md`.
