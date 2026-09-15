#include <iostream>
#include <filesystem>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <string>
#include <chrono>

// PGE3 Single-Header Include
#define OLC_PGE3_APPLICATION
#include "olcPixelGameEngine3.h"

namespace fs = std::filesystem;

// ============================================================================
// 1. DATENMODELL & HIERARCHIE
// ============================================================================

namespace {
    struct FileNode {
        fs::path path;
        std::string name;
        uintmax_t sizeBytes = 0;
        bool isDirectory = false;
        std::vector<std::unique_ptr<FileNode>> children;

        // Koordinaten im virtuellen Welt-Raum (wird vom Treemap-Algorithmus berechnet)
        olc::vf2d visualPos  = { 0.0f, 0.0f };
        olc::vf2d visualSize = { 0.0f, 0.0f };
        olc::Pixel color     = olc::Colour::WHITE;
    };
}

// ============================================================================
// 2. THREAD-SHARED STATE (Klar getrennt vom UI-/Render-Zustand)
// ============================================================================

namespace {
    struct SharedScanContext {
        // Dieser Mutex schützt AUSSCHLIESSLICH den Zugriff auf den Datenbaum
        mutable std::mutex treeMutex;
        std::unique_ptr<FileNode> rootNode = nullptr;
        std::string currentPathInspected;

        // Lock-freie Status-Variablen für das UI
        std::atomic<bool> isScanning{ false };
        std::atomic<bool> abortScanRequested{ false };
        std::atomic<uintmax_t> totalFilesScanned{ 0 };
        std::atomic<uintmax_t> totalBytesScanned{ 0 };

        // Signalisiert dem Render-Thread, dass neue Daten zur Verfügung stehen
        std::atomic<bool> hasNewDataForLayout{ false };
    };
}

// ============================================================================
// 3. ENGINE IMPLEMENTIERUNG (PGE3 INTERFACE-BASIERT)
// ============================================================================

namespace {
    class DiskTreemapAnalyzer : public olc::PixelGameEngine {
    public:
        DiskTreemapAnalyzer() {
            sAppName = "PGE3 - Live Disk Treemap Visualizer";
        }

        ~DiskTreemapAnalyzer() override {
            // Hintergrund-Thread sauber stoppen
            m_shared.abortScanRequested = true;
            if (m_scanThread.joinable()) {
                m_scanThread.join();
            }
        }

    private:
        // --- Bildschirm- & Canvas-Konfiguration ---
        const olc::vi2d m_screenSize = { 1280, 720 };
        const olc::vf2d WORLD_CANVAS_SIZE = { 1000.0f, 1000.0f };

        // --- Threading & Daten ---
        SharedScanContext m_shared;
        std::thread m_scanThread;
        std::unique_ptr<FileNode> m_renderRoot = nullptr;

        // --- Interaktions- & Kamera-Zustand (Nur Render-Thread) ---
        olc::vf2d m_cameraOffset = { 0.0f, 0.0f };
        float m_cameraZoom       = 1.0f;
        olc::vi2d m_lastMousePos = { 0, 0 };
        const FileNode* m_hoveredNode = nullptr;

        // Aktualisierungsintervall für das Layout während des Scans
        std::chrono::steady_clock::time_point m_lastLayoutUpdate;
        const std::chrono::milliseconds LAYOUT_REFRESH_RATE{ 400 };

    public:
        bool OnUserCreate() override {
            m_lastLayoutUpdate = std::chrono::steady_clock::now();
            m_lastMousePos = mouse.GetPosition();

            // Scan im aktuellen Arbeitsverzeichnis starten
            StartBackgroundScan(fs::current_path());
            return true;
        }

        bool OnUserUpdate(float fElapsedTime) override {
            HandleInput(fElapsedTime);
            CheckAndRebuildLayout();

            // --------------------------------------------------------------------
            // RENDERING PIPELINE (PGE3 Hardware Draw Interface)
            // --------------------------------------------------------------------
            draw.Clear(olc::Pixel(20, 24, 30));

            // 1. Affine Welt-Transformation auf die PGE3-Draw-Pipeline anwenden
            draw.WorldReset();
            draw.WorldScale({ m_cameraZoom, m_cameraZoom });
            draw.WorldOffset(m_cameraOffset);

            // 2. Treemap rekursiv zeichnen
            m_hoveredNode = nullptr;
            olc::vf2d mouseWorld = draw.ScreenToWorld(mouse.GetPosition());

            if (m_renderRoot) {
                RenderNode(m_renderRoot.get(), mouseWorld);
            }

            // 3. UI-HUD im Screen-Space zeichnen (Transformationen zurücksetzen)
            draw.WorldReset();
            RenderHUD();

            return true;
        }

    private:
        // ========================================================================
        // WORKER-THREAD: DATEISYSTEM-SCAN
        // ========================================================================
        void StartBackgroundScan(const fs::path& targetDirectory) {
            if (m_shared.isScanning) return;

            m_shared.isScanning = true;
            m_shared.abortScanRequested = false;

            m_scanThread = std::thread([this, targetDirectory]() {
                auto root = std::make_unique<FileNode>();
                root->path = targetDirectory;
                root->name = targetDirectory.filename().string();
                root->isDirectory = true;

                {
                    std::lock_guard<std::mutex> lock(m_shared.treeMutex);
                    m_shared.rootNode = std::make_unique<FileNode>();
                    m_shared.rootNode->path = root->path;
                    m_shared.rootNode->name = root->name;
                    m_shared.rootNode->isDirectory = true;
                }

                ScanDirectoryRecursive(targetDirectory, root.get());

                if (!m_shared.abortScanRequested) {
                    std::lock_guard<std::mutex> lock(m_shared.treeMutex);
                    m_shared.rootNode = std::move(root);
                    m_shared.hasNewDataForLayout = true;
                }

                m_shared.isScanning = false;
            });
        }

        void ScanDirectoryRecursive(const fs::path& currentPath, FileNode* parentNode) {
            try {
                for (const auto& entry : fs::directory_iterator(currentPath, fs::directory_options::skip_permission_denied)) {
                    if (m_shared.abortScanRequested) return;

                    auto child = std::make_unique<FileNode>();
                    child->path = entry.path();
                    child->name = entry.path().filename().string();
                    child->isDirectory = entry.is_directory();

                    if (child->isDirectory) {
                        ScanDirectoryRecursive(entry.path(), child.get());
                    } else {
                        try {
                            child->sizeBytes = entry.file_size();
                        } catch (...) {
                            child->sizeBytes = 0;
                        }
                    }

                    parentNode->sizeBytes = parentNode->sizeBytes + child->sizeBytes;

                    ++m_shared.totalFilesScanned;
                    m_shared.totalBytesScanned += child->sizeBytes;

                    parentNode->children.push_back(std::move(child));

                    if (m_shared.totalFilesScanned % 250 == 0) {
                        std::lock_guard<std::mutex> lock(m_shared.treeMutex);
                        m_shared.currentPathInspected = entry.path().string();
                        m_shared.hasNewDataForLayout = true;
                    }
                }
            } catch (...) {
                // Ignoriere unzureichende Berechtigungen auf Linux
            }
        }

        // ========================================================================
        // LAYOUT-GENERIERUNG
        // ========================================================================
        void CheckAndRebuildLayout() {
            const auto now = std::chrono::steady_clock::now();
            const bool timeElapsed = (now - m_lastLayoutUpdate) > LAYOUT_REFRESH_RATE;

            if (m_shared.hasNewDataForLayout && (timeElapsed || !m_shared.isScanning)) {
                m_shared.hasNewDataForLayout = false;
                m_lastLayoutUpdate = now;

                std::unique_ptr<FileNode> treeSnapshot = nullptr;
                {
                    std::lock_guard<std::mutex> lock(m_shared.treeMutex);
                    if (m_shared.rootNode) {
                        treeSnapshot = DeepCopyTree(m_shared.rootNode.get());
                    }
                }

                if (treeSnapshot) {
                    treeSnapshot->visualPos  = { 0.0f, 0.0f };
                    treeSnapshot->visualSize = WORLD_CANVAS_SIZE;
                    CalculateTreemapLayout(treeSnapshot.get(), treeSnapshot->visualPos, treeSnapshot->visualSize, 0);
                    m_renderRoot = std::move(treeSnapshot);
                }
            }
        }

        void CalculateTreemapLayout(FileNode* node, olc::vf2d pos, olc::vf2d size, int depth) {
            if (!node || node->children.empty() || node->sizeBytes == 0) return;

            uint8_t r = (depth * 45 + 70) % 255;
            uint8_t g = (depth * 85 + 100) % 255;
            uint8_t b = (depth * 125 + 130) % 255;
            node->color = olc::Pixel(r, g, b);

            bool splitVertical = size.x >= size.y;
            float currentOffset = 0.0f;

            for (auto& child : node->children) {
                if (child->sizeBytes == 0) continue;

                const float ratio = static_cast<float>(child->sizeBytes) / static_cast<float>(node->sizeBytes);

                if (splitVertical) {
                    float childWidth = size.x * ratio;
                    child->visualPos  = { pos.x + currentOffset, pos.y };
                    child->visualSize = { childWidth, size.y };
                    currentOffset += childWidth;
                } else {
                    float childHeight = size.y * ratio;
                    child->visualPos  = { pos.x, pos.y + currentOffset };
                    child->visualSize = { size.x, childHeight };
                    currentOffset += childHeight;
                }

                CalculateTreemapLayout(child.get(), child->visualPos, child->visualSize, depth + 1);
            }
        }

        static std::unique_ptr<FileNode> DeepCopyTree(const FileNode* source) {
            if (!source) return nullptr;
            auto copy = std::make_unique<FileNode>();
            copy->path = source->path;
            copy->name = source->name;
            copy->sizeBytes = source->sizeBytes;
            copy->isDirectory = source->isDirectory;
            for (const auto& child : source->children) {
                copy->children.push_back(DeepCopyTree(child.get()));
            }
            return copy;
        }

        // ========================================================================
        // EINGABE & INTERAKTION (PGE3 hw::Mouse & hw::Keyboard)
        // ========================================================================
        void HandleInput(float fElapsedTime) {
            // PGE3 Mouse Buttons: 0 = Links, 1 = Rechts, 2 = Mitte
            if (mouse.GetButton(0).bHeld || mouse.GetButton(2).bHeld) {
                olc::vi2d delta = mouse.GetPosition() - m_lastMousePos;
                m_cameraOffset += olc::vf2d(delta) / m_cameraZoom;
            }
            m_lastMousePos = mouse.GetPosition();

            // Zoom über Mausrad mit Erhalt des Fokuspunktes
            int wheel = mouse.GetWheel();
            if (wheel != 0) {
                olc::vf2d mouseBeforeZoom = draw.ScreenToWorld(mouse.GetPosition());
                if (wheel > 0) m_cameraZoom *= 1.15f;
                if (wheel < 0) m_cameraZoom /= 1.15f;
                m_cameraZoom = std::clamp(m_cameraZoom, 0.05f, 100.0f);

                olc::vf2d mouseAfterZoom = draw.ScreenToWorld(mouse.GetPosition());
                m_cameraOffset += (mouseAfterZoom - mouseBeforeZoom);
            }

            // Reset-Kamera mit Leertaste
            if (keyboard.GetKey(olc::Key::SPACE).bPressed) {
                m_cameraOffset = { 0.0f, 0.0f };
                m_cameraZoom   = 1.0f;
            }
        }

        // ========================================================================
        // RENDERING (PGE3 Hardware Draw Interface)
        // ========================================================================
        void RenderNode(const FileNode* node, const olc::vf2d& mouseWorld) {
            if (!node) return;

            // Culling für zu kleine Elemente
            if (node->visualSize.x * m_cameraZoom < 1.5f || node->visualSize.y * m_cameraZoom < 1.5f) {
                return;
            }

            if (node->children.empty()) {
                draw.FilledRect(node->visualPos, node->visualSize, node->color, olc::Colour::WHITE);
                draw.Rect(node->visualPos, node->visualSize, olc::Pixel(10, 10, 10, 180));
            } else {
                for (const auto& child : node->children) {
                    RenderNode(child.get(), mouseWorld);
                }
                draw.Rect(node->visualPos, node->visualSize, olc::Colour::WHITE);
            }

            // Hover-Abfrage im World-Space
            if (mouseWorld.x >= node->visualPos.x && mouseWorld.x <= (node->visualPos.x + node->visualSize.x) &&
                mouseWorld.y >= node->visualPos.y && mouseWorld.y <= (node->visualPos.y + node->visualSize.y)) {
                m_hoveredNode = node;
            }

            // Text bei ausreichender Größe einblenden
            if (node->visualSize.x * m_cameraZoom > 60.0f && node->visualSize.y * m_cameraZoom > 20.0f) {
                float invZoom = 1.0f / m_cameraZoom;
                draw.String(node->visualPos + olc::vf2d{ 4.0f, 4.0f }, node->name,  olc::Colour::BLACK, { invZoom, invZoom });
            }
        }

        void RenderHUD() {
            // Statusleiste
            olc::vf2d barPos = { 0.0f, 0.0f };
            olc::vf2d barSize = { static_cast<float>(m_screenSize.x), 45.0f };
            draw.FilledRect(barPos, barSize, olc::Pixel(15, 18, 22, 230), olc::Colour::WHITE);

            std::string scanStatus = m_shared.isScanning ? "SCANNING..." : "SCAN FINISHED";
            olc::Pixel statusColor = m_shared.isScanning ? olc::Colour::YELLOW : olc::Colour::GREEN;

            draw.String({ 10.0f, 8.0f }, scanStatus, statusColor);
            draw.String({ 150.0f, 8.0f }, "Files: " + std::to_string(m_shared.totalFilesScanned.load()), olc::Colour::WHITE);
            draw.String({ 320.0f, 8.0f }, "Size: " + FormatBytes(m_shared.totalBytesScanned.load()), olc::Colour::CYAN);

            if (m_hoveredNode) {
                std::string hoverInfo = m_hoveredNode->name + " (" + FormatBytes(m_hoveredNode->sizeBytes) + ")";
                draw.String({ 10.0f, 26.0f }, hoverInfo, olc::Colour::WHITE);
            } else {
                draw.String({ 10.0f, 26.0f }, "Pan: Left/Middle Drag | Zoom: Wheel | Reset: Space", olc::Colour::DARK_GREY);
            }
        }

        static std::string FormatBytes(uintmax_t bytes) {
            const char* suffixes[] = { "B", "KB", "MB", "GB", "TB" };
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
}

// ============================================================================
// MAIN FUNCTION & PGE3 CONSTRUCT
// ============================================================================
int main() {
    DiskTreemapAnalyzer demo;

    // PGE3 Konstruktor mit Screen- und Pixel-Größe
    if (demo.Construct({ 1280, 720 }, { 1, 1 }, false)) {
        demo.Start();
    }
    return 0;
}