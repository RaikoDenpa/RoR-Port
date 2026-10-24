<div align="center">

# Rule of Rose — PC Port

**An unofficial, fan-made native PC port of *Rule of Rose* (PlayStation 2, 2006)**

</div>

---

> [!IMPORTANT]
> **This repository contains no game assets.** No textures, models, audio, video, or original game code are distributed here.
> You must supply data extracted from **your own legally purchased copy** of *Rule of Rose* for PlayStation 2.
> This project is not affiliated with, endorsed by, or connected to Sony Interactive Entertainment, Punchline, Atlus, 505 Games, or any rights holder.

---

## About

*Rule of Rose* is a 2006 psychological survival horror game developed by Punchline for the PlayStation 2. It never received an official re-release, and original copies are now expensive and hard to find.

This project rebuilds the game to run natively on modern PCs. It is not an emulator. The goal is a faithful experience with optional quality-of-life improvements that you can toggle off for a purist playthrough.

## Project Status

| Area | Status |
|---|---|
| Boot / Menus | ✅ Working |

Legend: ✅ Working · 🟡 Partial · ❌ Broken · ⬜ Untested

## Requirements

### Minimum
- **OS:** Windows 10 64-bit / Linux (glibc 2.31+)
- **CPU:** Any x86-64 CPU from the last ~10 years
- **GPU:** OpenGL 3.3 or Vulkan 1.1 capable
- **RAM:** 4 GB
- **Storage:** ~5 GB (game data + port)

### You will also need
- An original PS2 disc of *Rule of Rose* (NTSC-U, NTSC-J, or PAL)
- A way to make an ISO from your disc (see [Dumping Your Disc](#dumping-your-disc))

## Installation

### 1. Dumping Your Disc

Create an ISO image from **your own** disc using a PC DVD drive:

- **Windows:** [ImgBurn](https://www.imgburn.com/) → *Create image file from disc*
- **Linux:**
  ```bash
  dd if=/dev/sr0 of=rule_of_rose.iso bs=2048 status=progress
  ```


> Fill in the serial numbers from your verified dumps.

### 2. Download the Port

There is no download available yet

### 3. Extract Game Data

Run the extractor and point it at your ISO:

```bash
# Windows
rorpc-extract.exe path\to\rule_of_rose.iso

# Linux
./rorpc-extract path/to/rule_of_rose.iso
```

This verifies the ISO's checksum and extracts the assets into a `data/` folder next to the executable. Unsupported or modified ISOs are rejected.

### 4. Play

None Yet


Binaries are written to `build/bin/`.

See the [issue tracker](https://github.com/raikodenpa/RoR-Port/issues) for the full list.

## FAQ

**Can I just download the game here?**
No. This repo has no game files, and requests for ROMs or ISOs will be closed. Dump your own disc.

**Is this an emulator?**
No. It's a native port that runs the game logic directly on your PC, using your own game data.

- Bug reports: open an issue with your region, OS, GPU, and a log file (`logs/latest.log`)
- Pull requests: one fix or feature per PR, and follow the existing code style
- **Never commit copyrighted game assets** or code copied from the original binary. PRs that include them will be rejected.

## Legal

This is a non-commercial fan preservation project. *Rule of Rose* and all related names, characters, and assets are the property of their respective owners. This project distributes no copyrighted material and requires users to own a legitimate copy of the game.

If you are a rights holder and have concerns, please contact `Raiko@mostima.dev` before taking further action.

## Credits

- **Original game:** Punchline, published by Sony Computer Entertainment, Atlus, and 505 Games
- **Port development:** [@Raikodenpa](https://github.com/raikodenpa)
- **Libraries:** [SDL2](https://www.libsdl.org/) and others listed in [THIRD_PARTY.md](THIRD_PARTY.md)

---

<div align="center">
<sub>Made with care for a game that deserved better.</sub>
</div>
