#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "olcPixelGameEngine3.h"

namespace pge_treemap {

struct FileNode {
    std::string                            name;
    uintmax_t                              sizeBytes   = 0;
    bool                                   isDirectory = false;
    std::vector<std::unique_ptr<FileNode>> children;

    vf2d  visualPos  = {0.0f, 0.0f};
    vf2d  visualSize = {0.0f, 0.0f};
    Pixel color      = Colour::WHITE;
};

// Synchronization state shared between background scanner and render thread
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

} // namespace pge_treemap
