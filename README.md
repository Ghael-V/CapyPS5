# CapyPS5

**English** · [Español](README.es.md)

The first PlayStation 5 emulator for Android, built on KytyPS5 and the FEX x86-64 JIT.

**Website:** <https://capyps5.org> · **Discord:** <https://discord.com/invite/PEE7Q5TVM5>

> **Alpha (v0.0.1).** Few games work and many don't run at full speed. This release is for testing
> and reporting problems. The app tells you when a new version is out.

Source code: <https://git.capyps5.org/CapyPS5/CapyPS5> (mirror: <https://github.com/Ghael-V/CapyPS5>).
Every release has a tag (`v0.0.1`, …) with the exact code it was built from.

## Requirements

- Android 12 or later, arm64.
- A GPU with Vulkan 1.3. **Only tested on Qualcomm Adreno 830 (AYN Odin 3)** with the system driver.
  On other Adreno GPUs (for example the 750 in the Galaxy S24 Ultra) use a Turnip driver, and on Mali
  try PanVK: the app downloads them in **Settings → Graphics → Download drivers**.
- 12 GB of RAM or more recommended.
- Your own PS5 game dumps (a folder with `eboot.bin` and `sce_sys/`).

**CapyPS5 includes no games, BIOS, firmware or Sony keys, and needs none of them.** Only use games
you dumped from your own console. CapyPS5 is not affiliated with Sony Interactive Entertainment;
PlayStation and PS5 are trademarks of their respective owners.

## Usage

1. Install the APK and grant all-files access (the app asks for it on first start).
2. Copy each game as a folder into `CapyPS5/` on internal storage, or add your folders in
   **Settings → Game folders**.
3. Pick a game and press **Play**. **Ⓨ** or **⋯** opens that game's own options.
4. In game, swipe from the left edge, press the back button, the controller's home button or
   Select + Start to open the menu.

If a game closes because of an error, the library shows why when you return and can save the log
(to `CapyPS5/logs/`). Attach it when you report the problem.

## Compatibility (v0.0.1, AYN Odin 3)

Unreal Engine games start at 25% resolution (configurable in Settings).

| Game | State |
|---|---|
| Dead Cells | Playable, 60 FPS |
| Blasphemous | Playable, 60 FPS |
| Animal Well | Playable, 60 FPS |
| Greak: Memories of Azur | Playable, 60 FPS |
| The Binding of Isaac: Repentance | Playable, 60 FPS |
| Monster Boy and the Cursed Kingdom | Playable, ~60 FPS |
| Rogue Prince of Persia | Playable, ~30 FPS, occasional bad frames |
| Unbound: Worlds Apart | Playable, ~20 FPS |
| MARVEL Cosmic Invasion | Works |
| Born of Bread | Playable at 25%, 16-28 FPS; the floor of some rooms is missing |
| Earth Defense Force | ~14-16 FPS |
| Guri | ~8 FPS |
| Astérix & Obélix XXL2 | ~20 FPS, a distant building flickers |
| Stray | Boots, ~4-7 FPS |
| Castlevania: Belmont's Curse | Boots, ~4 FPS, very long loads |
| Little Nightmares Enhanced Edition | Boots and plays, ~4-6 FPS (no ray tracing) |
| Sonic Origins Plus | Closes at start |
| Sackboy: A Big Adventure | Endless loading |

## Building

See `tools/build_android.ps1` (NDK 28.2, CMake 3.31, Ninja). `tools/sign_release.ps1` builds and
signs the release APK.

## Credits

- **Ghael-V**, author of CapyPS5.
- **Madelein**, for the CapyPS5 logo.
- KytyPS5 and the original Kyty by InoriRus, and the FEX project, which CapyPS5 is built on.

## License

CapyPS5 is free software under the **GNU General Public License v2** (`GPL-2.0-only`, `LICENSE`).
Every source file names its copyright holders and license in an SPDX header: CapyPS5's own code is
© 2026 CapyPS5 Project, and derived files keep the credit of KytyPS5 and of the original Kyty by
InoriRus (MIT, notice in `LICENSES/Kyty-MIT.txt`). Anyone who redistributes or forks it must keep
those headers and offer the source code. Third-party components and their licenses are listed in
`docs/THIRD_PARTY_NOTICES.md`.
