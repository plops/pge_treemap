#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
        std::vector<std::unique_ptr<FileNode> > children;

        // Koordinaten im virtuellen Welt-Raum (wird vom Treemap-Algorithmus berechnet)
        vf2d visualPos = {0.0f, 0.0f};
        vf2d visualSize = {0.0f, 0.0f};
        Pixel color = Colour::WHITE;
    };
} // namespace

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
        std::atomic<bool> isScanning{false};
        std::atomic<bool> abortScanRequested{false};
        std::atomic<uintmax_t> totalFilesScanned{0};
        std::atomic<uintmax_t> totalBytesScanned{0};

        // Signalisiert dem Render-Thread, dass neue Daten zur Verfügung stehen
        std::atomic<bool> hasNewDataForLayout{false};
    };
} // namespace

// ============================================================================
// 3. ENGINE IMPLEMENTIERUNG (PGE3 INTERFACE-BASIERT)
// ============================================================================
namespace {
    // Erzeugt kräftige RGB-Farben aus HSL (0.0f - 1.0f)
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

    // Ordnet Dateien nach Kategorie Farben zu (WinDirStat-Style)
    Pixel GetColorForFile(const fs::path &filePath) {
        std::string ext = filePath.extension().string();
        std::ranges::transform(ext, ext.begin(), tolower);

        // Medien / Video
        if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov")
            return {185, 60, 220}; // Lila

        // Audio
        if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg")
            return {240, 205, 35}; // Goldgelb

        // Bilder
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif")
            return {30, 190, 230}; // Cyan

        // Archive / Komprimiert
        if (ext == ".zip" || ext == ".rar" || ext == ".7z" || ext == ".tar" || ext == ".gz")
            return {235, 50, 50}; // Rot

        // Code & Text
        if (ext == ".cpp" || ext == ".h" || ext == ".rs" || ext == ".py" || ext == ".js" || ext == ".txt" || ext ==
            ".md")
            return {40, 210, 110}; // Smaragdgrün

        // Ausführbar / System
        if (ext == ".exe" || ext == ".dll" || ext == ".so" || ext == ".bin")
            return {60, 100, 240}; // Königsblau

        // Fallback: Deterministischer Farbton über Hash der Dateiendung
        if (!ext.empty()) {
            const size_t hash = std::hash<std::string>{}(ext);
            const float hue = static_cast<float>(hash % 360) / 360.0f;
            return HSLtoPixel(hue, 0.70f, 0.55f);
        }

        return {120, 130, 140}; // Dateien ohne Endung
    }


    class DiskTreemapAnalyzer : public PixelGameEngine {
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
        const vi2d m_screenSize = {1280, 720};
        const vf2d WORLD_CANVAS_SIZE = {1000.0f, 1000.0f};

        // --- Threading & Daten ---
        SharedScanContext m_shared;
        std::thread m_scanThread;
        std::unique_ptr<FileNode> m_renderRoot = nullptr;

        // --- Interaktions- & Kamera-Zustand (Nur Render-Thread) ---
        vf2d m_cameraOffset = {0.0f, 0.0f};
        float m_cameraZoom = 1.0f;
        vi2d m_lastMousePos = {0, 0};
        const FileNode *m_hoveredNode = nullptr;

        // Aktualisierungsintervall für das Layout während des Scans
        std::chrono::steady_clock::time_point m_lastLayoutUpdate;
        const std::chrono::milliseconds LAYOUT_REFRESH_RATE{400};

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
            draw.Clear(Pixel(20, 24, 30));

            // 1. Affine Welt-Transformation auf die PGE3-Draw-Pipeline anwenden
            draw.WorldReset();
            draw.WorldScale({m_cameraZoom, m_cameraZoom});
            draw.WorldOffset(m_cameraOffset);

            // 2. Treemap rekursiv zeichnen
            m_hoveredNode = nullptr;
            const vf2d mouseWorld = draw.ScreenToWorld(mouse.GetPosition());

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
        void StartBackgroundScan(const fs::path &targetDirectory) {
            if (m_shared.isScanning) return;

            m_shared.isScanning = true;
            m_shared.abortScanRequested = false;

            m_scanThread = std::thread([this, targetDirectory]() {
                auto root = std::make_unique<FileNode>();
                root->path = targetDirectory;
                root->name = targetDirectory.filename().string();
                root->isDirectory = true;

                {
                    std::lock_guard lock(m_shared.treeMutex);
                    m_shared.rootNode = std::make_unique<FileNode>();
                    m_shared.rootNode->path = root->path;
                    m_shared.rootNode->name = root->name;
                    m_shared.rootNode->isDirectory = true;
                }

                ScanDirectoryRecursive(targetDirectory, root.get());

                if (!m_shared.abortScanRequested) {
                    std::lock_guard lock(m_shared.treeMutex);
                    m_shared.rootNode = std::move(root);
                    m_shared.hasNewDataForLayout = true;
                }

                m_shared.isScanning = false;
            });
        }

        void ScanDirectoryRecursive(const fs::path &currentPath, FileNode *parentNode) {
            try {
                for (const auto &entry: fs::directory_iterator(currentPath,
                                                               fs::directory_options::skip_permission_denied)) {
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
                        std::lock_guard lock(m_shared.treeMutex);
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

            if (const bool timeElapsed = (now - m_lastLayoutUpdate) > LAYOUT_REFRESH_RATE;
                m_shared.hasNewDataForLayout && (timeElapsed || !m_shared.isScanning)) {
                m_shared.hasNewDataForLayout = false;
                m_lastLayoutUpdate = now;

                std::unique_ptr<FileNode> treeSnapshot = nullptr;
                {
                    std::lock_guard lock(m_shared.treeMutex);
                    if (m_shared.rootNode) {
                        treeSnapshot = DeepCopyTree(m_shared.rootNode.get());
                    }
                }

                if (treeSnapshot) {
                    treeSnapshot->visualPos = {0.0f, 0.0f};
                    treeSnapshot->visualSize = WORLD_CANVAS_SIZE;
                    CalculateTreemapLayout(treeSnapshot.get(), treeSnapshot->visualPos, treeSnapshot->visualSize, 0);
                    m_renderRoot = std::move(treeSnapshot);
                }
            }
        }

        static void CalculateTreemapLayout(FileNode *node, vf2d pos, vf2d size, const int depth) {
            if (!node || node->sizeBytes == 0) return;

            // WICHTIG: Farbe hier für Blätter (Dateien) vergeben!
            if (!node->isDirectory) {
                node->color = GetColorForFile(node->path);
            }

            if (node->children.empty()) return;

            const bool splitVertical = size.x >= size.y;
            float currentOffset = 0.0f;

            for (auto &child: node->children) {
                if (child->sizeBytes == 0) continue;

                const float ratio = static_cast<float>(child->sizeBytes) / static_cast<float>(node->sizeBytes);

                if (splitVertical) {
                    float childWidth = size.x * ratio;
                    child->visualPos = {pos.x + currentOffset, pos.y};
                    child->visualSize = {childWidth, size.y};
                    currentOffset += childWidth;
                } else {
                    float childHeight = size.y * ratio;
                    child->visualPos = {pos.x, pos.y + currentOffset};
                    child->visualSize = {size.x, childHeight};
                    currentOffset += childHeight;
                }

                CalculateTreemapLayout(child.get(), child->visualPos, child->visualSize, depth + 1);
            }
        }


        static std::unique_ptr<FileNode> DeepCopyTree(const FileNode *source) {
            if (!source) return nullptr;
            auto copy = std::make_unique<FileNode>();
            copy->path = source->path;
            copy->name = source->name;
            copy->sizeBytes = source->sizeBytes;
            copy->isDirectory = source->isDirectory;
            for (const auto &child: source->children) {
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
                const vi2d delta = mouse.GetPosition() - m_lastMousePos;
                m_cameraOffset += vf2d(delta) / m_cameraZoom;
            }
            m_lastMousePos = mouse.GetPosition();

            // Zoom über Mausrad mit Erhalt des Fokuspunktes
            if (const int wheel = mouse.GetWheel(); wheel != 0) {
                const vf2d mouseBeforeZoom = draw.ScreenToWorld(mouse.GetPosition());
                if (wheel > 0) m_cameraZoom *= 1.15f;
                if (wheel < 0) m_cameraZoom /= 1.15f;
                m_cameraZoom = std::clamp(m_cameraZoom, 0.05f, 100.0f);

                const vf2d mouseAfterZoom = draw.ScreenToWorld(mouse.GetPosition());
                m_cameraOffset += mouseAfterZoom - mouseBeforeZoom;
            }

            // Reset-Kamera mit Leertaste
            if (keyboard.GetKey(Key::SPACE).bPressed) {
                m_cameraOffset = {0.0f, 0.0f};
                m_cameraZoom = 1.0f;
            }
        }

        void DrawCushionRect(const vf2d &pos, const vf2d &size, const Pixel &baseCol) {
            // Basis-Fläche
            draw.FilledRect(pos, size, baseCol);

            // Kissen-Effekt lohnt sich erst ab einer gewissen Mindestgröße
            const float screenW = size.x * m_cameraZoom;
            if (const float screenH = size.y * m_cameraZoom; screenW < 4.0f || screenH < 4.0f) return;

            // Berechne adaptive Randbreite für die Rundung
            float bevel = std::clamp(std::min(size.x, size.y) * 0.15f, 1.0f, 8.0f);

            // 1. Lichtkante oben und links (Licht kommt von oben-links)
            // 50% weißes Glanzlicht
            constexpr Pixel highlight(255, 255, 255, 80);
            draw.FilledRect(pos, {size.x, bevel}, highlight); // Oben
            draw.FilledRect(pos, {bevel, size.y}, highlight); // Links

            // 2. Schattenkante unten und rechts
            // 60% schwarzer Eigenschatten
            constexpr Pixel shadow(0, 0, 0, 110);
            draw.FilledRect({pos.x, pos.y + size.y - bevel}, {size.x, bevel}, shadow); // Unten
            draw.FilledRect({pos.x + size.x - bevel, pos.y}, {bevel, size.y}, shadow); // Rechts

            // 3. Zweiter innerer Schein für weichere Wölbung (wenn groß genug)
            if (bevel >= 3.0f) {
                constexpr Pixel softLight(255, 255, 255, 35);
                draw.FilledRect({pos.x + bevel, pos.y + bevel}, {size.x - 2 * bevel, bevel * 0.6f}, softLight);
                draw.FilledRect({pos.x + bevel, pos.y + bevel}, {bevel * 0.6f, size.y - 2 * bevel}, softLight);
            }

            // 4. Feine dunkle Trennfuge
            draw.Rect(pos, size, Pixel(15, 15, 20, 200));
        }

        // ========================================================================
        // RENDERING (PGE3 Hardware Draw Interface)
        // ========================================================================
        void RenderNode(const FileNode *node, const vf2d &mouseWorld) {
            if (!node) return;

            // Culling für zu kleine Elemente
            if (node->visualSize.x * m_cameraZoom < 1.0f || node->visualSize.y * m_cameraZoom < 1.0f) {
                return;
            }

            // Wenn Blattknoten (Datei) -> Cushion zeichnen
            if (node->children.empty()) {
                DrawCushionRect(node->visualPos, node->visualSize, node->color);
            } else {
                // Ordner: Kinder rekursiv zeichnen
                for (const auto &child: node->children) {
                    RenderNode(child.get(), mouseWorld);
                }
                // Dezenter Ordner-Rahmen für visuelle Hierarchie
                draw.Rect(node->visualPos, node->visualSize, Pixel(0, 0, 0, 160));
            }

            // Hover-Abfrage im World-Space
            if (mouseWorld.x >= node->visualPos.x && mouseWorld.x <= (node->visualPos.x + node->visualSize.x) &&
                mouseWorld.y >= node->visualPos.y && mouseWorld.y <= (node->visualPos.y + node->visualSize.y)) {
                m_hoveredNode = node;
            }

            // Beschriftung nur anzeigen, wenn Platz ausreicht
            if (node->visualSize.x * m_cameraZoom > 70.0f && node->visualSize.y * m_cameraZoom > 22.0f) {
                float invZoom = 1.0f / m_cameraZoom;
                // Text mit leichtem Schatten für Lesbarkeit auf Kissenoberflächen
                draw.String(node->visualPos + vf2d{5.0f, 5.0f}, node->name, Colour::BLACK, {invZoom, invZoom});
                draw.String(node->visualPos + vf2d{4.0f, 4.0f}, node->name, Colour::WHITE, {invZoom, invZoom});
            }
        }

        void RenderHUD() {
            // Statusleiste
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
} // namespace

// ============================================================================
// MAIN FUNCTION & PGE3 CONSTRUCT
// ============================================================================
int main() {
    // PGE3 Konstruktor mit Screen- und Pixel-Größe
    if (DiskTreemapAnalyzer demo; demo.Construct({1280, 720}, {1, 1}, false)) {
        demo.Start();
    }
    return 0;
}
