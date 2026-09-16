#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "010_types.hpp"
#include "020_color_utils.hpp"
#include "030_thread_pool.hpp"
#include "040_file_watcher.hpp"
#include "060_scanner.hpp"
#include "olcPixelGameEngine3.h"

namespace pge_treemap {

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
        if (m_scanner) m_scanner->Stop();
        if (m_watcher) m_watcher->Stop();
        if (m_layoutThreadPool) m_layoutThreadPool->Stop();
    }

    bool OnUserCreate() override
    {
        m_layoutThreadPool = std::make_unique<LayoutThreadPool>();
        m_lastMousePos     = mouse.GetPosition();
        m_lastFrameTime    = std::chrono::steady_clock::now();

        m_watcher = std::make_unique<FileWatcher>([this]() {
            if (m_scanner) m_scanner->RequestRescan();
        });

        m_scanner = std::make_unique<DirectoryScanner>(
            m_targetPath, m_shared, *m_layoutThreadPool, *m_watcher, WORLD_CANVAS_SIZE);

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

        if (const bool isScanning = m_shared.isScanning.load(std::memory_order_relaxed); isScanning || hasNewTree || mouseMoved || mouseInteracting || keyInteracting)
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

        if (m_watcher && m_watcher->IsActive() && !m_shared.isScanning)
        {
            scanStatus  = "LIVE (INOTIFY)";
            statusColor = Colour::GREEN;
        }

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

    const vi2d m_screenSize      = {1920, 1080};
    const vf2d WORLD_CANVAS_SIZE = {1920.0f, 1080.0f};

    fs::path                          m_targetPath;
    SharedScanContext                 m_shared;
    std::unique_ptr<LayoutThreadPool> m_layoutThreadPool = nullptr;
    std::unique_ptr<FileWatcher>      m_watcher          = nullptr;
    std::unique_ptr<DirectoryScanner> m_scanner          = nullptr;

    std::unique_ptr<FileNode>             m_renderRoot   = nullptr;
    vf2d                                  m_cameraOffset = {0.0f, 0.0f};
    float                                 m_cameraZoom   = 1.0f;
    vi2d                                  m_lastMousePos = {0, 0};
    const FileNode*                       m_hoveredNode  = nullptr;
    int                                   m_cleanFrames  = 0;
    std::chrono::steady_clock::time_point m_lastFrameTime;
};

} // namespace pge_treemap
