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

## Compatibilidad (v0.0.2)

Medido en una AYN Odin 3 (Snapdragon 8 Elite) y según los testers en otros móviles (Snapdragon 8 Elite, 8 Gen 3 y 888, Dimensity con Mali). Los resultados cambian con la GPU y el driver. Los juegos de Unreal Engine empiezan al 25 % de resolución (se cambia en Ajustes).

| Juego | Estado | Velocidad | Notas |
|---|---|---|---|
| Dead Cells | Jugable | 60 FPS |  |
| Animal Well | Jugable | 60 FPS |  |
| Blasphemous | Jugable | 60 FPS |  |
| Blasphemous 2 | Jugable | 55–61 FPS | No arranca en Mali G720 |
| Greak: Memories of Azur | Jugable | 60 FPS | Se ha visto audio a tirones |
| The Binding of Isaac: Repentance | Jugable | 60 FPS | Se cierra al arrancar en algunos dispositivos |
| Monster Boy and the Cursed Kingdom | Jugable | ~60 FPS | Se cierra al arrancar en algunos dispositivos |
| The Messenger | Jugable | 60 FPS |  |
| MARVEL Cosmic Invasion | Jugable | 60 FPS |  |
| Dreaming Sarah | Jugable | 60 FPS | Adreno 660 arreglado en v0.0.2 |
| Jitsu Squad | Jugable | 60 FPS |  |
| New Joe & Mac: Caveman Ninja | Jugable | 60 FPS |  |
| R-Type Delta: HD Boosted | Jugable | 60 FPS |  |
| Skautfold: Into the Fray | Jugable | 60 FPS |  |
| Mighty Morphin Power Rangers | Jugable | 60 FPS |  |
| Astérix & Obélix: Slap Them All! 2 | Jugable | 60 FPS |  |
| Atari 50: The Anniversary Celebration | Jugable | 50–60 FPS |  |
| BloodRayne: Betrayal | Jugable | 40–60 FPS |  |
| Cult of the Lamb | Jugable | – |  |
| Tomb Raider I-III Remastered | Jugable | – |  |
| Mewgenics | Jugable | – | El cursor saltaba con el stick; probablemente arreglado en v0.0.2 |
| Astérix & Obélix XXL2 | Jugable | 20–60 FPS | Fallos de texturas, un edificio lejano parpadea |
| Astérix & Obélix XXL3 | Jugable | 60 FPS | Fallos gráficos |
| Minecraft | Jugable | – | FPS bajos en algunas escenas |
| Rogue Prince of Persia | Jugable | ~30 FPS | Algún fotograma suelto mal |
| Prince of Persia: The Lost Crown | Jugable | 20–30 FPS |  |
| Unicorn Overlord | Jugable | <30 FPS | Se cierra en algunos dispositivos |
| God of War: Sons of Sparta | Jugable | <35 FPS | Más lento en combate en Mali |
| TriFox | Jugable | <35 FPS | Fallos de texturas |
| Ghost Song | Jugable | <30 FPS |  |
| Lost Epic | Jugable | <30 FPS |  |
| Graveyard Keeper II | Jugable | – | FPS bajos en algunos tramos |
| Born of Bread | Lento | 7–28 FPS | Unreal Engine; falta el suelo en algunas salas |
| Unbound: Worlds Apart | Lento | 10–20 FPS | Unreal Engine 4 |
| Earth Defense Force: World Brothers 2 | Lento | 14–16 FPS | Se cierra al arrancar en algunos dispositivos |
| Gori: Cuddly Carnage | Lento | ~8 FPS | Se cierra al arrancar en algunos dispositivos |
| Stray | Lento | 4–7 FPS |  |
| Little Nightmares Enhanced Edition | Lento | 4–6 FPS | Sin trazado de rayos |
| Castlevania: Belmont's Curse | Lento | ~4 FPS | Cargas muy largas |
| DNF Duel | En partida, con problemas | – | Se cierra en partida |
| Little Nightmares III | En partida, con problemas | – | FPS bajos, destellos negros |
| Crown Wars: The Black Prince | En partida, con problemas | – | Pantalla negra y se cierra |
| 63 Days | En partida, con problemas | <20 FPS | Se cierra tras la pantalla de carga |
| Valkyrie Elysium | En partida, con problemas | 5–16 FPS | Se cierra en una pantalla de carga |
| Avatar: The Last Airbender – Quest for Balance | En partida, con problemas | 20–60 FPS | Se congela en la cinemática del título |
| Void Terrarium | En partida, con problemas | – | Se congela en partida |
| Sackboy: A Big Adventure | En partida, con problemas | 60 FPS | Carga infinita |
| Hollow Knight | No arranca | – | Mali G720 |
| The Adventures of Elliot | No arranca | – |  |
| Astro Bot | No arranca | – |  |
| Bloodborne | No arranca | – |  |
| Demon's Souls | No arranca | – |  |
| Ghost of Yōtei | No arranca | – |  |
| Marvel's Wolverine | No arranca | – |  |
| Flashback 2 | No arranca | – |  |
| Mega Man Star Force Legacy Collection | No arranca | – |  |
| Sonic Origins Plus | No arranca | – |  |
| Earthion | No arranca | – |  |
| Squirrel with a Gun | No arranca | – |  |
| Big Helmet Heroes | No arranca | – |  |

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
