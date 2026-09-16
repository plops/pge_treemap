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
```

##  Codebase Map

The source files are prefixed with numerical increments of 10 (`000_`, `010_`, `020_`, ...). This allows new stages (such as filters, search indices, exporters, or custom shaders) to be slotted in between (e.g. `015_`, `045_`) without renumbering existing files.

```
source0/src/
├── 000_platform_fix.hpp     # Linux/X11 idle CPU spin interceptor
├── 010_types.hpp            # Geometric & tree data structures, synchronization state
├── 020_color_utils.hpp      # HSL color hashing, extension palettes, byte string formatting
├── 030_thread_pool.hpp      # General-purpose work-stealing task thread pool
├── 040_file_watcher.hpp     # Linux inotify event engine with temporal debouncing
├── 050_treemap_layout.hpp   # Squarified layout engine with recursive parallel fork/join
├── 060_scanner.hpp          # Directory crawler, tree cloner, and snapshot publisher
├── 070_treemap_app.hpp      # PGE3 front-end: cushion renderer, LOD culling, HUD & camera
├── 080_cli.hpp              # Command-line argument parser and directory validator
└── 090_main.cpp             # Translation unit entry point (PGE3 application instantiation)
```

### Suggested Reading Order
To understand how data flows through the application, read the files in numerical order:

1. **`010_types.hpp`**: Understand `FileNode`, `SharedScanContext`, and the double-buffered snapshot exchange model.
2. **`050_treemap_layout.hpp`**: Study how arbitrary-sized child nodes are squarified into 2D bounding boxes while optimizing aspect ratios.
3. **`030_thread_pool.hpp`**: Observe how the layout engine splits recursive subtrees across worker threads.
4. **`060_scanner.hpp`**: See how the disk crawler builds trees asynchronously and publishes snapshots at throttled intervals.
5. **`040_file_watcher.hpp`**: Learn how Linux `inotify` watches are dynamically registered during the crawl to trigger debounced rescans.
6. **`070_treemap_app.hpp`**: Follow the rendering pipeline, camera transformations, cushion shading, and zero-overhead garbage collection of stale trees.
7. **`080_cli.hpp` & `090_main.cpp`**: Inspect application bootstrapping and command-line entry.

---

## 2. System Architecture & Threading Model

The application runs four distinct concurrency domains simultaneously without stalling the 60 FPS graphics loop:

```
                      +-----------------------------+
                      |       Filesystem (OS)       |
                      +-----------------------------+
                        ▲                         │
                        │ inotify events          │ directory entries
                        │                         ▼
       +-------------------------------+   +------------------------------------+
       |   FileWatcher (inotify loop)  |   |   DirectoryScanner (Worker Thread) |
       |     300ms Debounced Queue     |──>|   Recursive crawl, gathers size    |
       +-------------------------------+   +------------------------------------+
                                                          │
                                         Periodically clones working tree
                                         and dispatches to Layout Engine
                                                          ▼
                                           +----------------------------+
                                           |      LayoutThreadPool      |
                                           |   Parallel Squarify Tasks  |
                                           +----------------------------+
                                                          │
                                              Tree layout complete
                                              (Atomic swap via mutex)
                                                          ▼
                                           +----------------------------+
                                           |   DiskTreemapAnalyzer      |
                                           |   PGE3 Main Render Thread  |
                                           |   - Cushion shading & LOD  |
                                           |   - Pan/Zoom camera & HUD  |
                                           +----------------------------+
```

### The Double-Buffered Snapshot Mechanism
- The `DirectoryScanner` thread modifies its own private working tree (`m_workerRoot`).
- When a publish boundary is reached (every 1,000 ms or after 10,000 files), the scanner **deep-copies** `m_workerRoot` into a snapshot.
- The snapshot's layout coordinates are computed in parallel using `LayoutThreadPool`.
- Once the coordinates are finalized, the snapshot is swapped into `SharedScanContext::readyRenderTree` under a lightweight mutex lock.
- On the next frame, `DiskTreemapAnalyzer::OnUserUpdate` steals `readyRenderTree` with an atomic pointer swap.
- The previous tree is moved to a detached background thread for deallocation (`std::thread([old = std::move(newTree)]() {}).detach()`), ensuring zero frame drops from heap deallocations of trees with 100,000+ nodes.

---

## 3. Algorithm: Parallel Squarified Treemaps

Standard slice-and-dice treemaps produce long, thin rectangles with terrible aspect ratios that are difficult to read and hover over. `pge_treemap` implements the **Squarified Treemap algorithm** (Bruls, Huizing, van Wijk, 2000), extended to execute concurrently across multiple CPU cores.

### 3.1 Mathematical Principle
Given an available bounding rectangle of dimensions $w \times h$ (with short side $s = \min(w, h)$) and a row of candidate children with areas $R = \{r_1, r_2, \dots, r_k\}$ and total area $R^+ = \sum r_i$:

The thickness of the row is:
$$t = \frac{R^+}{s}$$

The aspect ratio of each item $r_i$ placed within this row is:
$$\rho(r_i) = \max\left(\frac{s^2 \cdot r_i}{(R^+)^2}, \frac{(R^+)^2}{s^2 \cdot r_i}\right)$$

The worst aspect ratio of the row is:
$$\text{Worst}(R, s) = \max_{r_i \in R} \rho(r_i)$$

The algorithm greedily adds children to the active row as long as $\text{Worst}(R \cup \{r_{k+1}\}, s) \le \text{Worst}(R, s)$. When the ratio would degrade, the current row is finalized, subtracted from the bounding box, and a new row begins along the new shortest edge.

### 3.2 Parallel Fork-Join Decomposition (`050_treemap_layout.hpp`)
Rather than keeping the entire layout computation single-threaded, subdirectories with $\ge 4$ children fork into `LayoutThreadPool`:
```cpp
if (shouldFork && pool) {
    activeTasks.fetch_add(1, std::memory_order_release);
    pool->Enqueue([&executeTask, child]() {
        executeTask(executeTask, child);
    });
}
```
A barrier synchronization (`std::condition_variable compCv` coupled with an atomic task counter) holds the publisher until all subtrees across the thread pool have settled.

---

## 4. Cushion Treemap Rendering & Level-of-Detail (`070_treemap_app.hpp`)

Plain flat rectangles make it hard to tell where one directory ends and another begins. `pge_treemap` uses **adaptive cushion shading**:

1. **Bevel Highlights & Shadows:** Pseudo-3D borders with highlight (`RGBA(255, 255, 255, 80)`) on top/left and shadow (`RGBA(0, 0, 0, 110)`) on bottom/right.
2. **Bevel Clamping:** Bevel width scales dynamically based on tile dimensions:
   $$\text{bevel} = \text{clamp}(\min(\text{width}, \text{height}) \times 0.15, 1.0, 8.0)$$
3. **Screen-Space LOD Culling:**
   - Blocks projecting to $< 1.0\text{ px}$ on screen are discarded.
   - Non-leaf directories projecting to $< 3.0\text{ px}$ are rendered as solid grey blocks without traversing deeper children.
   - Text labels are only rendered if screen dimensions exceed $70 \times 22\text{ px}$.

---

## 5. Live Directory Monitoring (`040_file_watcher.hpp`)

On Linux, real-time filesystem updates are detected via kernel `inotify` queues:
- Directories encountered during the recursive crawl automatically have watch descriptors registered (`IN_MODIFY`, `IN_CREATE`, `IN_DELETE`, `IN_MOVED_FROM`, `IN_MOVED_TO`).
- Newly created subdirectories dynamically register watches on the fly.
- **Temporal Debounce:** Changes reset a 300 ms timer. Multiple rapid writes (e.g., git checkouts or compilers writing object files) collapse into a single atomic rescan trigger once the disk activity settles.

---

## 6. Build Instructions

### Prerequisites
- Modern C++23 compiler (`g++-13+` or `clang++-17+`)
- CMake 3.17+
- Ninja build system
- Development libraries: X11, Xi, OpenGL, PNG

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y cmake ninja-build libx11-dev libxi-dev libgl1-mesa-dev libpng-dev

# Clone and configure
git clone https://github.com/plops/pge_treemap.git
cd pge_treemap
# Also download PGE3 header (see Github action in .github folder)
cmake -S source0 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build
ninja -C build
```

---

## 7. Command-Line Usage

```bash
# Inspect current working directory
./build/pge_treemap

# Inspect a specific target directory
./build/pge_treemap /home/user/Downloads
./build/pge_treemap -d /var/log

# Show help
./build/pge_treemap --help
```
