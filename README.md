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

## Compatibility (v0.0.2)

Measured on an AYN Odin 3 (Snapdragon 8 Elite) and reported by testers on other phones (Snapdragon 8 Elite, 8 Gen 3 and 888, Dimensity with Mali). Results change with the GPU and driver. Unreal Engine games start at 25% resolution (configurable in Settings).

| Game | State | Speed | Notes |
|---|---|---|---|
| Dead Cells | Playable | 60 FPS |  |
| Animal Well | Playable | 60 FPS |  |
| Blasphemous | Playable | 60 FPS |  |
| Blasphemous 2 | Playable | 55–61 FPS | Doesn't boot on Mali G720 |
| Greak: Memories of Azur | Playable | 60 FPS | Audio stutter reported |
| The Binding of Isaac: Repentance | Playable | 60 FPS | Closes at start on some devices |
| Monster Boy and the Cursed Kingdom | Playable | ~60 FPS | Closes at start on some devices |
| The Messenger | Playable | 60 FPS |  |
| MARVEL Cosmic Invasion | Playable | 60 FPS |  |
| Dreaming Sarah | Playable | 60 FPS | Adreno 660 fixed in v0.0.2 |
| Jitsu Squad | Playable | 60 FPS |  |
| New Joe & Mac: Caveman Ninja | Playable | 60 FPS |  |
| R-Type Delta: HD Boosted | Playable | 60 FPS |  |
| Skautfold: Into the Fray | Playable | 60 FPS |  |
| Mighty Morphin Power Rangers | Playable | 60 FPS |  |
| Astérix & Obélix: Slap Them All! 2 | Playable | 60 FPS |  |
| Atari 50: The Anniversary Celebration | Playable | 50–60 FPS |  |
| BloodRayne: Betrayal | Playable | 40–60 FPS |  |
| Cult of the Lamb | Playable | – |  |
| Tomb Raider I-III Remastered | Playable | – |  |
| Mewgenics | Playable | – | Cursor jumped with the stick; likely fixed in v0.0.2 |
| Astérix & Obélix XXL2 | Playable | 20–60 FPS | Texture glitches, a distant building flickers |
| Astérix & Obélix XXL3 | Playable | 60 FPS | Graphical glitches |
| Minecraft | Playable | – | Low FPS in some scenes |
| Rogue Prince of Persia | Playable | ~30 FPS | Occasional bad frames |
| Prince of Persia: The Lost Crown | Playable | 20–30 FPS |  |
| Unicorn Overlord | Playable | <30 FPS | Crashes on some devices |
| God of War: Sons of Sparta | Playable | <35 FPS | Slower in battles on Mali |
| TriFox | Playable | <35 FPS | Texture glitches |
| Ghost Song | Playable | <30 FPS |  |
| Lost Epic | Playable | <30 FPS |  |
| Graveyard Keeper II | Playable | – | Low FPS in places |
| Born of Bread | Slow | 7–28 FPS | Unreal Engine; floor missing in some rooms |
| Unbound: Worlds Apart | Slow | 10–20 FPS | Unreal Engine 4 |
| Earth Defense Force: World Brothers 2 | Slow | 14–16 FPS | Closes at start on some devices |
| Gori: Cuddly Carnage | Slow | ~8 FPS | Closes at start on some devices |
| Stray | Slow | 4–7 FPS |  |
| Little Nightmares Enhanced Edition | Slow | 4–6 FPS | Ray tracing skipped |
| Castlevania: Belmont's Curse | Slow | ~4 FPS | Very long loads |
| DNF Duel | In game, with problems | – | Crashes in game |
| Little Nightmares III | In game, with problems | – | Low FPS, black flashes |
| Crown Wars: The Black Prince | In game, with problems | – | Black screen, then closes |
| 63 Days | In game, with problems | <20 FPS | Closes after the loading screen |
| Valkyrie Elysium | In game, with problems | 5–16 FPS | Closes on a loading screen |
| Avatar: The Last Airbender – Quest for Balance | In game, with problems | 20–60 FPS | Freezes in the title cutscene |
| Void Terrarium | In game, with problems | – | Freezes in game |
| Sackboy: A Big Adventure | In game, with problems | 60 FPS | Endless loading |
| Hollow Knight | Doesn't boot | – | Mali G720 |
| The Adventures of Elliot | Doesn't boot | – |  |
| Astro Bot | Doesn't boot | – |  |
| Bloodborne | Doesn't boot | – |  |
| Demon's Souls | Doesn't boot | – |  |
| Ghost of Yōtei | Doesn't boot | – |  |
| Marvel's Wolverine | Doesn't boot | – |  |
| Flashback 2 | Doesn't boot | – |  |
| Mega Man Star Force Legacy Collection | Doesn't boot | – |  |
| Sonic Origins Plus | Doesn't boot | – |  |
| Earthion | Doesn't boot | – |  |
| Squirrel with a Gun | Doesn't boot | – |  |
| Big Helmet Heroes | Doesn't boot | – |  |

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
