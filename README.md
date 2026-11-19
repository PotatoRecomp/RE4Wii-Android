# Resident Evil 4 Wii for Android

<div align="justify">

An unofficial port of *Resident Evil 4 Wii edition* (NTSC-U, `RB4E08`) to Android (arm64) and Windows.

The game's PowerPC code is recompiled to C++ ahead of time on a PC and built into a native app. A runtime stands in for the Wii's system software, and a GX renderer draws on Vulkan. Nothing is emulated on the phone.

Port made by xu@shiinobu and coded by Claude Opus 5.5. It is built on [WiiCompiled](https://github.com/patchzyy/Wiicompiled) (GPLv3), through our Sengoku Basara 2 Heroes port (see [Credits](#credits)).

</div>

> [!NOTE]
> This repository contains no game assets and no game code. You need a dump of your own legally acquired disc.
>
> This is an unofficial fan project, not affiliated with or endorsed by Capcom. Resident Evil, its characters and its logo belong to Capcom.

<div align="justify">

**Contents:** [Status](#status) · [Features](#features) · [Getting started](#getting-started) · [Coming from the Beta](#coming-from-the-beta) · [Saves](#saves) · [Reporting a problem](#reporting-a-problem) · [Building](#building) · [Credits](#credits) · [License](#license)

## Status

**1.0** (November 2026), the first release. Download it from [Releases](../../releases/latest). The Beta has ended.

The main game plays on Android and Windows with sound, saving and loading, and Separate Ways runs too. Not every chapter has been played through on every phone yet. In the app, **What's New** lists what each version changed.

> [!WARNING]
> **Android 10:** phones with an Adreno 5xx GPU (Snapdragon 625, 630, 660, 835 and similar) on Android 10 are not supported: their Vulkan driver stops the game on its first screens. Android 11 or newer is recommended.
>
> **PowerVR GPUs** (some Samsung, OPPO and Helio phones, e.g. Samsung Galaxy A04 with the PowerVR GE8320): the game plays, but the inventory background shows only in its top-right quarter. Not recommended; use the Low-End or Ultra Low preset if you try.

### Known issues

- With 60 FPS on, smoke, fire, blood and the menus still move at 30.
- On some Mali phones with 60 FPS Full, inventory items can appear and disappear.
- Adreno 610: keep Render Resolution at 1× or lower in the castle.
- Going back from the main menu to the title movie can freeze on some phones (seen on a moto g57).
- On some PowerVR phones (Samsung A04), the inventory background shows only in the top-right quarter.
- Adreno 5xx phones on Android 10 are not supported: their Vulkan driver stops the game on its first screens.
- Play As is experimental: some moves and screens of the other characters still borrow Leon's, Krauser shows as Leon in the cutscene of Ada's fight with him, and Play As stays off in Separate Ways.

### Tested phones

Results from our own phones and from testers' logs. The game itself runs at 30 fps (60 FPS adds pictures in between); "Gameplay" is what the phone held in play. "Version" is the build it was last tested on; a phone not retested may run better on the current one.

**Adreno and Mali**

| Phone | Chip / GPU | Android | Gameplay | Version |
|---|---|---|---|---|
| Poco F4 | Snapdragon 870 / Adreno 650 | | 60 fps with 60 FPS on | Beta 1 |
| Redmi Note 12 Pro | Snapdragon 732G / Adreno 618 | 13 | 30 fps in the village | Final Beta |
| moto g57 power | Snapdragon (SM6435) / Adreno | 16 | 30 fps at Render Resolution 2× | Final Beta |
| realme 5 Pro (RMX1971) | Snapdragon 712 / Adreno 616 | 11 | about 30 fps at 1.5× with Turnip | Beta 14 |
| OPPO CPH2375 | Snapdragon 680 / Adreno 610 | 13 | 30 fps in the village at 0.75× | Final Beta |
| OPPO CPH1933 | Snapdragon 665 / Adreno 610 | 11 | 28–30 fps in Separate Ways; keep Render Resolution at 1× or lower in the castle | Beta 14 |
| Infinix X6882 | Helio G99 / Mali-G57 MC2 | 14 | 30 fps at 1.5×, dips to 15 in busy village moments | Beta 14 |
| Infinix X6876 | Mali-G615 | | plays; its crashes in chapters 1-2 and 1-4 are fixed | Beta 8 |
| OPPO CPH2591 | Helio G85 / Mali-G52 MC2 | 15 | 30 fps with the malisx2 driver | Final Beta |
| Xiaomi 2409BRN2CY | Helio G81-class / Mali-G52 MC2 | 16 | 30 fps | Beta 14 |
| itel A666LN | Unisoc T606 / Mali-G57 MP1 | | plays; the crash on a fresh install is fixed in the Final Beta | Beta 14 |

**PowerVR** (many MediaTek and Unisoc phones): less tested. Use the **Ultra Low** preset, and send **Share Log** when something goes wrong.

| Phone | Chip / GPU | Android | Result | Version |
|---|---|---|---|---|
| Samsung Galaxy A04 | Helio P35 / PowerVR GE8320 | | plays; the inventory background shows only in the top-right quarter | Beta 13 |

Your phone is not here, or runs differently? Send a Share Log (see [Reporting a problem](#reporting-a-problem)), working or not: these reports are what fills this list.

## Features

| Area | What it has |
|---|---|
| **In-game menu** | Every setting is in the game's menu (Back, or the gear): Display, Graphics, Performance, Controls, Save Data, Cheats, Textures and System, with Restart for the settings that need it. English and Indonesian. |
| **Display** | Fill Screen without black bars, Camera Distance, Crosshair, Laser Color, Minimap, Speedrun Mode (IGT, RTA and chapter times). |
| **Performance** | 60 FPS (Off, Lite or Full), Frameskip, Low-End and Ultra Low presets, Display Resolution, Draw Distance, Disable Effects / Screen Filters / Shadows / Fog, GX Thread, Skip Boot. Shaders are prepared before play, so new areas stutter less. A device profile picks the settings on first start. |
| **Graphics** | Renderer choice (Vulkan, WebGPU, OpenGL ES experimental), Render Resolution 0.5× to 4×, upscaling, custom GPU drivers. |
| **Controls** | Touch controls in the Classic Controller layout (aims with the laser) with a layout editor that moves, resizes or hides each control, touch camera (Locked or the game's own), Aim Sensitivity apart from Camera Sensitivity, Auto Run, gyro aim, scope and binocular zoom on the D-pad, gamepads with remappable buttons, and a rebindable keyboard on Windows. |
| **Cheats** | Unlocks, Player, Weapons and Items, and Assist: Infinite Ammo, Item Stack, Largest Attache Case, One-Shot Kills, Auto Aim, Steady Aim, Auto QTE, Special Weapons at the merchant, and Play As (Ada, HUNK, Krauser or Wesker in Leon's story, in cutscenes and radio calls too, experimental; Replay Cutscene). |
| **Textures** | Texture mods (.png or .dds, Dolphin names), texture dumping. |
| **Tools** | Performance overlay, auto save of each room, save backup and import, Share Log, Share Shader Cache, What's New, updates from this repository's Releases (Settings > System > Auto Update). |

## Getting started

**Requirements:** Android 10 or newer, arm64, Vulkan recommended.

1. Download `re4wii-<version>-android-arm64.apk` from [Releases](../../releases/latest) and install it. Do not use APKs from anywhere else.
2. Open the app and allow file access when asked.
3. Pick your own dump of the game (`.iso` or `.rvz`, Wii NTSC-U `RB4E08`). Unpack it once (about 4 GB), or play it straight from the file. After that the icon goes straight into the game.
4. The first start prepares the game and its shaders, which takes a little while. Later starts are quick.
5. Change the settings any time from the game's menu (Back, or the gear).

When a newer release is out, the app offers it at start (or turn on Auto Update). Installing a new version over the old one keeps your save and settings.

## Coming from the Beta

The Beta no longer opens. The release is its own app (`Resident Evil 4 Wii`), next to RE4 Beta:

1. Install the release and choose your game file again.
2. Bring your save: Settings > Save Data > **Import**, and pick `RE4Wii-Beta/Saves/Auto/latest.zip`. The Beta copied your latest save there by itself each time you played. Made a **Back Up** yourself? Pick that file from `RE4Wii-Beta/Saves` instead.
3. Then uninstall RE4 Beta. The `RE4Wii-Beta` folder stays after the uninstall; delete it once your save is imported.

## Saves

The save lives in the app's own storage, and each room is also auto-saved to `RE4Wii/Saves/Auto`. A new install or Clear Data brings the save back from there. In **Settings > Save Data**:

- **Back Up** writes `re4-save-DATE.zip` to `RE4Wii/Saves`. Copy it to a PC or a cloud drive to keep it safe.
- **Import** replaces the save on the phone. It accepts:
  - a `.zip` made by Back Up,
  - a bare save file,
  - a `data.bin` copied to SD from a Wii (Data Management) or exported by Dolphin (Export Wii Save).
- **Undo Last Import** brings back the save from before the last import.

## Reporting a problem

In the app, open Settings > System > **Share Log**. It puts the logs of the runs you pick (the crashed one first) and your device info in one zip. Send it with what you were doing to either:

- a new [GitHub issue](../../issues), or
- the Telegram channel: [t.me/indonesiarecompcomunity](https://t.me/indonesiarecompcomunity), or
- the Discord server: [discord.gg/2NswhkFeyD](https://discord.gg/2NswhkFeyD)

A screen recording helps with anything you can see on screen. The logs are also on the phone, one folder per game start: `RE4Wii/Logs/<date and time>/`.

## Building

For developers. Requires Windows, .NET 8, llvm-mingw in `third_party/llvm-mingw` (Windows build) and Android NDK 28.2 (fetched by `tools/setup-android.ps1`).

```powershell
# Extract your disc so that assets/re4/sys/main.dol exists, then:
tools\setup-android.ps1               # toolchain and dependencies (third_party/)
dotnet build translator\src\Translator.Cli -c Release
tools\b-translate.ps1                 # PowerPC -> C++ (generated/)
tools\b-win.ps1 -Mode configure       # once
tools\b-win.ps1 -Mode cycle           # Windows build and run
tools\b-android.ps1 -Mode configure   # once
tools\b-android.ps1 -Mode build       # libmain.so
tools\b-android.ps1 -Mode stage
cd app
..\third_party\gradle\bin\gradle.bat assembleRelease "-Pre4VersionName=1.0" "-Pre4VersionCode=19" --offline
# a Beta: -Pre4Beta "-Pre4BetaNumber=N" ["-Pre4BetaLabel=Final Beta"] [-Pre4BetaTrack=test2]
# In PowerShell quote each -P...=x.y, or the value splits.
```

Optional: `tools\b-pgo.ps1` for a profile-guided (PGO + ThinLTO) Android build. On Windows, guest memory is at `0x100000000000` plus the Wii address.

| Path | Contents |
|---|---|
| `app/` | Android app (Gradle module): launcher, in-game menu, settings, touch controls |
| `runtime/` | Host runtime and Wii SDK HLE (OS, GX, AX, NAND/DVD, VI), cheats and game tweaks, performance overlay |
| `aurora/` | GX renderer on WebGPU (Dawn), native Vulkan and OpenGL ES |
| `translator/` | PowerPC-to-C++ translator (C#/.NET 8) |
| `config/re4/` | Game configuration: `recomp.yml` |
| `tools/` | Build, run and analysis scripts |

How the pieces fit together: [docs/architecture.md](docs/architecture.md).

## Credits

This project is built on WiiCompiled. It came here through our Sengoku Basara 2 Heroes port and was changed from November 2026 to run *Resident Evil 4 Wii edition*.

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors: the static recompiler and runtime underneath this port.
- **b2hwii-android**: our Sengoku Basara 2 Heroes port, the direct base of this code.
- **WitherBlitz**: parts of the runtime and the Android app (GPLv3).
- **datfooldive**: the touch button design.
- **[Jude (@YlPorts)](https://github.com/YlPorts)**: the Android draw-count and Mali GPU changes.
- **[KartPad](https://github.com/chrissotraidis/kartpad)**: WiiCompiled on Android, whose approach and game-file import steps this port follows.
- **[aurora](https://github.com/encounter/aurora)** by Luke Street: the GX rendering and windowing backend.
- **[Dawn](https://dawn.googlesource.com/dawn)**: Google's WebGPU implementation, used by aurora for Vulkan and Direct3D.
- **[CubeEvil4-Android](https://github.com/Andiweli/CubeEvil4-Android)** by Andiweli (EvilCube): methods and ideas this port follows: the minimap above all, and Play As and the 60 FPS frame blending. Our code is written separately.
- **[adonis-singh/re4](https://github.com/adonis-singh/re4)** (GameCube decompilation) and **emoose/re4-research**: function and structure names, used as reference only. No code was copied.
- **Dolphin Emulator** wiki: notes on how the game uses the GPU.
- The Beta testers, whose logs, videos and shader caches made 1.0 what it is.

Full third-party license details: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## License

GNU General Public License v3.0, see [LICENSE](LICENSE). This covers the port's own code only. The game, its code and its assets belong to Capcom and are not part of this repository.

</div>
