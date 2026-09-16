#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__) && __has_include(<sys/inotify.h>)
#define HAS_INOTIFY 1
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

#define OLC_PGE3_APPLICATION
#include "olcPixelGameEngine3.h"

// ----------------------------------------------------------------------------
// Linux / X11 Idle Fix:
// PGE3's Host_Linux_X11::StartSystem() spins XPending() at 100% CPU when idle.
// Intercepting XPending to sleep briefly when empty drops idle CPU to ~0%.
// ----------------------------------------------------------------------------
#if defined(__linux__)
extern "C" {
struct _XDisplay;
int XEventsQueued(struct _XDisplay* display, int mode);

int XPending(struct _XDisplay* display)
{
    // QueuedAfterFlush = 2
    int count = XEventsQueued(display, 2);
    if (count == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // QueuedAlready = 0
        count = XEventsQueued(display, 0);
    }
    return count;
}
}
#endif

namespace fs = std::filesystem;

namespace {
struct FileNode {
    std::string                            name;
    uintmax_t                              sizeBytes   = 0;
    bool                                   isDirectory = false;
    std::vector<std::unique_ptr<FileNode>> children;

    vf2d  visualPos  = {0.0f, 0.0f};
    vf2d  visualSize = {0.0f, 0.0f};
    Pixel color      = Colour::WHITE;
};

// ----------------------------------------------------------------------------
// Synchronization State: Background Scanner -> Main Render Thread
// ----------------------------------------------------------------------------
struct SharedScanContext {
    mutable std::mutex        readySnapshotMutex;
    std::unique_ptr<FileNode> readyRenderTree = nullptr;
    std::string               currentPathInspected;

    std::atomic<bool>      isScanning{false};
    std::atomic<bool>      abortScanRequested{false};
    std::atomic<uintmax_t> totalFilesScanned{0};
    std::atomic<uintmax_t> totalBytesScanned{0};
    std::atomic<bool>      hasNewDataForRender{false};
};

struct SquarifyItem {
    FileNode* node = nullptr;
    double    area = 0.0;
};

struct LayoutRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// ----------------------------------------------------------------------------
// General-purpose ThreadPool for parallel layout computation
// ----------------------------------------------------------------------------
class LayoutThreadPool
{
public:
    explicit LayoutThreadPool(const size_t numThreads = std::max(1u, std::thread::hardware_concurrency()))
    {
        m_workers.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i)
        {
            m_workers.emplace_back([this]() { WorkerLoop(); });
        }
    }

    ~LayoutThreadPool()
    {
        Stop();
    }

    void Stop()
    {
        {
            std::lock_guard lock(m_taskQueueMutex);
            if (m_stopRequested) return;
            m_stopRequested = true;
        }
        m_taskQueueCv.notify_all();
        for (auto& worker: m_workers)
        {
            if (worker.joinable()) worker.join();
        }
        m_workers.clear();
    }

    template<typename F>
    void Enqueue(F&& f)
    {
        {
            std::lock_guard lock(m_taskQueueMutex);
            m_taskQueue.emplace_back(std::forward<F>(f));
        }
        m_taskQueueCv.notify_one();
    }

    [[nodiscard]] size_t ThreadCount() const { return m_workers.size(); }

private:
    void WorkerLoop()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock lock(m_taskQueueMutex);
                m_taskQueueCv.wait(lock, [this]() {
                    return m_stopRequested || !m_taskQueue.empty();
                });

                if (m_stopRequested && m_taskQueue.empty())
                {
                    return;
                }

                task = std::move(m_taskQueue.front());
                m_taskQueue.pop_front();
            }
            task();
        }
    }

    std::vector<std::thread>          m_workers;
    std::deque<std::function<void()>> m_taskQueue;

    std::mutex              m_taskQueueMutex;
    std::condition_variable m_taskQueueCv;
    bool                    m_stopRequested = false;
};

Pixel HSLtoPixel(float h, float s, float l)
{
    auto f = [h, s, l](const float n) {
        const float k = std::fmod(n + h * 12.0f, 12.0f);
        const float a = s * std::min(l, 1.0f - l);
        return l - a * std::max(-1.0f, std::min({k - 3.0f, 9.0f - k, 1.0f}));
    };
    return {
        static_cast<uint8_t>(f(0) * 255.0f),
        static_cast<uint8_t>(f(8) * 255.0f),
        static_cast<uint8_t>(f(4) * 255.0f)};
}

Pixel GetColorForFilename(const std::string_view name)
{
    const auto dotPos = name.rfind('.');
    if (dotPos == std::string_view::npos) return {120, 130, 140};

    std::string ext(name.substr(dotPos));
    for (char& c: ext) c = static_cast<char>(tolower(c));

    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov") return {185, 60, 220};
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg") return {240, 205, 35};
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif") return {30, 190, 230};
    if (ext == ".zip" || ext == ".rar" || ext == ".7z" || ext == ".tar" || ext == ".gz") return {235, 50, 50};
    if (ext == ".cpp" || ext == ".h" || ext == ".rs" || ext == ".py" || ext == ".js" || ext == ".txt" || ext == ".md")
        return {40, 210, 110};
    if (ext == ".exe" || ext == ".dll" || ext == ".so" || ext == ".bin") return {60, 100, 240};

    const size_t hash = std::hash<std::string>{}(ext);
    return HSLtoPixel(static_cast<float>(hash % 360) / 360.0f, 0.70f, 0.55f);
}

class DiskTreemapAnalyzer : public PixelGameEngine
{
public:
    explicit DiskTreemapAnalyzer(fs::path targetPath)
        : m_targetPath(std::move(targetPath))
    {
        sAppName = "PGE3 - Treemap [" + m_targetPath.string() + "]";
    }

    ~DiskTreemapAnalyzer() override
    {
        m_shared.abortScanRequested = true;

        {
            std::lock_guard lock(m_scanMutex);
            m_scanRequested = false;
        }
        m_scanCv.notify_all();

#if defined(HAS_INOTIFY)
        if (m_stopEventFd >= 0)
        {
            const uint64_t        val          = 1;
            [[maybe_unused]] auto bytesWritten = write(m_stopEventFd, &val, sizeof(val));
        }
        if (m_inotifyThread.joinable()) m_inotifyThread.join();
        if (m_inotifyFd >= 0) close(m_inotifyFd);
        if (m_stopEventFd >= 0) close(m_stopEventFd);
#endif

        if (m_scanThread.joinable()) m_scanThread.join();
        if (m_layoutThreadPool) m_layoutThreadPool->Stop();
    }

private:
    const vi2d m_screenSize      = {1920, 1080};
    const vf2d WORLD_CANVAS_SIZE = {1920.0f, 1080.0f};

    fs::path                m_targetPath;
    SharedScanContext       m_shared;
    std::thread             m_scanThread;
    std::mutex              m_scanMutex;
    std::condition_variable m_scanCv;
    bool                    m_scanRequested = true;

    std::unique_ptr<FileNode>         m_renderRoot       = nullptr;
    std::unique_ptr<LayoutThreadPool> m_layoutThreadPool = nullptr;

    std::atomic<int64_t>    m_layoutActiveTaskCount{0};
    std::mutex              m_layoutCompletionMutex;
    std::condition_variable m_layoutCompletionCv;

    vf2d            m_cameraOffset = {0.0f, 0.0f};
    float           m_cameraZoom   = 1.0f;
    vi2d            m_lastMousePos = {0, 0};
    const FileNode* m_hoveredNode  = nullptr;
    int             m_cleanFrames  = 0;

    std::chrono::steady_clock::time_point m_lastFrameTime;
    std::chrono::steady_clock::time_point m_lastPublishTime;
    uint32_t                              m_filesSinceLastPublishCheck = 0;
    std::unique_ptr<FileNode>             m_workerRoot                 = nullptr;

#if defined(HAS_INOTIFY)
    int                                  m_inotifyFd   = -1;
    int                                  m_stopEventFd = -1;
    std::thread                          m_inotifyThread;
    std::mutex                           m_watchMutex;
    std::unordered_map<int, fs::path>    m_wdToPath;
    std::unordered_map<std::string, int> m_pathToWd;
    bool                                 m_inotifyActive = false;
#endif

public:
    bool OnUserCreate() override
    {
        m_layoutThreadPool = std::make_unique<LayoutThreadPool>();
        m_lastMousePos     = mouse.GetPosition();
        m_lastFrameTime    = std::chrono::steady_clock::now();

#if defined(HAS_INOTIFY)
        m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotifyFd >= 0)
        {
            m_stopEventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (m_stopEventFd >= 0)
            {
                m_inotifyActive = true;
                m_inotifyThread = std::thread([this]() { InotifyLoop(); });
            }
        }
#endif

        m_scanThread = std::thread([this]() { ScanWorkerLoop(); });
        return true;
    }

    bool OnUserUpdate(float fElapsedTime) override
    {
        // 1. Instant non-blocking tree swap
        bool hasNewTree = false;
        if (m_shared.hasNewDataForRender.load(std::memory_order_acquire))
        {
            std::unique_ptr<FileNode> newTree = nullptr;
            {
                std::lock_guard lock(m_shared.readySnapshotMutex);
                newTree = std::move(m_shared.readyRenderTree);
                m_shared.hasNewDataForRender.store(false, std::memory_order_relaxed);
            }
            std::swap(m_renderRoot, newTree);
            if (newTree)
            {
                std::thread([old = std::move(newTree)]() {}).detach();
            }
            hasNewTree    = true;
            m_hoveredNode = nullptr;
        }

        // 2. Activity / Dirty detection
        const vi2d currentMousePos  = mouse.GetPosition();
        const bool mouseMoved       = (currentMousePos != m_lastMousePos);
        const bool mouseInteracting = mouse.GetButton(0).bHeld || mouse.GetButton(0).bPressed || mouse.GetButton(0).bReleased || mouse.GetButton(1).bHeld || mouse.GetButton(1).bPressed || mouse.GetButton(1).bReleased || mouse.GetButton(2).bHeld || mouse.GetButton(2).bPressed || mouse.GetButton(2).bReleased || mouse.GetWheel() != 0;
        const bool keyInteracting   = keyboard.GetKey(Key::SPACE).bPressed || keyboard.GetKey(Key::SPACE).bHeld;
        const bool isScanning       = m_shared.isScanning.load(std::memory_order_relaxed);

        if (isScanning || hasNewTree || mouseMoved || mouseInteracting || keyInteracting)
        {
            m_cleanFrames = 0;
        }
        else
        {
            ++m_cleanFrames;
        }

        // Static scene sleep: avoid burning CPU redrawing identical frames
        if (m_cleanFrames > 2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return true;
        }

        HandleInput(fElapsedTime);

        // 3. Rendering
        draw.Clear(Pixel(20, 24, 30));
        draw.WorldReset();
        draw.WorldScale({m_cameraZoom, m_cameraZoom});
        draw.WorldOffset(m_cameraOffset);

        const vf2d mouseWorld = draw.ScreenToWorld(mouse.GetPosition());
        m_hoveredNode         = m_renderRoot ? FindHoveredNode(m_renderRoot.get(), mouseWorld) : nullptr;

        if (m_renderRoot)
        {
            const vf2d viewMin = draw.ScreenToWorld({0, 0});
            const vf2d viewMax = draw.ScreenToWorld(m_screenSize);
            RenderNode(m_renderRoot.get(), viewMin, viewMax);
        }

        draw.WorldReset();
        RenderHUD();

        // 4. Frame rate limiter (~60 FPS fallback)
        const auto now     = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_lastFrameTime);
        if (constexpr auto targetFrameTime = std::chrono::microseconds(16666); elapsed < targetFrameTime)
        {
            std::this_thread::sleep_for(targetFrameTime - elapsed);
        }
        m_lastFrameTime = std::chrono::steady_clock::now();

        return true;
    }

private:
    void RequestRescan()
    {
        {
            std::lock_guard lock(m_scanMutex);
            m_scanRequested = true;
        }
        m_scanCv.notify_one();
    }

    void ScanWorkerLoop()
    {
        while (!m_shared.abortScanRequested)
        {
            {
                std::unique_lock lock(m_scanMutex);
                m_scanCv.wait(lock, [this]() {
                    return m_shared.abortScanRequested || m_scanRequested;
                });

                if (m_shared.abortScanRequested) break;
                m_scanRequested = false;
            }

            m_shared.isScanning        = true;
            m_shared.totalBytesScanned = 0;
            m_shared.totalFilesScanned = 0;

            m_workerRoot         = std::make_unique<FileNode>();
            std::string rootName = m_targetPath.filename().string();
            if (rootName.empty())
            {
                rootName = m_targetPath.lexically_normal().filename().string();
                if (rootName.empty()) rootName = m_targetPath.string();
            }
            m_workerRoot->name        = rootName;
            m_workerRoot->isDirectory = true;

            m_lastPublishTime            = std::chrono::steady_clock::now();
            m_filesSinceLastPublishCheck = 0;

            AddSingleWatch(m_targetPath);
            ScanDirectoryRecursive(m_targetPath, m_workerRoot.get());

            if (!m_shared.abortScanRequested)
            {
                PublishSnapshot(m_targetPath.string());
            }
            m_shared.isScanning = false;
        }
    }

    void ScanDirectoryRecursive(const fs::path& currentPath, FileNode* parentNode)
    {
        try
        {
            for (const auto& entry:
                 fs::directory_iterator(currentPath, fs::directory_options::skip_permission_denied))
            {
                if (m_shared.abortScanRequested) return;

                std::error_code ec;
                if (entry.is_symlink(ec)) continue;

                auto child         = std::make_unique<FileNode>();
                child->name        = entry.path().filename().string();
                child->isDirectory = entry.is_directory(ec);

                FileNode* childPtr = child.get();
                parentNode->children.push_back(std::move(child));

                if (childPtr->isDirectory)
                {
                    AddSingleWatch(entry.path());
                    ScanDirectoryRecursive(entry.path(), childPtr);
                }
                else
                {
                    try
                    {
                        childPtr->sizeBytes = entry.file_size();
                    } catch (...)
                    {
                        childPtr->sizeBytes = 0;
                    }
                    childPtr->color = GetColorForFilename(childPtr->name);
                    m_shared.totalBytesScanned += childPtr->sizeBytes;
                    ++m_shared.totalFilesScanned;
                }

                if (++m_filesSinceLastPublishCheck >= 10000)
                {
                    m_filesSinceLastPublishCheck = 0;
                    if (const auto now = std::chrono::steady_clock::now();
                        now - m_lastPublishTime >= std::chrono::milliseconds(1000))
                    {
                        m_lastPublishTime = now;
                        PublishSnapshot(entry.path().string());
                    }
                }
            }
        } catch (...)
        {
        }
    }

    void PublishSnapshot(const std::string& currentInspectedPath)
    {
        if (!m_workerRoot || m_shared.abortScanRequested) return;

        auto snapshot = DeepCopyTree(m_workerRoot.get());
        if (!snapshot || snapshot->sizeBytes == 0 || m_shared.abortScanRequested) return;

        snapshot->visualPos  = {0.0f, 0.0f};
        snapshot->visualSize = WORLD_CANVAS_SIZE;

        CalculateTreemapLayoutParallel(snapshot.get(), snapshot->visualPos, snapshot->visualSize);

        if (m_shared.abortScanRequested) return;

        {
            std::lock_guard lock(m_shared.readySnapshotMutex);
            m_shared.readyRenderTree      = std::move(snapshot);
            m_shared.currentPathInspected = currentInspectedPath;
            m_shared.hasNewDataForRender.store(true, std::memory_order_release);
        }
    }

    // ------------------------------------------------------------------------
    // Linux inotify File System Watcher
    // ------------------------------------------------------------------------
    void AddSingleWatch(const fs::path& p)
    {
#if defined(HAS_INOTIFY)
        if (m_inotifyFd < 0) return;

        std::error_code   ec;
        const fs::path    canon   = fs::weakly_canonical(p, ec);
        const std::string pathStr = (!ec) ? canon.string() : p.lexically_normal().string();

        std::lock_guard lock(m_watchMutex);
        if (m_pathToWd.contains(pathStr)) return;

        constexpr uint32_t flags = IN_MODIFY | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB;
        if (const int wd = inotify_add_watch(m_inotifyFd, pathStr.c_str(), flags); wd >= 0)
        {
            m_wdToPath[wd]      = pathStr;
            m_pathToWd[pathStr] = wd;
        }
#else
        (void)p;
#endif
    }

    void AddWatchRecursive(const fs::path& rootPath)
    {
#if defined(HAS_INOTIFY)
        if (m_inotifyFd < 0) return;
        std::error_code ec;
        if (!fs::exists(rootPath, ec) || !fs::is_directory(rootPath, ec)) return;

        AddSingleWatch(rootPath);
        try
        {
            for (const auto& entry:
                 fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied, ec))
            {
                if (m_shared.abortScanRequested) return;
                if (entry.is_directory(ec))
                {
                    AddSingleWatch(entry.path());
                }
            }
        } catch (...)
        {
        }
#else
        (void)rootPath;
#endif
    }

#if defined(HAS_INOTIFY)
    void RemoveWatch(const int wd)
    {
        std::lock_guard lock(m_watchMutex);
        if (const auto it = m_wdToPath.find(wd); it != m_wdToPath.end())
        {
            m_pathToWd.erase(it->second.string());
            m_wdToPath.erase(it);
        }
    }

    fs::path GetPathForWd(const int wd)
    {
        std::lock_guard lock(m_watchMutex);
        if (const auto it = m_wdToPath.find(wd); it != m_wdToPath.end())
        {
            return it->second;
        }
        return {};
    }

    void InotifyLoop()
    {
        bool           hasPendingChange = false;
        auto           lastEventTime    = std::chrono::steady_clock::now();
        constexpr auto debounceDuration = std::chrono::milliseconds(300);

        pollfd pfd[2];
        pfd[0].fd     = m_inotifyFd;
        pfd[0].events = POLLIN;
        pfd[1].fd     = m_stopEventFd;
        pfd[1].events = POLLIN;

        alignas(alignof(struct inotify_event)) char buffer[4096 * 8];

        while (!m_shared.abortScanRequested)
        {
            int timeoutMs = -1;
            if (hasPendingChange)
            {
                const auto now = std::chrono::steady_clock::now();
                if (const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEventTime); elapsed >= debounceDuration)
                {
                    hasPendingChange = false;
                    RequestRescan();
                    timeoutMs = -1;
                }
                else
                {
                    timeoutMs = static_cast<int>((debounceDuration - elapsed).count());
                }
            }

            const int ret = poll(pfd, 2, timeoutMs);
            if (ret < 0)
            {
                if (errno == EINTR) continue;
                break;
            }

            if (pfd[1].revents & POLLIN)
            {
                break;
            }

            if (ret == 0 && hasPendingChange)
            {
                hasPendingChange = false;
                RequestRescan();
                continue;
            }

            if (pfd[0].revents & POLLIN)
            {
                if (const ssize_t len = read(m_inotifyFd, buffer, sizeof(buffer)); len > 0)
                {
                    for (const char* ptr = buffer; ptr < buffer + len;)
                    {
                        const auto* event = reinterpret_cast<const struct inotify_event*>(ptr);

                        if (event->mask & IN_IGNORED)
                        {
                            RemoveWatch(event->wd);
                        }
                        else if (event->mask & IN_Q_OVERFLOW)
                        {
                            hasPendingChange = true;
                            lastEventTime    = std::chrono::steady_clock::now();
                        }
                        else
                        {
                            if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)))
                            {
                                if (event->len > 0)
                                {
                                    if (const fs::path parentPath = GetPathForWd(event->wd); !parentPath.empty())
                                    {
                                        AddWatchRecursive(parentPath / event->name);
                                    }
                                }
                            }
                            hasPendingChange = true;
                            lastEventTime    = std::chrono::steady_clock::now();
                        }

                        ptr += sizeof(struct inotify_event) + event->len;
                    }
                }
            }
        }
    }
#endif

    static double WorstAspectRatio(const std::vector<SquarifyItem>& row, const double rowAreaSum, const double s)
    {
        if (row.empty() || s <= 0.0 || rowAreaSum <= 0.0)
        {
            return std::numeric_limits<double>::infinity();
        }
        const double s2       = s * s;
        const double sum2     = rowAreaSum * rowAreaSum;
        double       maxRatio = 0.0;
        for (const auto& [node, area]: row)
        {
            if (area <= 0.0) continue;
            const double r1 = (area * s2) / sum2;
            const double r2 = sum2 / (area * s2);
            if (const double r = (r1 > r2) ? r1 : r2; r > maxRatio)
            {
                maxRatio = r;
            }
        }
        return maxRatio;
    }

    static void LayoutRow(const std::vector<SquarifyItem>& row, const double rowAreaSum,
                          LayoutRect& rect, const bool isLastRow)
    {
        if (row.empty()) return;

        if (rect.w <= 0.0f || rect.h <= 0.0f || rowAreaSum <= 0.0)
        {
            for (const auto& [node, area]: row)
            {
                node->visualPos  = {rect.x, rect.y};
                node->visualSize = {0.0f, 0.0f};
            }
            return;
        }

        if (rect.w >= rect.h)
        {
            float rowThickness = isLastRow ? rect.w : static_cast<float>(rowAreaSum / rect.h);
            rowThickness       = std::clamp(rowThickness, 0.0f, rect.w);

            float currentY = rect.y;
            for (size_t i = 0; i < row.size(); ++i)
            {
                FileNode* child      = row[i].node;
                float     itemHeight = 0.0f;
                if (i == row.size() - 1)
                {
                    itemHeight = (rect.y + rect.h) - currentY;
                }
                else
                {
                    itemHeight = static_cast<float>((row[i].area / rowAreaSum) * rect.h);
                }
                itemHeight = std::max(0.0f, itemHeight);

                child->visualPos  = {rect.x, currentY};
                child->visualSize = {rowThickness, itemHeight};
                currentY += itemHeight;
            }

            rect.x += rowThickness;
            rect.w -= rowThickness;
            if (rect.w < 0.0f) rect.w = 0.0f;
        }
        else
        {
            float rowThickness = isLastRow ? rect.h : static_cast<float>(rowAreaSum / rect.w);
            rowThickness       = std::clamp(rowThickness, 0.0f, rect.h);

            float currentX = rect.x;
            for (size_t i = 0; i < row.size(); ++i)
            {
                FileNode* child     = row[i].node;
                float     itemWidth = 0.0f;
                if (i == row.size() - 1)
                {
                    itemWidth = (rect.x + rect.w) - currentX;
                }
                else
                {
                    itemWidth = static_cast<float>((row[i].area / rowAreaSum) * rect.w);
                }
                itemWidth = std::max(0.0f, itemWidth);

                child->visualPos  = {currentX, rect.y};
                child->visualSize = {itemWidth, rowThickness};
                currentX += itemWidth;
            }

            rect.y += rowThickness;
            rect.h -= rowThickness;
            if (rect.h < 0.0f) rect.h = 0.0f;
        }
    }

    static std::vector<FileNode*> LayoutDirectChildren(FileNode* node)
    {
        std::vector<FileNode*> eligibleChildren;
        if (!node || node->sizeBytes == 0 || node->children.empty()) return eligibleChildren;
        if (node->visualSize.x < 0.5f || node->visualSize.y < 0.5f) return eligibleChildren;

        std::ranges::sort(node->children,
                          [](const auto& a, const auto& b) { return a->sizeBytes > b->sizeBytes; });

        uintmax_t totalBytes = 0;
        for (const auto& child: node->children)
        {
            if (child && child->sizeBytes > 0)
            {
                totalBytes += child->sizeBytes;
            }
        }
        if (totalBytes == 0) return eligibleChildren;

        const double              totalArea = static_cast<double>(node->visualSize.x) * static_cast<double>(node->visualSize.y);
        std::vector<SquarifyItem> items;
        items.reserve(node->children.size());

        for (const auto& child: node->children)
        {
            if (child && child->sizeBytes > 0)
            {
                const double area = (static_cast<double>(child->sizeBytes) / static_cast<double>(totalBytes)) * totalArea;
                items.push_back({.node = child.get(), .area = area});
            }
            else if (child)
            {
                child->visualPos  = node->visualPos;
                child->visualSize = {0.0f, 0.0f};
            }
        }

        if (items.empty()) return eligibleChildren;

        LayoutRect                rect = {.x = node->visualPos.x, .y = node->visualPos.y, .w = node->visualSize.x, .h = node->visualSize.y};
        std::vector<SquarifyItem> currentRow;
        double                    currentRowAreaSum = 0.0;

        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& candidate = items[i];

            if (currentRow.empty())
            {
                currentRow.push_back(candidate);
                currentRowAreaSum = candidate.area;
            }
            else
            {
                const double s            = (rect.w >= rect.h) ? rect.h : rect.w;
                const double currentWorst = WorstAspectRatio(currentRow, currentRowAreaSum, s);

                currentRow.push_back(candidate);

                if (const double newWorst = WorstAspectRatio(currentRow, currentRowAreaSum + candidate.area, s);
                    newWorst <= currentWorst)
                {
                    currentRowAreaSum += candidate.area;
                }
                else
                {
                    currentRow.pop_back();
                    LayoutRow(currentRow, currentRowAreaSum, rect, false);
                    currentRow.clear();

                    if (rect.w <= 0.0f || rect.h <= 0.0f)
                    {
                        for (size_t j = i; j < items.size(); ++j)
                        {
                            items[j].node->visualPos  = {rect.x, rect.y};
                            items[j].node->visualSize = {0.0f, 0.0f};
                        }
                        break;
                    }

                    currentRow.push_back(candidate);
                    currentRowAreaSum = candidate.area;
                }
            }
        }

        if (!currentRow.empty())
        {
            LayoutRow(currentRow, currentRowAreaSum, rect, true);
        }

        eligibleChildren.reserve(node->children.size());
        for (const auto& child: node->children)
        {
            if (child && !child->children.empty() && child->visualSize.x >= 0.5f && child->visualSize.y >= 0.5f)
            {
                eligibleChildren.push_back(child.get());
            }
        }
        return eligibleChildren;
    }

    void CalculateTreemapLayoutParallel(FileNode* root, const vf2d pos, const vf2d size)
    {
        if (!root || root->sizeBytes == 0) return;
        root->visualPos  = pos;
        root->visualSize = size;
        if (size.x < 0.5f || size.y < 0.5f || root->children.empty()) return;

        const size_t maxParallelTasks = m_layoutThreadPool ? m_layoutThreadPool->ThreadCount() * 4 : 1;
        m_layoutActiveTaskCount.store(1, std::memory_order_release);

        if (m_layoutThreadPool && m_layoutThreadPool->ThreadCount() > 1)
        {
            m_layoutThreadPool->Enqueue([this, root, maxParallelTasks]() {
                ExecuteSubtreeTask(root, maxParallelTasks);
            });

            std::unique_lock lock(m_layoutCompletionMutex);
            m_layoutCompletionCv.wait(lock, [this]() {
                return m_layoutActiveTaskCount.load(std::memory_order_acquire) == 0;
            });
        }
        else
        {
            ExecuteSubtreeTask(root, 0);
        }
    }

    void ExecuteSubtreeTask(FileNode* node, const size_t maxParallelTasks)
    {
        LayoutSubtreeRecursive(node, true, maxParallelTasks);

        if (m_layoutActiveTaskCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            std::lock_guard lock(m_layoutCompletionMutex);
            m_layoutCompletionCv.notify_all();
        }
    }

    void LayoutSubtreeRecursive(FileNode* node, const bool allowFork, const size_t maxParallelTasks)
    {
        if (m_shared.abortScanRequested.load(std::memory_order_relaxed)) return;

        for (const std::vector<FileNode*> eligibleChildren = LayoutDirectChildren(node); FileNode* child: eligibleChildren)
        {
            if (m_shared.abortScanRequested.load(std::memory_order_relaxed)) return;

            if (const bool shouldFork = allowFork && (child->children.size() >= 4) && (m_layoutActiveTaskCount.load(std::memory_order_relaxed) < static_cast<int64_t>(maxParallelTasks)); shouldFork && m_layoutThreadPool)
            {
                m_layoutActiveTaskCount.fetch_add(1, std::memory_order_release);
                m_layoutThreadPool->Enqueue([this, child, maxParallelTasks]() {
                    ExecuteSubtreeTask(child, maxParallelTasks);
                });
            }
            else
            {
                LayoutSubtreeRecursive(child, false, maxParallelTasks);
            }
        }
    }

    static std::unique_ptr<FileNode> DeepCopyTree(const FileNode* source)
    {
        if (!source) return nullptr;

        auto copy         = std::make_unique<FileNode>();
        copy->name        = source->name;
        copy->isDirectory = source->isDirectory;
        copy->color       = source->color;

        if (!source->isDirectory)
        {
            copy->sizeBytes = source->sizeBytes;
        }
        else
        {
            copy->sizeBytes = 0;
            copy->children.reserve(source->children.size());
            for (const auto& child: source->children)
            {
                if (child->sizeBytes == 0 && !child->isDirectory) continue;

                if (auto childCopy = DeepCopyTree(child.get()); childCopy && childCopy->sizeBytes > 0)
                {
                    copy->sizeBytes += childCopy->sizeBytes;
                    copy->children.push_back(std::move(childCopy));
                }
            }
        }
        return copy;
    }

    void HandleInput(float)
    {
        if (mouse.GetButton(0).bHeld || mouse.GetButton(2).bHeld)
        {
            const vi2d delta = mouse.GetPosition() - m_lastMousePos;
            m_cameraOffset += vf2d(delta) / m_cameraZoom;
        }
        m_lastMousePos = mouse.GetPosition();

        if (const int wheel = mouse.GetWheel(); wheel != 0)
        {
            const vf2d mouseBeforeZoom = draw.ScreenToWorld(mouse.GetPosition());
            if (wheel > 0) m_cameraZoom *= 1.15f;
            if (wheel < 0) m_cameraZoom /= 1.15f;
            m_cameraZoom = std::clamp(m_cameraZoom, 0.05f, 100.0f);

            const vf2d mouseAfterZoom = draw.ScreenToWorld(mouse.GetPosition());
            m_cameraOffset += mouseAfterZoom - mouseBeforeZoom;
        }

        if (keyboard.GetKey(Key::SPACE).bPressed)
        {
            m_cameraOffset = {0.0f, 0.0f};
            m_cameraZoom   = 1.0f;
        }
    }

    void DrawCushionRect(const vf2d& pos, const vf2d& size, const Pixel& baseCol)
    {
        draw.FilledRect(pos, size, baseCol);
        const float screenW = size.x * m_cameraZoom;
        if (const float screenH = size.y * m_cameraZoom; screenW < 4.0f || screenH < 4.0f) return;

        const float     bevel = std::clamp(std::min(size.x, size.y) * 0.15f, 1.0f, 8.0f);
        constexpr Pixel highlight(255, 255, 255, 80);
        draw.FilledRect(pos, {size.x, bevel}, highlight);
        draw.FilledRect(pos, {bevel, size.y}, highlight);

        constexpr Pixel shadow(0, 0, 0, 110);
        draw.FilledRect({pos.x, pos.y + size.y - bevel}, {size.x, bevel}, shadow);
        draw.FilledRect({pos.x + size.x - bevel, pos.y}, {bevel, size.y}, shadow);

        draw.Rect(pos, size, Pixel(15, 15, 20, 200));
    }

    static const FileNode* FindHoveredNode(const FileNode* node, const vf2d& pt)
    {
        if (!node || node->visualSize.x <= 0.0f || node->visualSize.y <= 0.0f) return nullptr;
        if (pt.x < node->visualPos.x || pt.x > (node->visualPos.x + node->visualSize.x) || pt.y < node->visualPos.y || pt.y > (node->visualPos.y + node->visualSize.y))
        {
            return nullptr;
        }

        for (const auto& child: node->children)
        {
            if (const auto* hit = FindHoveredNode(child.get(), pt))
            {
                return hit;
            }
        }
        return node;
    }

    void RenderNode(const FileNode* node, const vf2d& viewMin, const vf2d& viewMax)
    {
        if (!node) return;

        if (node->visualPos.x > viewMax.x || (node->visualPos.x + node->visualSize.x) < viewMin.x || node->visualPos.y > viewMax.y || (node->visualPos.y + node->visualSize.y) < viewMin.y)
        {
            return;
        }

        const float screenW = node->visualSize.x * m_cameraZoom;
        const float screenH = node->visualSize.y * m_cameraZoom;

        if (screenW < 1.0f || screenH < 1.0f)
        {
            return;
        }

        if (node->children.empty())
        {
            DrawCushionRect(node->visualPos, node->visualSize, node->color);
        }
        else
        {
            if (screenW < 3.0f || screenH < 3.0f)
            {
                draw.FilledRect(node->visualPos, node->visualSize, Pixel(40, 45, 55));
                return;
            }

            for (const auto& child: node->children)
            {
                RenderNode(child.get(), viewMin, viewMax);
            }
            draw.Rect(node->visualPos, node->visualSize, Pixel(0, 0, 0, 160));
        }

        if (screenW > 70.0f && screenH > 22.0f)
        {
            const float invZoom = 1.0f / m_cameraZoom;
            draw.String(node->visualPos + vf2d{5.0f, 5.0f}, node->name, Colour::BLACK, {invZoom, invZoom});
            draw.String(node->visualPos + vf2d{4.0f, 4.0f}, node->name, Colour::WHITE, {invZoom, invZoom});
        }
    }

    void RenderHUD()
    {
        constexpr vf2d barPos  = {0.0f, 0.0f};
        const vf2d     barSize = {static_cast<float>(m_screenSize.x), 45.0f};
        draw.FilledRect(barPos, barSize, Pixel(15, 18, 22, 230), Colour::WHITE);

        std::string scanStatus  = m_shared.isScanning ? "SCANNING..." : "SCAN FINISHED";
        Pixel       statusColor = m_shared.isScanning ? Colour::YELLOW : Colour::GREEN;

#if defined(HAS_INOTIFY)
        if (m_inotifyActive && !m_shared.isScanning)
        {
            scanStatus  = "LIVE (INOTIFY)";
            statusColor = Colour::GREEN;
        }
#endif

        draw.String({10.0f, 8.0f}, scanStatus, statusColor);
        draw.String({150.0f, 8.0f}, "Files: " + std::to_string(m_shared.totalFilesScanned.load()), Colour::WHITE);
        draw.String({320.0f, 8.0f}, "Size: " + FormatBytes(m_shared.totalBytesScanned.load()), Colour::CYAN);

        std::string pathDisplay = m_targetPath.string();
        if (pathDisplay.length() > 65)
        {
            pathDisplay = "..." + pathDisplay.substr(pathDisplay.length() - 62);
        }
        draw.String({500.0f, 8.0f}, "Path: " + pathDisplay, Colour::GREY);

        if (m_hoveredNode)
        {
            const std::string hoverInfo = m_hoveredNode->name + " (" + FormatBytes(m_hoveredNode->sizeBytes) + ")";
            draw.String({10.0f, 26.0f}, hoverInfo, Colour::WHITE);
        }
        else if (m_shared.isScanning)
        {
            std::string inspecting;
            {
                std::lock_guard lock(m_shared.readySnapshotMutex);
                inspecting = m_shared.currentPathInspected;
            }
            draw.String({10.0f, 26.0f}, "Scanning: " + inspecting, Colour::GREY);
        }
        else
        {
            draw.String({10.0f, 26.0f}, "Pan: Left/Middle Drag | Zoom: Wheel | Reset: Space", Colour::DARK_GREY);
        }
    }

    static std::string FormatBytes(const uintmax_t bytes)
    {
        const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
        int         i          = 0;
        auto        dBytes     = static_cast<double>(bytes);
        while (dBytes >= 1024.0 && i < 4)
        {
            dBytes /= 1024.0;
            i++;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f %s", dBytes, suffixes[i]);
        return {buf};
    }
};
} // namespace

int main(int argc, char* argv[])
{
    fs::path targetDir = fs::current_path();

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: " << argv[0] << " [DIRECTORY] [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  -d, --dir <PATH>     Directory to visualize\n"
                      << "  -h, --help           Display this help and exit\n\n"
                      << "If no directory is provided, the current working directory is used.\n"
                      << "On Linux with inotify, changes inside the directory automatically update the treemap.\n";
            return 0;
        }
        if ((arg == "-d" || arg == "--dir" || arg == "-p" || arg == "--path") && i + 1 < argc)
        {
            targetDir = argv[++i];
        }
        else if (!arg.starts_with('-'))
        {
            targetDir = arg;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << "\n"
                      << "Run '" << argv[0] << " --help' for usage.\n";
            return 1;
        }
    }

    std::error_code ec;
    targetDir = fs::absolute(targetDir, ec);
    if (ec || !fs::exists(targetDir, ec))
    {
        std::cerr << "Error: Path does not exist: " << targetDir.string() << "\n";
        return 1;
    }
    if (!fs::is_directory(targetDir, ec))
    {
        std::cerr << "Error: Path is not a directory: " << targetDir.string() << "\n";
        return 1;
    }

    const PGEConfig config = [] {
        PGEConfig c{
            .vScreenSize = {1920, 1080},
            .vPixelSize  = {1, 1},
            .bVSync      = true};
        c.bFullScreen = false;
        return c;
    }();

    if (DiskTreemapAnalyzer demo(targetDir); demo.Construct(config))
    {
        demo.Start();
    }
    return 0;
}