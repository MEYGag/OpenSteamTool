#include "GitHubManifestClient.h"
#include "dllmain.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Config/ManifestCacheManager.h"
#include "Utils/Logging/Log.h"
#include "Utils/Manifest/ManifestDecompressor.h"
#include "Utils/Manifest/ZipExtractor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GitHubManifestClient {

    static std::mutex g_clientMutex;
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_lastFetchTime;
    constexpr auto kFetchCooldown = std::chrono::seconds(30);

    static std::string BuildDownloadUrl(const Config::GitHubRepoInfo& repoInfo, uint32_t appId, const std::string& mirror) {
        std::string directUrl = std::format("https://codeload.github.com/{}/{}/zip/refs/heads/{}",
                                            repoInfo.owner, repoInfo.repo, appId);

        if (mirror.empty()) {
            return directUrl;
        }

        std::string m = mirror;
        if (m.find("{url}") != std::string::npos) {
            size_t pos = m.find("{url}");
            return m.replace(pos, 5, directUrl);
        }

        if (!m.ends_with('/')) {
            m += '/';
        }
        return m + directUrl;
    }

    bool EnsureManifestsForApp(uint32_t appId, bool forceRefresh) {
        if (!appId) return false;

        if (!Config::IsGitHubManifestRepo()) {
            return false;
        }

        if (!forceRefresh && ManifestCacheManager::HasAppManifestsCached(appId)) {
            LOG_MANIFEST_TRACE("GitHubManifestClient: AppID {} manifests already cached and present in depotcache", appId);
            return true;
        }

        {
            std::lock_guard lock(g_clientMutex);
            auto it = g_lastFetchTime.find(appId);
            if (it != g_lastFetchTime.end()) {
                auto now = std::chrono::steady_clock::now();
                if (now - it->second < kFetchCooldown) {
                    LOG_MANIFEST_TRACE("GitHubManifestClient: skipping duplicate fetch for AppID {} (within cooldown)", appId);
                    return true;
                }
            }
        }

        auto repoInfo = Config::GetGitHubManifestRepoInfo();
        if (!repoInfo.isGitHub) {
            return false;
        }

        const auto timeouts = Config::GetManifestTimeouts();
        const std::string mirror = Config::GetManifestMirror();
        const std::string directUrl = std::format("https://codeload.github.com/{}/{}/zip/refs/heads/{}",
                                                  repoInfo.owner, repoInfo.repo, appId);

        std::vector<std::string> urlsToTry;
        if (!mirror.empty()) {
            urlsToTry.push_back(BuildDownloadUrl(repoInfo, appId, mirror));
        }
        urlsToTry.push_back(directUrl);

        OSTPlatform::Http::Result result;
        for (const auto& url : urlsToTry) {
            LOG_MANIFEST_INFO("GitHubManifestClient: requesting manifests for AppID {} from {}", appId, url);
            result = OSTPlatform::Http::Execute(
                L"GET",
                url.c_str(),
                nullptr,
                0,
                nullptr,
                timeouts.resolve,
                timeouts.connect,
                timeouts.send,
                timeouts.recv
            );

            if (result.ok && result.status == 200) {
                break;
            }

            if (result.ok && result.status == 404) {
                LOG_MANIFEST_INFO("GitHubManifestClient: branch for AppID {} not found in repo {}/{} (HTTP 404)",
                                  appId, repoInfo.owner, repoInfo.repo);
                return false;
            }

            LOG_MANIFEST_WARN("GitHubManifestClient: request failed for {} (status={}, ok={})",
                              url, result.status, result.ok);
        }

        if (!result.ok || result.status != 200 || result.body.empty()) {
            LOG_MANIFEST_WARN("GitHubManifestClient: failed to download zip archive for AppID {}", appId);
            return false;
        }

        auto extractedFiles = ZipExtractor::ExtractFiles(
            reinterpret_cast<const uint8_t*>(result.body.data()),
            result.body.size(),
            ".manifest"
        );

        if (extractedFiles.empty()) {
            LOG_MANIFEST_WARN("GitHubManifestClient: archive for AppID {} contained no .manifest files", appId);
            return false;
        }

        std::filesystem::path depotcacheDir = std::filesystem::path(SteamInstallPath) / "depotcache";
        std::error_code ec;
        std::filesystem::create_directories(depotcacheDir, ec);

        std::map<uint32_t, uint64_t> depotGids;
        std::regex manifestNameRegex(R"(^(\d+)_(\d+)\.manifest$)", std::regex::icase);

        for (const auto& file : extractedFiles) {
            std::filesystem::path targetPath = depotcacheDir / file.filename;
            std::filesystem::path tempPath = depotcacheDir / (file.filename + ".tmp");

            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                LOG_MANIFEST_WARN("GitHubManifestClient: failed to write {}", tempPath.string());
                continue;
            }

            out.write(reinterpret_cast<const char*>(file.data.data()), static_cast<std::streamsize>(file.data.size()));
            out.close();

            // Decompress/sanitize in-place if compressed on the temp file
            ManifestDecompressor::SanitizeManifestFile(tempPath);

            // Atomically replace targetPath with the sanitized temp file
            std::filesystem::rename(tempPath, targetPath, ec);
            if (ec) {
                std::filesystem::copy_file(tempPath, targetPath, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tempPath, ec);
            }

            std::smatch match;
            if (std::regex_search(file.filename, match, manifestNameRegex)) {
                uint32_t depotId = std::stoul(match[1].str());
                uint64_t gid = std::stoull(match[2].str());
                depotGids[depotId] = gid;
            }
        }

        if (!depotGids.empty()) {
            {
                std::lock_guard lock(g_clientMutex);
                g_lastFetchTime[appId] = std::chrono::steady_clock::now();
            }

            ManifestCacheManager::SetAppDepotManifests(appId, depotGids);

            // Re-parse manifestcache.lua into LuaConfig to update in-memory overrides
            const std::string cacheFilePath = ManifestCacheManager::GetCacheFilePathString();
            if (!cacheFilePath.empty()) {
                LuaConfig::ParseFile(cacheFilePath);
            }

            LOG_MANIFEST_INFO("GitHubManifestClient: successfully updated {} manifests for AppID {}",
                              depotGids.size(), appId);
            return true;
        }

        return false;
    }

} // namespace GitHubManifestClient
