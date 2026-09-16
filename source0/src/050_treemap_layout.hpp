#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <vector>

#include "010_types.hpp"
#include "030_thread_pool.hpp"

namespace pge_treemap {

class TreemapLayout
{
public:
    static void CalculateParallel(FileNode* root, const vf2d pos, const vf2d size,
                                  LayoutThreadPool* pool, const std::atomic<bool>& abortFlag)
    {
        if (!root || root->sizeBytes == 0) return;
        root->visualPos  = pos;
        root->visualSize = size;
        if (size.x < 0.5f || size.y < 0.5f || root->children.empty()) return;

        const size_t maxParallelTasks = pool ? pool->ThreadCount() * 4 : 1;

        std::atomic<int64_t>    activeTasks{1};
        std::mutex              compMutex;
        std::condition_variable compCv;

        auto executeTask = [&](auto& self, FileNode* node) -> void {
            LayoutSubtreeRecursive(node, true, maxParallelTasks, pool, abortFlag, activeTasks, self);
            if (activeTasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                std::lock_guard lock(compMutex);
                compCv.notify_all();
            }
        };

        if (pool && pool->ThreadCount() > 1)
        {
            pool->Enqueue([&]() { executeTask(executeTask, root); });

            std::unique_lock lock(compMutex);
            compCv.wait(lock, [&]() {
                return activeTasks.load(std::memory_order_acquire) == 0;
            });
        }
        else
        {
            executeTask(executeTask, root);
        }
    }

private:
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
            if (child && child->sizeBytes > 0) totalBytes += child->sizeBytes;
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

    template<typename TaskFunc>
    static void LayoutSubtreeRecursive(FileNode* node, const bool allowFork, const size_t maxParallelTasks,
                                       LayoutThreadPool* pool, const std::atomic<bool>& abortFlag,
                                       std::atomic<int64_t>& activeTasks, TaskFunc&& executeTask)
    {
        if (abortFlag.load(std::memory_order_relaxed)) return;

        for (const std::vector<FileNode*> eligibleChildren = LayoutDirectChildren(node); FileNode* child: eligibleChildren)
        {
            if (abortFlag.load(std::memory_order_relaxed)) return;

            if (const bool shouldFork = allowFork && (child->children.size() >= 4) && (activeTasks.load(std::memory_order_relaxed) < static_cast<int64_t>(maxParallelTasks)); shouldFork && pool)
            {
                activeTasks.fetch_add(1, std::memory_order_release);
                pool->Enqueue([&executeTask, child]() {
                    executeTask(executeTask, child);
                });
            }
            else
            {
                LayoutSubtreeRecursive(child, false, maxParallelTasks, pool, abortFlag, activeTasks, executeTask);
            }
        }
    }
};

} // namespace pge_treemap
