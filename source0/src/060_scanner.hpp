#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "010_types.hpp"
#include "020_color_utils.hpp"
#include "030_thread_pool.hpp"
#include "040_file_watcher.hpp"
#include "050_treemap_layout.hpp"

namespace pge_treemap {

namespace fs = std::filesystem;

class DirectoryScanner
{
public:
    DirectoryScanner(fs::path targetPath, SharedScanContext& shared, LayoutThreadPool& layoutPool,
                     FileWatcher& watcher, const vf2d canvasSize)
        : m_targetPath(std::move(targetPath)), m_shared(shared), m_layoutPool(layoutPool), m_watcher(watcher), m_canvasSize(canvasSize)
    {
        m_workerThread = std::thread([this]() { WorkerLoop(); });
    }

    ~DirectoryScanner()
    {
        Stop();
    }

    void Stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_scanRequested = false;
        }
        m_cv.notify_all();

        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }
    }

    void RequestRescan()
    {
        {
            std::lock_guard lock(m_mutex);
            m_scanRequested = true;
        }
        m_cv.notify_one();
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

private:
    void WorkerLoop()
    {
        while (!m_shared.abortScanRequested)
        {
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this]() {
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

            m_watcher.AddWatch(m_targetPath);
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
                    m_watcher.AddWatch(entry.path());
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

    void PublishSnapshot(const std::string& currentInspectedPath) const
    {
        if (!m_workerRoot || m_shared.abortScanRequested) return;

        auto snapshot = DeepCopyTree(m_workerRoot.get());
        if (!snapshot || snapshot->sizeBytes == 0 || m_shared.abortScanRequested) return;

        snapshot->visualPos  = {0.0f, 0.0f};
        snapshot->visualSize = m_canvasSize;

        TreemapLayout::CalculateParallel(snapshot.get(), snapshot->visualPos, snapshot->visualSize,
                                         &m_layoutPool, m_shared.abortScanRequested);

        if (m_shared.abortScanRequested) return;

        {
            std::lock_guard lock(m_shared.readySnapshotMutex);
            m_shared.readyRenderTree      = std::move(snapshot);
            m_shared.currentPathInspected = currentInspectedPath;
            m_shared.hasNewDataForRender.store(true, std::memory_order_release);
        }
    }

    fs::path           m_targetPath;
    SharedScanContext& m_shared;
    LayoutThreadPool&  m_layoutPool;
    FileWatcher&       m_watcher;
    vf2d               m_canvasSize;

    std::thread             m_workerThread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_scanRequested = true;

    std::unique_ptr<FileNode>             m_workerRoot = nullptr;
    std::chrono::steady_clock::time_point m_lastPublishTime;
    uint32_t                              m_filesSinceLastPublishCheck = 0;
};

} // namespace pge_treemap
