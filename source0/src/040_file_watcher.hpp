#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(__linux__) && __has_include(<sys/inotify.h>)
#define HAS_INOTIFY 1
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace pge_treemap {

namespace fs = std::filesystem;

class FileWatcher
{
public:
    using ChangeCallback = std::function<void()>;

    explicit FileWatcher(ChangeCallback onChange)
        : m_onChange(std::move(onChange))
    {
#if defined(HAS_INOTIFY)
        m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotifyFd >= 0)
        {
            m_stopEventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (m_stopEventFd >= 0)
            {
                m_isActive    = true;
                m_watchThread = std::thread([this]() { EventLoop(); });
            }
        }
#endif
    }

    ~FileWatcher()
    {
        Stop();
    }

    void Stop()
    {
#if defined(HAS_INOTIFY)
        if (m_stopEventFd >= 0)
        {
            constexpr uint64_t        val = 1;
            [[maybe_unused]] auto s   = write(m_stopEventFd, &val, sizeof(val));
        }

        if (m_watchThread.joinable())
        {
            m_watchThread.join();
        }

        if (m_inotifyFd >= 0)
        {
            close(m_inotifyFd);
            m_inotifyFd = -1;
        }

        if (m_stopEventFd >= 0)
        {
            close(m_stopEventFd);
            m_stopEventFd = -1;
        }
        m_isActive = false;
#endif
    }

    [[nodiscard]] bool IsActive() const { return m_isActive; }

    void AddWatch(const fs::path& p)
    {
#if defined(HAS_INOTIFY)
        if (m_inotifyFd < 0) return;

        std::error_code   ec;
        const fs::path    canon   = fs::weakly_canonical(p, ec);
        const std::string pathStr = (!ec) ? canon.string() : p.lexically_normal().string();

        std::lock_guard lock(m_watchMutex);
        if (m_pathToWd.contains(pathStr)) return;

        constexpr uint32_t flags = IN_MODIFY | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB;
        if (const int wd = inotify_add_watch(m_inotifyFd, pathStr.c_str(), flags); wd >= 0)
        {
            m_wdToPath[wd]      = pathStr;
            m_pathToWd[pathStr] = wd;
        }
#else
        (void)p;
#endif
    }

    void AddWatchRecursive(const fs::path& rootPath)
    {
#if defined(HAS_INOTIFY)
        if (m_inotifyFd < 0) return;

        std::error_code ec;
        if (!fs::exists(rootPath, ec) || !fs::is_directory(rootPath, ec)) return;

        AddWatch(rootPath);
        try
        {
            for (const auto& entry:
                 fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied, ec))
            {
                if (entry.is_directory(ec))
                {
                    AddWatch(entry.path());
                }
            }
        } catch (...)
        {
        }
#else
        (void)rootPath;
#endif
    }

private:
#if defined(HAS_INOTIFY)
    void RemoveWatch(const int wd)
    {
        std::lock_guard lock(m_watchMutex);
        if (const auto it = m_wdToPath.find(wd); it != m_wdToPath.end())
        {
            m_pathToWd.erase(it->second.string());
            m_wdToPath.erase(it);
        }
    }

    fs::path GetPathForWd(const int wd)
    {
        std::lock_guard lock(m_watchMutex);
        if (const auto it = m_wdToPath.find(wd); it != m_wdToPath.end())
        {
            return it->second;
        }
        return {};
    }

    void EventLoop()
    {
        bool           hasPendingChange = false;
        auto           lastEventTime    = std::chrono::steady_clock::now();
        constexpr auto debounceDuration = std::chrono::milliseconds(300);

        pollfd pfd[2];
        pfd[0].fd     = m_inotifyFd;
        pfd[0].events = POLLIN;
        pfd[1].fd     = m_stopEventFd;
        pfd[1].events = POLLIN;

        alignas(alignof(struct inotify_event)) char buffer[4096 * 8];

        while (true)
        {
            int timeoutMs = -1;
            if (hasPendingChange)
            {
                const auto now     = std::chrono::steady_clock::now();
                if (const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEventTime); elapsed >= debounceDuration)
                {
                    hasPendingChange = false;
                    if (m_onChange) m_onChange();
                    timeoutMs = -1;
                }
                else
                {
                    timeoutMs = static_cast<int>((debounceDuration - elapsed).count());
                }
            }

            const int ret = poll(pfd, 2, timeoutMs);
            if (ret < 0)
            {
                if (errno == EINTR) continue;
                break;
            }

            if (pfd[1].revents & POLLIN)
            {
                break; // Stop event triggered
            }

            if (ret == 0 && hasPendingChange)
            {
                hasPendingChange = false;
                if (m_onChange) m_onChange();
                continue;
            }

            if (pfd[0].revents & POLLIN)
            {
                if (const ssize_t len = read(m_inotifyFd, buffer, sizeof(buffer)); len > 0)
                {
                    for (const char* ptr = buffer; ptr < buffer + len;)
                    {
                        const auto* event = reinterpret_cast<const struct inotify_event*>(ptr);

                        if (event->mask & IN_IGNORED)
                        {
                            RemoveWatch(event->wd);
                        }
                        else if (event->mask & IN_Q_OVERFLOW)
                        {
                            hasPendingChange = true;
                            lastEventTime    = std::chrono::steady_clock::now();
                        }
                        else
                        {
                            if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)))
                            {
                                if (event->len > 0)
                                {
                                    if (const fs::path parent = GetPathForWd(event->wd); !parent.empty())
                                    {
                                        AddWatchRecursive(parent / event->name);
                                    }
                                }
                            }
                            hasPendingChange = true;
                            lastEventTime    = std::chrono::steady_clock::now();
                        }

                        ptr += sizeof(struct inotify_event) + event->len;
                    }
                }
            }
        }
    }

    int                                  m_inotifyFd   = -1;
    int                                  m_stopEventFd = -1;
    std::thread                          m_watchThread;
    std::mutex                           m_watchMutex;
    std::unordered_map<int, fs::path>    m_wdToPath;
    std::unordered_map<std::string, int> m_pathToWd;
#endif

    ChangeCallback m_onChange;
    bool           m_isActive = false;
};

} // namespace pge_treemap
