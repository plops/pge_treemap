#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define OLC_PGE3_APPLICATION
#include "olcPixelGameEngine3.h"

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

struct SharedScanContext {
    mutable std::mutex        treeMutex;
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

// Zero-allocation extension check
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
    DiskTreemapAnalyzer() { sAppName = "PGE3 - Live Disk Treemap Visualizer"; }

    ~DiskTreemapAnalyzer() override
    {
        m_shared.abortScanRequested = true;
        if (m_scanThread.joinable()) m_scanThread.join();
    }

private:
    const vi2d m_screenSize      = {1280, 720};
    const vf2d WORLD_CANVAS_SIZE = {1280.0f, 720.0f};

    SharedScanContext         m_shared;
    std::thread               m_scanThread;
    std::unique_ptr<FileNode> m_renderRoot = nullptr;

    vf2d            m_cameraOffset = {0.0f, 0.0f};
    float           m_cameraZoom   = 1.0f;
    vi2d            m_lastMousePos = {0, 0};
    const FileNode* m_hoveredNode  = nullptr;
    int             m_cleanFrames  = 0;

    std::chrono::steady_clock::time_point m_lastPublishTime;
    uint32_t                              m_filesSinceLastPublishCheck = 0;
    std::unique_ptr<FileNode>             m_workerRoot                 = nullptr;

public:
    bool OnUserCreate() override
    {
        m_lastMousePos = mouse.GetPosition();
        StartBackgroundScan(fs::current_path());
        return true;
    }

    bool OnUserUpdate(float fElapsedTime) override
    {
        // 1. Instant non-blocking tree swap & asynchronous destruction
        bool hasNewTree = false;
        if (m_shared.hasNewDataForRender.load(std::memory_order_relaxed))
        {
            std::unique_ptr<FileNode> newTree = nullptr;
            {
                std::lock_guard lock(m_shared.treeMutex);
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

        // 2. Determine activity state
        const vi2d currentMousePos  = mouse.GetPosition();
        const bool mouseMoved       = (currentMousePos != m_lastMousePos);
        const bool mouseInteracting = mouse.GetButton(0).bHeld || mouse.GetButton(0).bPressed || mouse.GetButton(0).bReleased ||
                                      mouse.GetButton(1).bHeld || mouse.GetButton(1).bPressed || mouse.GetButton(1).bReleased ||
                                      mouse.GetButton(2).bHeld || mouse.GetButton(2).bPressed || mouse.GetButton(2).bReleased ||
                                      mouse.GetWheel() != 0;
        const bool keyInteracting = keyboard.GetKey(Key::SPACE).bPressed || keyboard.GetKey(Key::SPACE).bHeld;
        const bool isScanning     = m_shared.isScanning.load(std::memory_order_relaxed);

        const bool isDirty = isScanning || hasNewTree || mouseMoved || mouseInteracting || keyInteracting;

        if (isDirty)
        {
            m_cleanFrames = 0;
        }
        else
        {
            ++m_cleanFrames;
        }

        // When scan is done and scene is completely static for >2 frames,
        // sleep to yield CPU and avoid re-rendering identical frames.
        if (m_cleanFrames > 2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
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

        return true;
    }

private:
    void StartBackgroundScan(const fs::path& targetDirectory)
    {
        if (m_shared.isScanning) return;
        m_shared.isScanning         = true;
        m_shared.abortScanRequested = false;

        m_scanThread = std::thread([this, targetDirectory]() {
            m_workerRoot              = std::make_unique<FileNode>();
            m_workerRoot->name        = targetDirectory.filename().string();
            m_workerRoot->isDirectory = true;

            m_lastPublishTime            = std::chrono::steady_clock::now();
            m_filesSinceLastPublishCheck = 0;

            ScanDirectoryRecursive(targetDirectory, m_workerRoot.get());

            if (!m_shared.abortScanRequested)
            {
                PublishSnapshot(targetDirectory.string());
            }
            m_shared.isScanning = false;
        });
    }

    void ScanDirectoryRecursive(const fs::path& currentPath, FileNode* parentNode)
    {
        try
        {
            for (const auto& entry:
                 fs::directory_iterator(currentPath, fs::directory_options::skip_permission_denied))
            {
                if (m_shared.abortScanRequested) return;

                auto child         = std::make_unique<FileNode>();
                child->name        = entry.path().filename().string();
                child->isDirectory = entry.is_directory();

                FileNode* childPtr = child.get();
                parentNode->children.push_back(std::move(child));

                if (childPtr->isDirectory)
                {
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
        if (!m_workerRoot) return;

        // 1. Copy snapshot and accumulate sizes (skips 0-byte files)
        auto snapshot = DeepCopyTree(m_workerRoot.get());
        if (!snapshot || snapshot->sizeBytes == 0) return;

        // 2. Compute layout entirely on the BACKGROUND thread
        snapshot->visualPos  = {0.0f, 0.0f};
        snapshot->visualSize = WORLD_CANVAS_SIZE;
        CalculateTreemapLayout(snapshot.get(), snapshot->visualPos, snapshot->visualSize);

        // 3. Hand ready-to-render snapshot to UI
        {
            std::lock_guard lock(m_shared.treeMutex);
            m_shared.readyRenderTree      = std::move(snapshot);
            m_shared.currentPathInspected = currentInspectedPath;
            m_shared.hasNewDataForRender.store(true, std::memory_order_relaxed);
        }
    }

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

                if (child->visualSize.x >= 0.5f && child->visualSize.y >= 0.5f && !child->children.empty())
                {
                    CalculateTreemapLayout(child, child->visualPos, child->visualSize);
                }
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

                if (child->visualSize.x >= 0.5f && child->visualSize.y >= 0.5f && !child->children.empty())
                {
                    CalculateTreemapLayout(child, child->visualPos, child->visualSize);
                }
            }

            rect.y += rowThickness;
            rect.h -= rowThickness;
            if (rect.h < 0.0f) rect.h = 0.0f;
        }
    }

    static void CalculateTreemapLayout(FileNode* node, const vf2d pos, const vf2d size)
    {
        if (!node || node->sizeBytes == 0) return;
        node->visualPos  = pos;
        node->visualSize = size;

        if (size.x < 0.5f || size.y < 0.5f || node->children.empty()) return;

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
        if (totalBytes == 0) return;

        const double              totalArea = static_cast<double>(size.x) * static_cast<double>(size.y);
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
                child->visualPos  = pos;
                child->visualSize = {0.0f, 0.0f};
            }
        }

        if (items.empty()) return;

        LayoutRect                rect = {.x = pos.x, .y = pos.y, .w = size.x, .h = size.y};
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

        float           bevel = std::clamp(std::min(size.x, size.y) * 0.15f, 1.0f, 8.0f);
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
        if (pt.x < node->visualPos.x || pt.x > (node->visualPos.x + node->visualSize.x) ||
            pt.y < node->visualPos.y || pt.y > (node->visualPos.y + node->visualSize.y))
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

        // Viewport frustum culling
        if (node->visualPos.x > viewMax.x || (node->visualPos.x + node->visualSize.x) < viewMin.x ||
            node->visualPos.y > viewMax.y || (node->visualPos.y + node->visualSize.y) < viewMin.y)
        {
            return;
        }

        const float screenW = node->visualSize.x * m_cameraZoom;
        const float screenH = node->visualSize.y * m_cameraZoom;

        // Sub-pixel culling
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
            // If directory is smaller than 3px on screen, render flat rect instead of traversing thousands of hidden children
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

        const std::string scanStatus  = m_shared.isScanning ? "SCANNING..." : "SCAN FINISHED";
        const Pixel       statusColor = m_shared.isScanning ? Colour::YELLOW : Colour::GREEN;

        draw.String({10.0f, 8.0f}, scanStatus, statusColor);
        draw.String({150.0f, 8.0f}, "Files: " + std::to_string(m_shared.totalFilesScanned.load()), Colour::WHITE);
        draw.String({320.0f, 8.0f}, "Size: " + FormatBytes(m_shared.totalBytesScanned.load()), Colour::CYAN);

        if (m_hoveredNode)
        {
            const std::string hoverInfo = m_hoveredNode->name + " (" + FormatBytes(m_hoveredNode->sizeBytes) + ")";
            draw.String({10.0f, 26.0f}, hoverInfo, Colour::WHITE);
        }
        else if (m_shared.isScanning)
        {
            std::string inspecting;
            {
                std::lock_guard lock(m_shared.treeMutex);
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

int main()
{
    PGEConfig config{.vScreenSize = {1280,720}, .vPixelSize = {1,1},.bVSync = True };
    config.bFullScreen = false;
    if (DiskTreemapAnalyzer demo; demo.Construct(config))
    {
        demo.Start();
    }
    return 0;
}