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

**1.2** (November 2026). Download it from [Releases](../../releases/latest). 1.0 was the first release (the Beta has ended), 1.1 brought smoother 30 and 60 FPS and new controls, and 1.2 fixes the 1.1 reports: gamepads, mouse and keyboard, PowerVR and Adreno 7xx phones, and Play As crashes.

The main game plays on Android and Windows with sound, saving and loading, and Separate Ways runs too. Not every chapter has been played through on every phone yet. In the app, **What's New** lists what each version changed.

</div>

> [!WARNING]
> **Android 10:** phones with an Adreno 5xx GPU (Snapdragon 625, 630, 660, 835 and similar) on Android 10 are not supported: their Vulkan driver stops the game on its first screens. Android 11 or newer is recommended.
>
> **PowerVR GPUs** (many MediaTek and Unisoc phones, e.g. Samsung Galaxy A04 with the PowerVR GE8320): the game plays, but older PowerVR Rogue GPUs stay at 10–25 fps. Since 1.2 there is no "Preparing shaders" wait at the start: shaders compile while you play, so a room can miss a few objects for a moment the first time. Use the Low-End or Ultra Low preset.

<div align="justify">

### Known issues

- With 60 FPS on, smoke, fire, blood and the menus still move at 30.
- PowerVR: the first visit to a room can miss a few objects for a moment while its shaders compile.
- Renderer OpenGL ES: the first minute or two can stutter a little while the shaders are prepared.
- The merchant after the chainsaw sisters is heavy on some Mali phones.
- Going back from the main menu to the title movie can freeze on some phones.
- Infinix X656 (PowerVR GE8320, Android 10): the screen stays black with sound after the start.
- Adreno 5xx phones on Android 10 are not supported: their Vulkan driver stops the game on its first screens.
- Play As is experimental: some cutscenes show Leon's model, and Play As stays off in Separate Ways.

### Tested phones

Results from our own phones and from testers' logs. The game itself runs at 30 fps (60 FPS adds pictures in between); "Gameplay" is what the phone held in play, at the Render Resolution shown. "Version" is the build it was last tested on; a phone not retested may run better on the current one.

**Adreno**

| Phone | Chip / GPU | Android | Gameplay | Version |
|---|---|---|---|---|
| vivo I2501 | Snapdragon (SM8850) / Adreno 840 | 17 | plays with 60 FPS on | 1.0 |
| vivo I2401 | Snapdragon 8 Elite / Adreno 830 | 17 | 30 fps at 4× | 1.0 |
| Xiaomi 25053PC47G | Snapdragon 8s Gen 4 / Adreno 825 | 16 | 30 fps at 1.5× | Beta 14 |
| Xiaomi 23127PN0CG | Snapdragon 8 Gen 3 / Adreno 750 | 16 | 30 fps at 2×, 60 FPS Full on | 1.1 |
| Xiaomi 24069PC21G | Snapdragon 8s Gen 3 / Adreno 735 | 16 | 30 fps at 2×, 60 FPS Full on; Keys/Treasures items fixed in 1.2 | 1.1 |
| moto g67 power 5G | Snapdragon (SM7435) / Adreno 710 | 16 | plays; its grey screen at start is fixed in 1.1 | 1.0 |
| moto g57 power | Snapdragon (SM6435) / Adreno 710 | 16 | 30 fps at 2×; its grey screen at start with V-Sync on is handled in 1.2 | 1.2 test |
| Xiaomi 25098RA98G | Snapdragon (SM6475) / Adreno 710 | 16 | 30 fps at 1.5× to 2× | 1.1 |
| ASUS ROG Phone 5 (ASUS_I005D) | Snapdragon 888 / Adreno 660 | 13 | 30 fps at 1.125× | 1.0 |
| Poco F4 | Snapdragon 870 / Adreno 650 | | 60 fps with 60 FPS on | Beta 1 |
| Xiaomi 22101320G | Snapdragon 778G / Adreno 642L | 14 | 30 fps at 2×, 60 FPS Full on | 1.1 |
| Redmi Note 12 Pro | Snapdragon 732G / Adreno 618 | 13 | 30 fps in the village | Final Beta |
| Redmi Note 9 Pro | Snapdragon 720G / Adreno 618 | 12 | 30 fps at 1.5× with a custom driver | 1.1 |
| OPPO CPH2159 | Snapdragon 720G / Adreno 618 | 13 | 30 fps at 1.125× | 1.1 |
| realme 5 Pro (RMX1971) | Snapdragon 712 / Adreno 616 | 11 | about 30 fps at 1.5× with Turnip | Beta 14 |
| OPPO CPH2375 | Snapdragon 680 / Adreno 610 | 13 | 30 fps in the village at 0.75× | Final Beta |
| OPPO CPH2565 | Snapdragon 680 / Adreno 610 | 15 | 29–30 fps at 1× | 1.1 |
| OPPO CPH2631 | Snapdragon 680 / Adreno 610 | 16 | about 30 fps at 1× | 1.1 |
| OPPO CPH1933 | Snapdragon 665 / Adreno 610 | 11 | 28–30 fps at 1×; keep Render Resolution at 1× or lower in the castle | 1.1 |

**Mali**

| Phone | Chip / GPU | Android | Gameplay | Version |
|---|---|---|---|---|
| Xiaomi 2412DPC0AG | Dimensity 8400 / Mali-G720 MC7 | 16 | plays at 3× with 60 FPS Full on | 1.1 |
| Xiaomi 2306EPN60G | Dimensity 8200 / Mali-G610 MC6 | 16 | 30 fps at 1.5×, 60 FPS Full on | 1.1 |
| Infinix X6871 | Dimensity 8200 / Mali-G610 MC6 | 16 | 30 fps at 1× to 1.5× | 1.1 |
| TECNO LJ8k | Dimensity 7300 / Mali-G615 MC2 | 16 | 30 fps at 1.125× | 1.0 |
| Infinix X6876 | Dimensity 7300 / Mali-G615 MC2 | 16 | plays; its crashes in chapters 1-2 and 1-4 are fixed | Beta 8 |
| samsung SM-M156B | Dimensity 6100+ / Mali-G57 MC2 | 15 | 30 fps at 1.5× | 1.0 |
| TECNO LH8n | Dimensity 6080 / Mali-G57 MC2 | 14 | 30 fps at 1.5× with the malisx2 driver | 1.0 |
| itel P661N | Dimensity 6080 / Mali-G57 MC2 | 13 | about 30 fps at 1×; 60 FPS Full drops (Adaptive fits better) | 1.1 |
| Infinix X6882 | Helio G99 / Mali-G57 MC2 | 14 | 30 fps at 1.5×, dips to 15 in busy village moments | Beta 14 |
| Infinix X6885 | Helio G99 / Mali-G57 MC2 | 16 | plays at 2× | 1.0 |
| Xiaomi 22071219CG | Helio G99 / Mali-G57 MC2 | 14 | 30 fps at 2× | 1.0 |
| Xiaomi 2312FPCA6G | Helio G99 / Mali-G57 MC2 | 16 | 29 fps at 1× | Beta 8 |
| nubia Z2465N | Unisoc T820 / Mali-G57 | 16 | 30 fps at 2×, 60 FPS Full on | 1.1 |
| VILLAON V6752L | Unisoc (UMS9230E) / Mali-G57 | 15 | 30 fps at 1× | 1.0 |
| itel A666LN | Unisoc T606 / Mali-G57 MP1 | 13 | about 30 fps at 1.2× | Beta 14 |
| OPPO CPH1819 | Helio P60 / Mali-G72 MP3 | 10 | 29 fps at 1× | 1.0 |
| OPPO CPH2591 | Helio G85 / Mali-G52 MC2 | 15 | 30 fps with the malisx2 driver | 1.0 |
| OPPO CPH2579 | Helio G85 / Mali-G52 MC2 | 15 | 30 fps in play at 2×; the slow title menu is fixed in 1.2 | 1.1 |
| Infinix X692 | Helio G85 / Mali-G52 MC2 | 10 | about 30 fps at 1.125× | 1.1 |
| Xiaomi 2409BRN2CY | Helio G81-class / Mali-G52 MC2 | 16 | 30 fps | Beta 14 |

**PowerVR** (many MediaTek and Unisoc phones): less tested. Use the **Ultra Low** preset, and send **Share Log** when something goes wrong.

| Phone | Chip / GPU | Android | Result | Version |
|---|---|---|---|---|
| moto g55 5G | Dimensity 7025 / PowerVR BXM-8-256 | 16 | plays; the crash at every start after the first is fixed in 1.1, no shader wait at start since 1.2 | 1.1 |
| OPPO CPH2209 | Helio P95 / PowerVR GM9446 | 12 | 29 fps at 0.5× | Beta 8 |
| Samsung Galaxy A04 | Helio P35 / PowerVR GE8320 | 14 | about 20 fps at 1.6×; the inventory background is fixed in 1.1 | Beta 13 |
| vivo V2149 | Helio P22 / PowerVR GE8320 | 11 | about 24 fps at 0.5× to 1× | Beta 14 |
| Infinix X656 | Helio (MT6765) / PowerVR GE8320 | 10 | black screen with sound after the start (open) | 1.1 |

Your phone is not here, or runs differently? Send a Share Log (see [Reporting a problem](#reporting-a-problem)), working or not: these reports are what fills this list.

## Features

| Area | What it has |
|---|---|
| **In-game menu** | Every setting is in the game's menu (Back, or the gear): Display, Graphics, Performance, Controls, Save Data, Cheats, Textures and System, with Restart for the settings that need it. English and Indonesian. |
| **Display** | Fill Screen without black bars, Aspect Ratio (4:3 Full Screen too), Camera Distance, Crosshair, Laser Color, Minimap, Speedrun Mode (IGT, RTA and chapter times). |
| **Performance** | 60 FPS (Off, Lite, Adaptive or Full), Even Frame Pacing, Frameskip, Low-End and Ultra Low presets, Display Resolution, Dynamic Resolution, Draw Distance, Disable Effects / Screen Filters / Shadows / Fog, GX Thread, Sustained Performance and Thermal Control, Skip Boot. Shaders are prepared before play (on PowerVR while you play, each room's own first), so new areas stutter less. A device profile picks the settings on first start. |
| **Graphics** | Renderer choice (Vulkan, WebGPU, OpenGL ES experimental), Render Resolution 0.5× to 4×, upscaling, custom GPU drivers. |
| **Controls** | Touch controls in the Classic Controller layout (aims with the laser) with a layout editor that moves, resizes or hides each control, Tap for QTE, Knife by Swipe, touch camera (Locked or the game's own), Aim Sensitivity apart from Camera Sensitivity, Toggle Aim, Toggle Run, Auto Run, gyro aim, scope and binocular zoom on the D-pad. Gamepads with remappable buttons, Alternate Controls, Swap Sticks, Invert Camera Y and Check Gamepad. Mouse and keyboard on Android and Windows: mouse aim, rebindable Keyboard Keys (mouse buttons too), a Reload key, wheel zoom for the scope and binoculars, and Controller Settings (F10). |
| **Cheats** | Unlocks, Player, Weapons and Items, and Assist: Infinite Health, Infinite Ammo, Item Stack, Largest Attache Case, One-Shot Kills, Auto Aim, Steady Aim, Auto QTE, Special Weapons at the merchant, and Play As (Ada, HUNK, Krauser or Wesker in Leon's story, in cutscenes and radio calls too, experimental; Replay Cutscene). |
| **Textures** | Texture mods (.png or .dds, Dolphin names), texture dumping. |
| **Tools** | Performance overlay, Auto Save of each room (off by default), save backup and import, Share Log, Share Shader Cache, What's New, updates from this repository's Releases (Settings > System > Auto Update). |

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

The save lives in the app's own storage. With **Auto Save** on (Settings > Save Data, off by default), each room is also saved to `RE4Wii/Saves/Auto`, and a new install or Clear Data brings the save back from there. In **Settings > Save Data**:

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
- the Discord server: [discord.gg/wvXZ6QZewF](https://discord.gg/wvXZ6QZewF)

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
..\third_party\gradle\bin\gradle.bat assembleRelease "-Pre4VersionName=1.2" "-Pre4VersionCode=21" --offline
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
- **[CubeEvil4-Android](https://github.com/Andiweli/CubeEvil4-Android)** by Andiweli (EvilCube): methods and ideas this port follows: the minimap above all, and Play As, the 60 FPS frame blending and the controls. Our code is written separately.
- **[adonis-singh/re4](https://github.com/adonis-singh/re4)** (GameCube decompilation) and **emoose/re4-research**: function and structure names, used as reference only. No code was copied.
- **Dolphin Emulator** wiki: notes on how the game uses the GPU.
- The testers, whose logs, videos and shader caches made 1.0, 1.1 and 1.2 what they are.

Full third-party license details: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## License

GNU General Public License v3.0, see [LICENSE](LICENSE). This covers the port's own code only. The game, its code and its assets belong to Capcom and are not part of this repository.

</div>
