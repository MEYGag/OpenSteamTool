#include "ManifestCacheManager.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <vector>

namespace ManifestCacheManager {

    static std::string g_steamInstallPath;
    static std::mutex g_mutex;
    // Map of AppID -> Map of (depotId -> manifestGid)
    static std::map<uint32_t, std::map<uint32_t, uint64_t>> g_appDepots;
    static uint64_t g_lastCheckTime = 0;

    static std::filesystem::path GetCacheFilePath() {
        if (g_steamInstallPath.empty()) return {};
        return std::filesystem::path(g_steamInstallPath) / "config" / "lua" / "manifestcache.lua";
    }

    std::string GetCacheFilePathString() {
        std::lock_guard lock(g_mutex);
        return GetCacheFilePath().string();
    }

    static std::vector<std::filesystem::path> GetLibrarySteamAppsDirsLocked() {
        std::vector<std::filesystem::path> dirs;
        if (g_steamInstallPath.empty()) return dirs;

        std::filesystem::path defaultSteamApps = std::filesystem::path(g_steamInstallPath) / "steamapps";
        dirs.push_back(defaultSteamApps);

        std::filesystem::path vdfPath = defaultSteamApps / "libraryfolders.vdf";
        std::error_code ec;
        if (std::filesystem::exists(vdfPath, ec)) {
            std::ifstream file(vdfPath);
            std::string line;
            std::regex pathRegex(R"(\"path\"\s+\"([^\"]+)\")", std::regex::icase);
            while (std::getline(file, line)) {
                std::smatch match;
                if (std::regex_search(line, match, pathRegex)) {
                    std::string libPathStr = match[1].str();
                    std::string normalized;
                    for (size_t i = 0; i < libPathStr.size(); ++i) {
                        if (libPathStr[i] == '\\' && i + 1 < libPathStr.size() && libPathStr[i + 1] == '\\') {
                            normalized += '\\';
                            ++i;
                        } else {
                            normalized += libPathStr[i];
                        }
                    }
                    std::filesystem::path libSteamApps = std::filesystem::path(normalized) / "steamapps";
                    if (std::filesystem::exists(libSteamApps, ec)) {
                        if (std::find(dirs.begin(), dirs.end(), libSteamApps) == dirs.end()) {
                            dirs.push_back(libSteamApps);
                        }
                    }
                }
            }
        }
        return dirs;
    }

    static void LoadCacheFileLocked() {
        const auto cachePath = GetCacheFilePath();
        if (cachePath.empty() || !std::filesystem::exists(cachePath)) {
            g_lastCheckTime = 0;
            return;
        }

        std::ifstream file(cachePath);
        if (!file.is_open()) return;

        g_appDepots.clear();
        g_lastCheckTime = 0;
        uint32_t currentAppId = 0;
        std::string line;

        // Regex for "-- [LastCheckTime: 123456789]"
        std::regex lastCheckRegex(R"(^--\s*\[?LastCheckTime:\s*(\d+)\]?)", std::regex::icase);
        // Regex for "-- [AppID: 12345]" or "-- AppID: 12345"
        std::regex appHeaderRegex(R"(^--\s*\[?AppID:\s*(\d+)\]?)", std::regex::icase);
        // Regex for setManifestid(depotId, "gid" [, size])
        std::regex setManifestRegex(R"(setManifestid\s*\(\s*(\d+)\s*,\s*\"?(\d+)\"?\s*(?:,\s*(\d+))?\s*\))", std::regex::icase);

        try {
            while (std::getline(file, line)) {
                std::smatch match;
                if (std::regex_search(line, match, lastCheckRegex)) {
                    try {
                        g_lastCheckTime = std::stoull(match[1].str());
                    } catch (...) {}
                    continue;
                }
                if (std::regex_search(line, match, appHeaderRegex)) {
                    try {
                        currentAppId = std::stoul(match[1].str());
                    } catch (...) {}
                    continue;
                }
                if (std::regex_search(line, match, setManifestRegex)) {
                    try {
                        uint32_t depotId = std::stoul(match[1].str());
                        uint64_t gid = std::stoull(match[2].str());
                        if (currentAppId == 0) {
                            currentAppId = depotId; // Fallback if no header
                        }
                        g_appDepots[currentAppId][depotId] = gid;
                    } catch (...) {}
                }
            }
        } catch (...) {
            LOG_MANIFEST_WARN("ManifestCacheManager: exception while parsing cache file");
        }
    }

    static void SaveCacheFileLocked() {
        const auto cachePath = GetCacheFilePath();
        if (cachePath.empty()) return;

        std::error_code ec;
        std::filesystem::create_directories(cachePath.parent_path(), ec);

        std::filesystem::path tempPath = cachePath;
        tempPath += ".tmp";

        std::ofstream file(tempPath, std::ios::trunc);
        if (!file.is_open()) {
            LOG_MANIFEST_WARN("ManifestCacheManager: failed to open temp file for writing: {}", tempPath.string());
            return;
        }

        file << "-- Auto-generated by OpenSteamTool manifest cache\n";
        file << "-- Do not edit manually; synchronized with installed games\n";
        if (g_lastCheckTime > 0) {
            file << "-- [LastCheckTime: " << g_lastCheckTime << "]\n";
        }
        file << "\n";

        for (const auto& [appId, depots] : g_appDepots) {
            if (depots.empty()) continue;
            file << "-- [AppID: " << appId << "]\n";
            for (const auto& [depotId, gid] : depots) {
                file << "setManifestid(" << depotId << ", \"" << gid << "\")\n";
            }
            file << "\n";
        }

        file.close();
        std::filesystem::rename(tempPath, cachePath, ec);
        if (ec) {
            LOG_MANIFEST_WARN("ManifestCacheManager: failed to replace {}: ec={}", cachePath.string(), ec.value());
        }
    }

    void Init(const std::string& steamInstallPath) {
        std::lock_guard lock(g_mutex);
        g_steamInstallPath = steamInstallPath;
        LoadCacheFileLocked();
        LOG_MANIFEST_INFO("ManifestCacheManager: initialized with {} cached apps (lastCheckTime={})",
                          g_appDepots.size(), g_lastCheckTime);
    }

    void SetAppDepotManifests(uint32_t appId, const std::map<uint32_t, uint64_t>& depotGids) {
        if (!appId || depotGids.empty()) return;
        std::lock_guard lock(g_mutex);

        auto& targetMap = g_appDepots[appId];
        for (const auto& [depotId, gid] : depotGids) {
            targetMap[depotId] = gid;
        }

        SaveCacheFileLocked();
        LOG_MANIFEST_INFO("ManifestCacheManager: updated {} depots for AppID {}", depotGids.size(), appId);
    }

    bool IsAppInstalled(uint32_t appId) {
        if (!appId) return false;
        std::lock_guard lock(g_mutex);
        const auto libDirs = GetLibrarySteamAppsDirsLocked();
        for (const auto& dir : libDirs) {
            std::filesystem::path acfPath = dir / std::format("appmanifest_{}.acf", appId);
            std::error_code ec;
            if (std::filesystem::exists(acfPath, ec)) {
                return true;
            }
        }
        return false;
    }

    void PruneUninstalledApps() {
        std::lock_guard lock(g_mutex);
        if (g_steamInstallPath.empty() || g_appDepots.empty()) return;

        const auto libDirs = GetLibrarySteamAppsDirsLocked();
        bool changed = false;

        auto it = g_appDepots.begin();
        while (it != g_appDepots.end()) {
            uint32_t appId = it->first;
            bool installed = false;
            for (const auto& dir : libDirs) {
                std::filesystem::path acfPath = dir / std::format("appmanifest_{}.acf", appId);
                std::error_code ec;
                if (std::filesystem::exists(acfPath, ec)) {
                    installed = true;
                    break;
                }
            }

            if (!installed) {
                LOG_MANIFEST_INFO("ManifestCacheManager: pruning uninstalled AppID {} from manifest cache", appId);
                it = g_appDepots.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }

        if (changed) {
            SaveCacheFileLocked();
        }
    }

    bool HasAppManifestsCached(uint32_t appId) {
        if (!appId) return false;
        std::lock_guard lock(g_mutex);
        auto it = g_appDepots.find(appId);
        if (it == g_appDepots.end() || it->second.empty()) return false;

        if (g_steamInstallPath.empty()) return false;
        std::filesystem::path depotcacheDir = std::filesystem::path(g_steamInstallPath) / "depotcache";

        for (const auto& [depotId, gid] : it->second) {
            std::filesystem::path manifestPath = depotcacheDir / std::format("{}_{}.manifest", depotId, gid);
            std::error_code ec;
            if (!std::filesystem::exists(manifestPath, ec)) {
                return false;
            }
        }
        return true;
    }

    uint32_t FindAppIdForDepot(uint32_t depotId) {
        if (!depotId) return 0;
        std::lock_guard lock(g_mutex);
        for (const auto& [appId, depots] : g_appDepots) {
            if (depots.count(depotId)) return appId;
        }
        return 0;
    }

    uint64_t GetLastCheckTime() {
        std::lock_guard lock(g_mutex);
        return g_lastCheckTime;
    }

    void SetLastCheckTime(uint64_t timestamp) {
        std::lock_guard lock(g_mutex);
        g_lastCheckTime = timestamp;
        SaveCacheFileLocked();
    }

} // namespace ManifestCacheManager
