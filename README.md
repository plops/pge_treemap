[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/plops/pge_treemap)

# PGE Treemap

A fast, interactive disk space visualizer built in modern C++23 and powered by **[olcPixelGameEngine3 (PGE3)](https://github.com/OneLoneCoder/olcPixelGameEngine3)**.

`pge_treemap` recursively scans your filesystem in a background worker thread and builds a live hierarchical Cushion Treemap (similar to *WinDirStat*, *KDirStat*, and *SequoiaView*) to help you spot disk space hogs instantly.

![Screenshot of treemap showing sizes of files in a folder](source0/img/pge_treemap_screenshot.png)
---

## Features

- **Live Asynchronous Scanning:** The filesystem scan runs in a separate thread and continuously updates the visual layout without blocking the UI or render loop.
- **Cushion Treemap Rendering:** Implements adaptive cushion shading (pseudo-3D lighting and beveling) to make nested directories visually distinct.
- **Color-Coded File Types:** Intuitive WinDirStat-style palette based on file extensions:
  - 🟣 **Media / Video:** `.mp4`, `.mkv`, `.avi`, `.mov`
  - 🟡 **Audio:** `.mp3`, `.flac`, `.wav`, `.ogg`
  - 🔵 **Images:** `.png`, `.jpg`, `.jpeg`, `.webp`, `.gif`
  - 🔴 **Archives:** `.zip`, `.tar`, `.gz`, `.7z`, `.rar`
  - 🟢 **Code & Documents:** `.cpp`, `.rs`, `.py`, `.js`, `.txt`, `.md`
  - 🔷 **Executables / Binaries:** `.bin`, `.so`, `.exe`
  - 🎨 **Dynamic fallback:** Deterministic HSL color hashing for all other extensions.
- **Interactive Camera:** Smooth pan and mouse-centric zoom into dense directory structures.
- **Real-time HUD:** Displays scan progress, total files indexed, total storage calculated, and hovered node metadata.

---

## Controls

| Action | Control |
|---|---|
| **Pan / Move Canvas** | `Left Click` or `Middle Click` + Drag |
| **Zoom in / out** | `Mouse Wheel` (centered around mouse pointer) |
| **Reset View** | `Spacebar` |
| **Inspect Node** | Hover mouse over any block |

---

## Download Pre-built Binaries

Pre-compiled 64-bit Linux binaries are built automatically via GitHub Actions.

Download the latest executable from the **[Releases](https://github.com/plops/pge_treemap/releases)** page:

```bash
# Download latest release
wget https://github.com/plops/pge_treemap/releases/latest/download/pge_treemap

# Make executable and run
chmod +x pge_treemap
./pge_treemap
