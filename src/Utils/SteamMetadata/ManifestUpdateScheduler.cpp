#include "ManifestUpdateScheduler.h"
#include "GitHubManifestClient.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Config/ManifestCacheManager.h"
#include "Utils/Logging/Log.h"
#include "OSTPlatform/include/Thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_set>

namespace ManifestUpdateScheduler {

    static std::atomic<bool> g_running{false};
    static std::mutex g_mutex;
    static std::condition_variable g_cv;

    static uint32_t WorkerLoop() {
        LOG_MANIFEST_INFO("ManifestUpdateScheduler: worker thread started");

        bool isFirstIteration = true;

        while (g_running) {
            uint32_t intervalHours = Config::GetManifestCheckIntervalHours();
            if (intervalHours == 0 || !Config::IsGitHubManifestRepo()) {
                // Wait until awakened on shutdown or config change
                std::unique_lock lock(g_mutex);
                g_cv.wait(lock, [&] { return !g_running; });
                break;
            }

            std::chrono::seconds waitDuration = std::chrono::hours(intervalHours);

            if (isFirstIteration) {
                isFirstIteration = false;

                uint64_t now = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );
                uint64_t lastCheck = ManifestCacheManager::GetLastCheckTime();
                uint64_t intervalSeconds = static_cast<uint64_t>(intervalHours) * 3600ULL;

                constexpr uint64_t kStartupDelaySeconds = 30;

                if (lastCheck > 0 && lastCheck <= now) {
                    uint64_t elapsed = now - lastCheck;
                    if (elapsed < intervalSeconds) {
                        uint64_t remainSeconds = intervalSeconds - elapsed;
                        waitDuration = std::chrono::seconds(remainSeconds);
                        LOG_MANIFEST_INFO("ManifestUpdateScheduler: next check scheduled in {}s (elapsed {}s / {}s)",
                                          remainSeconds, elapsed, intervalSeconds);
                    } else {
                        waitDuration = std::chrono::seconds(kStartupDelaySeconds);
                        LOG_MANIFEST_INFO("ManifestUpdateScheduler: last check was {}s ago (>= {}s), check in {}s after startup delay",
                                          elapsed, intervalSeconds, kStartupDelaySeconds);
                    }
                } else {
                    if (lastCheck > now) {
                        LOG_MANIFEST_WARN("ManifestUpdateScheduler: detected future lastCheckTime ({}), resetting to startup delay {}s",
                                          lastCheck, kStartupDelaySeconds);
                    } else {
                        LOG_MANIFEST_INFO("ManifestUpdateScheduler: no valid last check time found, initial check in {}s",
                                          kStartupDelaySeconds);
                    }
                    waitDuration = std::chrono::seconds(kStartupDelaySeconds);
                }
            }

            {
                std::unique_lock lock(g_mutex);
                if (g_cv.wait_for(lock, waitDuration, [&] { return !g_running; })) {
                    break;
                }
            }

            if (!g_running) break;

            if (!Config::IsGitHubManifestRepo()) continue;

            LOG_MANIFEST_INFO("ManifestUpdateScheduler: starting periodic manifest update check");

            // 1. Prune uninstalled apps first
            ManifestCacheManager::PruneUninstalledApps();

            // 2. Query all known AppIDs and check for updates if installed
            auto allDepotIds = LuaConfig::GetAllDepotIds();
            std::unordered_set<uint32_t> checkedApps;

            for (uint32_t appId : allDepotIds) {
                if (!g_running) break;
                if (!checkedApps.insert(appId).second) continue;

                if (ManifestCacheManager::IsAppInstalled(appId)) {
                    LOG_MANIFEST_INFO("ManifestUpdateScheduler: checking updates for installed AppID {}", appId);
                    GitHubManifestClient::EnsureManifestsForApp(appId, true);

                    // Rate limit polite delay between requests
                    std::unique_lock lock(g_mutex);
                    if (g_cv.wait_for(lock, std::chrono::milliseconds(1000), [&] { return !g_running; })) {
                        break;
                    }
                }
            }

            if (g_running) {
                uint64_t finishedTime = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );
                ManifestCacheManager::SetLastCheckTime(finishedTime);
                LOG_MANIFEST_INFO("ManifestUpdateScheduler: periodic manifest update check finished, updated lastCheckTime={}", finishedTime);
            }
        }

        LOG_MANIFEST_INFO("ManifestUpdateScheduler: worker thread exited");
        return 0;
    }

    void Start() {
        std::lock_guard lock(g_mutex);
        if (g_running) return;

        if (!Config::IsGitHubManifestRepo() || Config::GetManifestCheckIntervalHours() == 0) {
            LOG_MANIFEST_INFO("ManifestUpdateScheduler: not starting (repo not configured or interval is 0)");
            return;
        }

        g_running = true;
        OSTPlatform::Thread::StartDetached(WorkerLoop);
    }

    void Stop() {
        std::lock_guard lock(g_mutex);
        if (!g_running) return;
        g_running = false;
        g_cv.notify_all();
    }

} // namespace ManifestUpdateScheduler
