#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pge_treemap {

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
    std::mutex                        m_taskQueueMutex;
    std::condition_variable           m_taskQueueCv;
    bool                              m_stopRequested = false;
};

} // namespace pge_treemap
