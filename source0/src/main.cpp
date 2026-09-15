#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
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
        std::string name;
        uintmax_t sizeBytes = 0;
        bool isDirectory = false;
        std::vector<std::unique_ptr<FileNode>> children;

        vf2d visualPos = {0.0f, 0.0f};
        vf2d visualSize = {0.0f, 0.0f};
        Pixel color = Colour::WHITE;
    };

    struct SharedScanContext {
        mutable std::mutex treeMutex;
        std::unique_ptr<FileNode> readyRenderTree = nullptr;
        std::string currentPathInspected;

        std::atomic<bool> isScanning{false};
        std::atomic<bool> abortScanRequested{false};
        std::atomic<uintmax_t> totalFilesScanned{0};
        std::atomic<uintmax_t> totalBytesScanned{0};
        std::atomic<bool> hasNewDataForRender{false};
    };

    Pixel HSLtoPixel(float h, float s, float l) {
        auto f = [h, s, l](const float n) {
            const float k = std::fmod(n + h * 12.0f, 12.0f);
            const float a = s * std::min(l, 1.0f - l);
            return l - a * std::max(-1.0f, std::min({k - 3.0f, 9.0f - k, 1.0f}));
        };
        return {
            static_cast<uint8_t>(f(0) * 255.0f),
            static_cast<uint8_t>(f(8) * 255.0f),
            static_cast<uint8_t>(f(4) * 255.0f)
        };
    }

    // Zero-allocation extension check
    Pixel GetColorForFilename(std::string_view name) {
        auto dotPos = name.rfind('.');
        if (dotPos == std::string_view::npos) return {120, 130, 140};

        std::string ext(name.substr(dotPos));
        for (char &c : ext) c = static_cast<char>(tolower(c));

        if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov") return {185, 60, 220};
        if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg") return {240, 205, 35};
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif") return {30, 190, 230};
        if (ext == ".zip" || ext == ".rar" || ext == ".7z" || ext == ".tar" || ext == ".gz") return {235, 50, 50};
        if (ext == ".cpp" || ext == ".h" || ext == ".rs" || ext == ".py" || ext == ".js" || ext == ".txt" || ext == ".md") return {40, 210, 110};
        if (ext == ".exe" || ext == ".dll" || ext == ".so" || ext == ".bin") return {60, 100, 240};

        const size_t hash = std::hash<std::string>{}(ext);
        return HSLtoPixel(static_cast<float>(hash % 360) / 360.0f, 0.70f, 0.55f);
    }
} // namespace

class DiskTreemapAnalyzer : public PixelGameEngine {
public:
    DiskTreemapAnalyzer() { sAppName = "PGE3 - Live Disk Treemap Visualizer"; }
    ~DiskTreemapAnalyzer() override {
        m_shared.abortScanRequested = true;
        if (m_scanThread.joinable()) m_scanThread.join();
    }

private:
    const vi2d m_screenSize = {1280, 720};
    const vf2d WORLD_CANVAS_SIZE = {1280.0f, 720.0f};

    SharedScanContext m_shared;
    std::thread m_scanThread;
    std::unique_ptr<FileNode> m_renderRoot = nullptr;

    vf2d m_cameraOffset = {0.0f, 0.0f};
    float m_cameraZoom = 1.0f;
    vi2d m_lastMousePos = {0, 0};
    const FileNode *m_hoveredNode = nullptr;

    std::chrono::steady_clock::time_point m_lastPublishTime;
    uint32_t m_filesSinceLastPublishCheck = 0;
    std::unique_ptr<FileNode> m_workerRoot = nullptr;

public:
    bool OnUserCreate() override {
        m_lastMousePos = mouse.GetPosition();
        StartBackgroundScan(fs::current_path());
        return true;
    }

    bool OnUserUpdate(float fElapsedTime) override {
        HandleInput(fElapsedTime);

        // 1. Instant non-blocking tree swap & asynchronous destruction
        if (m_shared.hasNewDataForRender.load(std::memory_order_relaxed)) {
            std::unique_ptr<FileNode> newTree = nullptr;
            {
                std::lock_guard lock(m_shared.treeMutex);
                newTree = std::move(m_shared.readyRenderTree);
                m_shared.hasNewDataForRender.store(false, std::memory_order_relaxed);
            }
            std::swap(m_renderRoot, newTree);
            if (newTree) {
                // Destroy previous tree on a background thread to prevent UI stutter
                std::thread([old = std::move(newTree)]() {}).detach();
            }
        }

        // 2. Rendering
        draw.Clear(Pixel(20, 24, 30));
        draw.WorldReset();
        draw.WorldScale({m_cameraZoom, m_cameraZoom});
        draw.WorldOffset(m_cameraOffset);

        m_hoveredNode = nullptr;
        const vf2d mouseWorld = draw.ScreenToWorld(mouse.GetPosition());

        if (m_renderRoot) {
            RenderNode(m_renderRoot.get(), mouseWorld);
        }

        draw.WorldReset();
        RenderHUD();

        return true;
    }

private:
    void StartBackgroundScan(const fs::path &targetDirectory) {
        if (m_shared.isScanning) return;
        m_shared.isScanning = true;
        m_shared.abortScanRequested = false;

        m_scanThread = std::thread([this, targetDirectory]() {
            m_workerRoot = std::make_unique<FileNode>();
            m_workerRoot->name = targetDirectory.filename().string();
            m_workerRoot->isDirectory = true;

            m_lastPublishTime = std::chrono::steady_clock::now();
            m_filesSinceLastPublishCheck = 0;

            ScanDirectoryRecursive(targetDirectory, m_workerRoot.get());

            if (!m_shared.abortScanRequested) {
                PublishSnapshot(targetDirectory.string());
            }
            m_shared.isScanning = false;
        });
    }

    void ScanDirectoryRecursive(const fs::path &currentPath, FileNode *parentNode) {
        try {
            for (const auto &entry : fs::directory_iterator(currentPath, fs::directory_options::skip_permission_denied)) {
                if (m_shared.abortScanRequested) return;

                auto child = std::make_unique<FileNode>();
                child->name = entry.path().filename().string();
                child->isDirectory = entry.is_directory();

                FileNode *childPtr = child.get();
                parentNode->children.push_back(std::move(child));

                if (childPtr->isDirectory) {
                    ScanDirectoryRecursive(entry.path(), childPtr);
                } else {
                    try {
                        childPtr->sizeBytes = entry.file_size();
                    } catch (...) {
                        childPtr->sizeBytes = 0;
                    }
                    childPtr->color = GetColorForFilename(childPtr->name);
                    m_shared.totalBytesScanned += childPtr->sizeBytes;
                    ++m_shared.totalFilesScanned;
                }

                if (++m_filesSinceLastPublishCheck >= 10000) {
                    m_filesSinceLastPublishCheck = 0;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - m_lastPublishTime >= std::chrono::milliseconds(1000)) {
                        m_lastPublishTime = now;
                        PublishSnapshot(entry.path().string());
                    }
                }
            }
        } catch (...) {}
    }

    void PublishSnapshot(const std::string &currentInspectedPath) {
        if (!m_workerRoot) return;

        // 1. Copy snapshot and accumulate sizes (skips 0-byte files)
        auto snapshot = DeepCopyTree(m_workerRoot.get());
        if (!snapshot || snapshot->sizeBytes == 0) return;

        // 2. Compute layout entirely on the BACKGROUND thread
        snapshot->visualPos = {0.0f, 0.0f};
        snapshot->visualSize = WORLD_CANVAS_SIZE;
        CalculateTreemapLayout(snapshot.get(), snapshot->visualPos, snapshot->visualSize);

        // 3. Hand ready-to-render snapshot to UI
        {
            std::lock_guard lock(m_shared.treeMutex);
            m_shared.readyRenderTree = std::move(snapshot);
            m_shared.currentPathInspected = currentInspectedPath;
            m_shared.hasNewDataForRender.store(true, std::memory_order_relaxed);
        }
    }

    static void CalculateTreemapLayout(FileNode *node, vf2d pos, vf2d size) {
        if (!node || node->sizeBytes == 0) return;

        // OPTIMIZATION: Sub-pixel pruning. If container is < 1px, stop subdividing.
        if (size.x < 1.0f || size.y < 1.0f || node->children.empty()) return;

        std::sort(node->children.begin(), node->children.end(),
                  [](const auto &a, const auto &b) { return a->sizeBytes > b->sizeBytes; });

        const bool splitVertical = size.x >= size.y;
        float currentOffset = 0.0f;

        for (auto &child : node->children) {
            if (child->sizeBytes == 0) continue;

            const float ratio = static_cast<float>(child->sizeBytes) / static_cast<float>(node->sizeBytes);

            if (splitVertical) {
                float childWidth = size.x * ratio;
                child->visualPos = {pos.x + currentOffset, pos.y};
                child->visualSize = {childWidth, size.y};
                currentOffset += childWidth;

                // Prune microscopic slices (< 0.5px) from recursing further
                if (childWidth >= 0.5f) {
                    CalculateTreemapLayout(child.get(), child->visualPos, child->visualSize);
                }
            } else {
                float childHeight = size.y * ratio;
                child->visualPos = {pos.x, pos.y + currentOffset};
                child->visualSize = {size.x, childHeight};
                currentOffset += childHeight;

                if (childHeight >= 0.5f) {
                    CalculateTreemapLayout(child.get(), child->visualPos, child->visualSize);
                }
            }
        }
    }

    static std::unique_ptr<FileNode> DeepCopyTree(const FileNode *source) {
        if (!source) return nullptr;

        auto copy = std::make_unique<FileNode>();
        copy->name = source->name;
        copy->isDirectory = source->isDirectory;
        copy->color = source->color;

        if (!source->isDirectory) {
            copy->sizeBytes = source->sizeBytes;
        } else {
            copy->sizeBytes = 0;
            copy->children.reserve(source->children.size());
            for (const auto &child : source->children) {
                // Ignore 0-byte items completely
                if (child->sizeBytes == 0 && !child->isDirectory) continue;

                auto childCopy = DeepCopyTree(child.get());
                if (childCopy && childCopy->sizeBytes > 0) {
                    copy->sizeBytes += childCopy->sizeBytes;
                    copy->children.push_back(std::move(childCopy));
                }
            }
        }
        return copy;
    }

    void HandleInput(float fElapsedTime) {
        if (mouse.GetButton(0).bHeld || mouse.GetButton(2).bHeld) {
            const vi2d delta = mouse.GetPosition() - m_lastMousePos;
            m_cameraOffset += vf2d(delta) / m_cameraZoom;
        }
        m_lastMousePos = mouse.GetPosition();

        if (const int wheel = mouse.GetWheel(); wheel != 0) {
            const vf2d mouseBeforeZoom = draw.ScreenToWorld(mouse.GetPosition());
            if (wheel > 0) m_cameraZoom *= 1.15f;
            if (wheel < 0) m_cameraZoom /= 1.15f;
            m_cameraZoom = std::clamp(m_cameraZoom, 0.05f, 100.0f);

            const vf2d mouseAfterZoom = draw.ScreenToWorld(mouse.GetPosition());
            m_cameraOffset += mouseAfterZoom - mouseBeforeZoom;
        }

        if (keyboard.GetKey(Key::SPACE).bPressed) {
            m_cameraOffset = {0.0f, 0.0f};
            m_cameraZoom = 1.0f;
        }
    }

    void DrawCushionRect(const vf2d &pos, const vf2d &size, const Pixel &baseCol) {
        draw.FilledRect(pos, size, baseCol);
        const float screenW = size.x * m_cameraZoom;
        const float screenH = size.y * m_cameraZoom;
        if (screenW < 4.0f || screenH < 4.0f) return;

        float bevel = std::clamp(std::min(size.x, size.y) * 0.15f, 1.0f, 8.0f);
        constexpr Pixel highlight(255, 255, 255, 80);
        draw.FilledRect(pos, {size.x, bevel}, highlight);
        draw.FilledRect(pos, {bevel, size.y}, highlight);

        constexpr Pixel shadow(0, 0, 0, 110);
        draw.FilledRect({pos.x, pos.y + size.y - bevel}, {size.x, bevel}, shadow);
        draw.FilledRect({pos.x + size.x - bevel, pos.y}, {bevel, size.y}, shadow);

        draw.Rect(pos, size, Pixel(15, 15, 20, 200));
    }

    void RenderNode(const FileNode *node, const vf2d &mouseWorld) {
        if (!node) return;

        if (node->visualSize.x * m_cameraZoom < 1.0f || node->visualSize.y * m_cameraZoom < 1.0f) {
            return;
        }

        if (node->children.empty()) {
            DrawCushionRect(node->visualPos, node->visualSize, node->color);
        } else {
            for (const auto &child : node->children) {
                RenderNode(child.get(), mouseWorld);
            }
            draw.Rect(node->visualPos, node->visualSize, Pixel(0, 0, 0, 160));
        }

        if (mouseWorld.x >= node->visualPos.x && mouseWorld.x <= (node->visualPos.x + node->visualSize.x) &&
            mouseWorld.y >= node->visualPos.y && mouseWorld.y <= (node->visualPos.y + node->visualSize.y)) {
            if (!m_hoveredNode) m_hoveredNode = node;
        }

        if (node->visualSize.x * m_cameraZoom > 70.0f && node->visualSize.y * m_cameraZoom > 22.0f) {
            float invZoom = 1.0f / m_cameraZoom;
            draw.String(node->visualPos + vf2d{5.0f, 5.0f}, node->name, Colour::BLACK, {invZoom, invZoom});
            draw.String(node->visualPos + vf2d{4.0f, 4.0f}, node->name, Colour::WHITE, {invZoom, invZoom});
        }
    }

    void RenderHUD() {
        constexpr vf2d barPos = {0.0f, 0.0f};
        const vf2d barSize = {static_cast<float>(m_screenSize.x), 45.0f};
        draw.FilledRect(barPos, barSize, Pixel(15, 18, 22, 230), Colour::WHITE);

        const std::string scanStatus = m_shared.isScanning ? "SCANNING..." : "SCAN FINISHED";
        const Pixel statusColor = m_shared.isScanning ? Colour::YELLOW : Colour::GREEN;

        draw.String({10.0f, 8.0f}, scanStatus, statusColor);
        draw.String({150.0f, 8.0f}, "Files: " + std::to_string(m_shared.totalFilesScanned.load()), Colour::WHITE);
        draw.String({320.0f, 8.0f}, "Size: " + FormatBytes(m_shared.totalBytesScanned.load()), Colour::CYAN);

        if (m_hoveredNode) {
            const std::string hoverInfo = m_hoveredNode->name + " (" + FormatBytes(m_hoveredNode->sizeBytes) + ")";
            draw.String({10.0f, 26.0f}, hoverInfo, Colour::WHITE);
        } else if (m_shared.isScanning) {
            std::string inspecting;
            {
                std::lock_guard lock(m_shared.treeMutex);
                inspecting = m_shared.currentPathInspected;
            }
            draw.String({10.0f, 26.0f}, "Scanning: " + inspecting, Colour::GREY);
        } else {
            draw.String({10.0f, 26.0f}, "Pan: Left/Middle Drag | Zoom: Wheel | Reset: Space", Colour::DARK_GREY);
        }
    }

    static std::string FormatBytes(const uintmax_t bytes) {
        const char *suffixes[] = {"B", "KB", "MB", "GB", "TB"};
        int i = 0;
        auto dBytes = static_cast<double>(bytes);
        while (dBytes >= 1024.0 && i < 4) {
            dBytes /= 1024.0;
            i++;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f %s", dBytes, suffixes[i]);
        return {buf};
    }
};

int main() {
    if (DiskTreemapAnalyzer demo; demo.Construct({1280, 720}, {1, 1}, false)) {
        demo.Start();
    }
    return 0;
}