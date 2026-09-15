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
#define OLC_PGE_APPLICATION
#include "olcPixelGameEngine3.h"

namespace fs = std::filesystem;

// ============================================================================
// 1. DATENMODELL & HIERARCHIE
// ============================================================================

struct FileNode {
    fs::path path;
    std::string name;
    uintmax_t sizeBytes = 0;
    bool isDirectory = false;
    std::vector<std::unique_ptr<FileNode>> children;

    // Koordinaten im virtuellen Welt-Raum (wird vom Treemap-Algorithmus berechnet)
    olc::vf2d visualPos  = { 0.0f, 0.0f };
    olc::vf2d visualSize = { 0.0f, 0.0f };
    olc::Pixel color     = olc::WHITE;
};

// ============================================================================
// 2. THREAD-SHARED STATE (Klar getrennt vom UI-/Render-Zustand)
// ============================================================================

struct SharedScanContext {
    // Dieser Mutex schützt AUSSCHLIESSLICH den Zugriff auf den Datenbaum
    // und Strings, die während des Scans dynamisch wachsen.
    mutable std::mutex treeMutex;
    std::unique_ptr<FileNode> rootNode = nullptr;
    std::string currentPathInspected   = "";

    // Lock-freie Status-Variablen für das UI
    std::atomic<bool> isScanning{ false };
    std::atomic<bool> abortScanRequested{ false };
    std::atomic<uintmax_t> totalFilesScanned{ 0 };
    std::atomic<uintmax_t> totalBytesScanned{ 0 };

    // Signalisiert dem Render-Thread, dass neue Daten zur Verfügung stehen
    std::atomic<bool> hasNewDataForLayout{ false };
};

// ============================================================================
// 3. ENGINE IMPLEMENTIERUNG (PGE3 INTERFACE-BASIERT)
// ============================================================================

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
    // --- Threading & Daten ---
    SharedScanContext m_shared;
    std::thread m_scanThread;
    std::unique_ptr<FileNode> m_renderRoot = nullptr; // Lokaler Schnappschuss für den Render-Thread

    // --- Interaktions- & Kamera-Zustand (Nur Render-Thread) ---
    olc::vf2d m_cameraOffset = { 0.0f, 0.0f };
    float m_cameraZoom       = 1.0f;
    olc::vi2d m_lastMousePos = { 0, 0 };
    const FileNode* m_hoveredNode = nullptr;

    // Aktualisierungsintervall für das Layout während des Scans (z. B. alle 500 ms)
    std::chrono::steady_clock::time_point m_lastLayoutUpdate;
    const std::chrono::milliseconds LAYOUT_REFRESH_RATE{ 400 };

    const olc::vf2d WORLD_CANVAS_SIZE = { 1000.0f, 1000.0f };

public:
    bool OnUserCreate() override {
        m_lastLayoutUpdate = std::chrono::steady_clock::now();

        // Starte Scan im Standard-Ordner (z.B. Home oder aktuelles Verzeichnis)
        StartBackgroundScan(fs::current_path());
        return true;
    }

    bool OnUserUpdate(float fElapsedTime) override {
        HandleInput(fElapsedTime);
        CheckAndRebuildLayout();

        // --------------------------------------------------------------------
        // RENDERING PIPELINE (PGE3 Native Hardware Acceleration)
        // --------------------------------------------------------------------
        draw.Clear(olc::Pixel(20, 24, 30));

        // 1. Affine Welt-Transformation auf die PGE3-Draw-Pipeline anwenden
        draw.WorldReset();
        draw.WorldScale({ m_cameraZoom, m_cameraZoom });
        draw.WorldOffset(m_cameraOffset);

        // 2. Treemap rekursiv zeichnen
        m_hoveredNode = nullptr;
        olc::vf2d mouseWorld = draw.ScreenToWorld(mouse.GetPos());

        if (m_renderRoot) {
            RenderNode(m_renderRoot.get(), mouseWorld);
        }

        // 3. UI-HUD im Screen-Space zeichnen (Affine Transformationen zurücksetzen)
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

            // Root sofort im Shared-State registrieren, damit das Layout initial beginnen kann
            {
                std::lock_guard<std::mutex> lock(m_shared.treeMutex);
                m_shared.rootNode = std::make_unique<FileNode>();
                m_shared.rootNode->path = root->path;
                m_shared.rootNode->name = root->name;
                m_shared.rootNode->isDirectory = true;
            }

            ScanDirectoryRecursive(targetDirectory, root.get());

            // Scan beendet: Finale Übergabe des kompletten Baums
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

                parentNode->sizeBytes += child->sizeBytes;

                // Status-Atomics updaten
                m_shared.totalFilesScanned++;
                m_shared.totalBytesScanned += child->sizeBytes;

                parentNode->children.push_back(std::move(child));

                // Zwischendurch den Render-Thread benachrichtigen (nicht bei jeder einzelnen Datei)
                if (m_shared.totalFilesScanned % 250 == 0) {
                    std::lock_guard<std::mutex> lock(m_shared.treeMutex);
                    m_shared.currentPathInspected = entry.path().string();
                    m_shared.hasNewDataForLayout = true;
                }
            }
        } catch (...) {
            // Ignoriere unzureichende Berechtigungen auf Linux (/proc, /root etc.)
        }
    }

    // ========================================================================
    // LAYOUT-GENERIERUNG (Alternating-Axis Bisection)
    // ========================================================================
    void CheckAndRebuildLayout() {
        auto now = std::chrono::steady_clock::now();
        bool timeElapsed = (now - m_lastLayoutUpdate) > LAYOUT_REFRESH_RATE;

        if (m_shared.hasNewDataForLayout && (timeElapsed || !m_shared.isScanning)) {
            m_shared.hasNewDataForLayout = false;
            m_lastLayoutUpdate = now;

            // Schnappschuss synchronisieren (Minimaler Lock)
            std::unique_ptr<FileNode> treeSnapshot = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_shared.treeMutex);
                if (m_shared.rootNode) {
                    treeSnapshot = DeepCopyTree(m_shared.rootNode.get());
                }
            }

            if (treeSnapshot) {
                // Layout im virtuellen Raum [0, 0] bis [WORLD_CANVAS_SIZE] berechnen
                treeSnapshot->visualPos  = { 0.0f, 0.0f };
                treeSnapshot->visualSize = WORLD_CANVAS_SIZE;
                CalculateTreemapLayout(treeSnapshot.get(), treeSnapshot->visualPos, treeSnapshot->visualSize, 0);

                m_renderRoot = std::move(treeSnapshot);
            }
        }
    }

    void CalculateTreemapLayout(FileNode* node, olc::vf2d pos, olc::vf2d size, int depth) {
        if (!node || node->children.empty() || node->sizeBytes == 0) return;

        // Farben basierend auf Tiefe und Typ zuweisen
        uint8_t r = (depth * 45 + 70) % 255;
        uint8_t g = (depth * 85 + 100) % 255;
        uint8_t b = (depth * 125 + 130) % 255;
        node->color = olc::Pixel(r, g, b);

        // Schneide entlang der längeren Kante (Slice and Dice)
        bool splitVertical = size.x >= size.y;
        float currentOffset = 0.0f;

        for (auto& child : node->children) {
            if (child->sizeBytes == 0) continue;

            float ratio = static_cast<float>(child->sizeBytes) / static_cast<float>(node->sizeBytes);

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

            // Rekursion für Unterordner
            CalculateTreemapLayout(child.get(), child->visualPos, child->visualSize, depth + 1);
        }
    }

    std::unique_ptr<FileNode> DeepCopyTree(const FileNode* source) {
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
    // EINGABE & INTERAKTION (PGE3 Affine Controls)
    // ========================================================================
    void HandleInput(float fElapsedTime) {
        // Panning: Mittlere Maustaste oder Linksklick-Drag
        if (mouse.GetButton(olc::mouse::Button::Left).bHeld || mouse.GetButton(olc::mouse::Button::Middle).bHeld) {
            olc::vi2d delta = mouse.GetPos() - m_lastMousePos;
            m_cameraOffset += olc::vf2d(delta) / m_cameraZoom;
        }
        m_lastMousePos = mouse.GetPos();

        // Zoom: Mausrad zoomt auf die Cursor-Position in der Welt
        int wheel = mouse.GetWheel();
        if (wheel != 0) {
            olc::vf2d mouseBeforeZoom = draw.ScreenToWorld(mouse.GetPos());
            if (wheel > 0) m_cameraZoom *= 1.15f;
            if (wheel < 0) m_cameraZoom /= 1.15f;
            m_cameraZoom = std::clamp(m_cameraZoom, 0.05f, 100.0f);

            // Fokuspunkt unter der Maus beibehalten
            olc::vf2d mouseAfterZoom = draw.ScreenToWorld(mouse.GetPos());
            m_cameraOffset += (mouseAfterZoom - mouseBeforeZoom);
        }

        // Reset Taste
        if (keyboard.GetKey(olc::Key::SPACE).bPressed) {
            m_cameraOffset = { 0.0f, 0.0f };
            m_cameraZoom   = 1.0f;
        }
    }

    // ========================================================================
    // RENDERING DER RECHTECKE & TEXTE (PGE3 Hardware Draw Interface)
    // ========================================================================
    void RenderNode(const FileNode* node, const olc::vf2d& mouseWorld) {
        if (!node) return;

        // Culling: Überspringe Knoten, die zu klein sind, um dargestellt zu werden
        if (node->visualSize.x * m_cameraZoom < 1.5f || node->visualSize.y * m_cameraZoom < 1.5f) {
            return;
        }

        // Treemap-Blätter (Dateien) oder Ordner ohne Kinder füllen
        if (node->children.empty()) {
            draw.FilledRect(node->visualPos, node->visualSize, node->color);
            draw.Rect(node->visualPos, node->visualSize, olc::Pixel(10, 10, 10, 180));
        } else {
            // Für Ordner: Zeichne Kinder rekursiv
            for (const auto& child : node->children) {
                RenderNode(child.get(), mouseWorld);
            }
            // Umrandung um den Ordner
            draw.Rect(node->visualPos, node->visualSize, olc::WHITE);
        }

        // Hover-Detection im World-Space
        if (mouseWorld.x >= node->visualPos.x && mouseWorld.x <= (node->visualPos.x + node->visualSize.x) &&
            mouseWorld.y >= node->visualPos.y && mouseWorld.y <= (node->visualPos.y + node->visualSize.y)) {
            m_hoveredNode = node;
        }

        // Text nur zeichnen, wenn das Rechteck auf dem Bildschirm groß genug ist
        if (node->visualSize.x * m_cameraZoom > 60.0f && node->visualSize.y * m_cameraZoom > 20.0f) {
            // PGE3 Text Rendering
            draw.String(node->visualPos + olc::vf2d{ 4.0f, 4.0f }, node->name, olc::BLACK, 1.0f / m_cameraZoom);
        }
    }

    void RenderHUD() {
        // Statusleiste Hintergrund
        draw.FilledRect({ 0, 0 }, { (float)GetDrawTargetWidth(), 45.0f }, olc::Pixel(15, 18, 22, 230));

        // Scan-Fortschritt
        std::string scanStatus = m_shared.isScanning ? "SCANNING..." : "SCAN FINISHED";
        olc::Pixel statusColor = m_shared.isScanning ? olc::YELLOW : olc::GREEN;

        draw.String({ 10, 8 }, scanStatus, statusColor);
        draw.String({ 150, 8 }, "Files: " + std::to_string(m_shared.totalFilesScanned.load()), olc::WHITE);
        draw.String({ 320, 8 }, "Size: " + FormatBytes(m_shared.totalBytesScanned.load()), olc::CYAN);

        // Tooltip bei Mouse-Hover
        if (m_hoveredNode) {
            std::string hoverInfo = m_hoveredNode->name + " (" + FormatBytes(m_hoveredNode->sizeBytes) + ")";
            draw.String({ 10, 26 }, hoverInfo, olc::WHITE);
        } else {
            draw.String({ 10, 26 }, "Pan: Left/Middle Drag | Zoom: Mouse Wheel | Reset: Space", olc::DARK_GREY);
        }
    }

    std::string FormatBytes(uintmax_t bytes) const {
        const char* suffixes[] = { "B", "KB", "MB", "GB", "TB" };
        int i = 0;
        double dBytes = static_cast<double>(bytes);
        while (dBytes >= 1024.0 && i < 4) {
            dBytes /= 1024.0;
            i++;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f %s", dBytes, suffixes[i]);
        return std::string(buf);
    }
};

// ============================================================================
// MAIN FUNCTION & PGE3 CONFIGURATION
// ============================================================================
int main() {
    DiskTreemapAnalyzer demo;

    // In PGE3: Saubere Konfigurationsstruktur statt überladener Flags
    // (Fenster 1280x720, Pixelgröße 1x1, VSync aktiviert)
    if (demo.Construct(1280, 720, 1, 1, false, true)) {
        demo.Start();
    }
    return 0;
}
