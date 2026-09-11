#include "Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/ManifestClient.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <mutex>

namespace Config {
namespace {

    struct Snapshot {
        std::string manifestProvider = "opensteamtool";
        std::string manifestMirror;
        uint32_t manifestCheckIntervalHours = 24;
        ManifestTimeouts manifestTimeouts;
        LogLevel logLevel = LogLevel::Debug;
        std::string logDir;
        std::vector<std::string> luaPaths;
        std::string remoteUrlTemplate;
        bool statsEnableApi = true;
        InjectionSettings injection;
        CloudSettings cloud;
    };

    std::mutex g_mutex;
    bool g_loadedOnce = false;

    GitHubRepoInfo ParseGitHubRepo(std::string_view url) {
        GitHubRepoInfo info;
        if (url.empty()) return info;

        std::string s(url);
        while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
            s.pop_back();
        }

        constexpr std::string_view prefix1 = "https://github.com/";
        constexpr std::string_view prefix2 = "http://github.com/";
        constexpr std::string_view prefix3 = "github.com/";

        size_t startPos = 0;
        if (s.starts_with(prefix1)) {
            startPos = prefix1.size();
        } else if (s.starts_with(prefix2)) {
            startPos = prefix2.size();
        } else if (s.starts_with(prefix3)) {
            startPos = prefix3.size();
        } else {
            size_t slash = s.find('/');
            if (slash != std::string::npos && slash > 0 && slash < s.size() - 1 &&
                s.find('/', slash + 1) == std::string::npos &&
                s != "opensteamtool" && s != "wudrm" && s != "steamrun") {
                startPos = 0;
            } else {
                return info;
            }
        }

        std::string rest = s.substr(startPos);
        size_t slashPos = rest.find('/');
        if (slashPos == std::string::npos || slashPos == 0 || slashPos >= rest.size() - 1) {
            return info;
        }

        info.owner = rest.substr(0, slashPos);
        info.repo = rest.substr(slashPos + 1);

        if (info.repo.ends_with(".git")) {
            info.repo.resize(info.repo.size() - 4);
        }

        info.isGitHub = !info.owner.empty() && !info.repo.empty();
        return info;
    }

    const char* ToString(LogLevel level) {
        switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        }
        return "???";
    }

    Snapshot MakeDefaultSnapshot(const std::string& configPath) {
        Snapshot snapshot;
        std::filesystem::path p(configPath);
        snapshot.logDir = (p.parent_path() / "opensteamtool").string();
        return snapshot;
    }

    void ApplySnapshot(const Snapshot& snapshot) {
        manifestTimeoutResolve     = snapshot.manifestTimeouts.resolve;
        manifestTimeoutConnect     = snapshot.manifestTimeouts.connect;
        manifestTimeoutSend        = snapshot.manifestTimeouts.send;
        manifestTimeoutRecv        = snapshot.manifestTimeouts.recv;
        manifestUrl                = snapshot.manifestProvider;
        manifestMirror             = snapshot.manifestMirror;
        manifestCheckIntervalHours = snapshot.manifestCheckIntervalHours;
        gitHubRepoInfo             = ParseGitHubRepo(snapshot.manifestProvider);
        logLevel                   = snapshot.logLevel;
        logDir                     = snapshot.logDir;
        luaPaths                   = snapshot.luaPaths;
        remoteUrlTemplate          = snapshot.remoteUrlTemplate;
        statsEnableApi             = snapshot.statsEnableApi;
        injectEnabled              = snapshot.injection.enabled;
        injectLibraryX86           = snapshot.injection.libraryX86;
        injectLibraryX64           = snapshot.injection.libraryX64;
        cloudEnabled               = snapshot.cloud.enabled;
        cloudLibrary               = snapshot.cloud.library;
    }

    void ApplyManifestProvider(const std::string& provider) {
        auto ghInfo = ParseGitHubRepo(provider);
        if (ghInfo.isGitHub) {
            LOG_INFO("Config: manifest URL configured as GitHub repo: {}/{}", ghInfo.owner, ghInfo.repo);
            return;
        }
        if (!ManifestClient::SetProvider(provider)) {
            LOG_WARN("Unknown manifest.url \"{}\", keeping default", provider);
            ManifestClient::SetProvider("opensteamtool");
        }
    }

    LoadResult ApplySnapshotLocked(const Snapshot& snapshot) {
        std::lock_guard lock(g_mutex);
        LoadResult result;
        result.luaPathsChanged = luaPaths != snapshot.luaPaths;
        ApplySnapshot(snapshot);
        g_loadedOnce = true;
        result.applied = true;
        return result;
    }

} // namespace

    LoadResult Load(const std::string& configPath) {
        Snapshot snapshot = MakeDefaultSnapshot(configPath);
        if (!std::filesystem::exists(configPath)) {
            LOG_INFO("Config file not found, using defaults");
            ApplyManifestProvider(snapshot.manifestProvider);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ToString(GetLogLevel()),
                     (uint32_t)GetLuaPaths().size(),
                     GetStatsEnableApi(),
                     GetRemoteUrlTemplate().empty() ? "<default>" : GetRemoteUrlTemplate());
            return result;
        }

        try {
            auto tbl = toml::parse_file(configPath);

            // [manifest]
            if (auto manifest = tbl["manifest"].as_table()) {
                if (auto val = (*manifest)["url"].value<std::string>()) {
                    snapshot.manifestProvider = *val;
                }
                if (auto val = (*manifest)["mirror"].value<std::string>()) {
                    snapshot.manifestMirror = *val;
                }
                if (auto val = (*manifest)["check_interval"].value<int64_t>()) {
                    snapshot.manifestCheckIntervalHours = static_cast<uint32_t>(std::max<int64_t>(0, *val));
                }
                if (auto val = (*manifest)["timeout_resolve_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.resolve = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_connect_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.connect = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_send_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.send = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_recv_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.recv = static_cast<uint32_t>(*val);
            }

            // [log]
            if (auto log = tbl["log"].as_table()) {
                if (auto val = (*log)["level"].value<std::string>()) {
                    if (*val == "trace")           snapshot.logLevel = LogLevel::Trace;
                    else if (*val == "debug")       snapshot.logLevel = LogLevel::Debug;
                    else if (*val == "info")        snapshot.logLevel = LogLevel::Info;
                    else if (*val == "warn")        snapshot.logLevel = LogLevel::Warn;
                    else if (*val == "error")       snapshot.logLevel = LogLevel::Error;
                }
            }

            // [lua]
            if (auto lua = tbl["lua"].as_table()) {
                if (auto arr = (*lua)["paths"].as_array()) {
                    for (auto& elem : *arr) {
                        if (auto str = elem.value<std::string>()) {
                            snapshot.luaPaths.push_back(*str);
                        }
                    }
                }
            }

            // [remote]
            if (auto remote = tbl["remote"].as_table()) {
                if (auto val = (*remote)["url_template"].value<std::string>()) {
                    snapshot.remoteUrlTemplate = *val;
                }
            }

            // [stats]
            if (auto stats = tbl["stats"].as_table()) {
                if (auto val = (*stats)["enable_api"].value<bool>()) {
                    snapshot.statsEnableApi = *val;
                }
            }

            // [inject]
            if (auto inject = tbl["inject"].as_table()) {
                if (auto val = (*inject)["enabled"].value<bool>())
                    snapshot.injection.enabled = *val;
                if (auto val = (*inject)["library_x86"].value<std::string>())
                    snapshot.injection.libraryX86 = *val;
                if (auto val = (*inject)["library_x64"].value<std::string>())
                    snapshot.injection.libraryX64 = *val;
            }

            // [cloud]
            if (auto cloud = tbl["cloud"].as_table()) {
                if (auto val = (*cloud)["enabled"].value<bool>())
                    snapshot.cloud.enabled = *val;
                if (auto val = (*cloud)["library"].value<std::string>())
                    snapshot.cloud.library = *val;
            }

            ApplyManifestProvider(snapshot.manifestProvider);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ToString(snapshot.logLevel),
                     (uint32_t)snapshot.luaPaths.size(),
                     snapshot.statsEnableApi,
                     snapshot.remoteUrlTemplate.empty() ? "<default>" : snapshot.remoteUrlTemplate);
            return result;

        } catch (const toml::parse_error& e) {
            LOG_WARN("Config parse error: {}", e.what());
        } catch (...) {
            LOG_WARN("Config load failed");
        }
        bool shouldApplyDefault = false;
        {
            std::lock_guard lock(g_mutex);
            shouldApplyDefault = !g_loadedOnce;
        }
        if (shouldApplyDefault) {
            ApplyManifestProvider(snapshot.manifestProvider);
            std::lock_guard lock(g_mutex);
            const bool luaChanged = luaPaths != snapshot.luaPaths;
            ApplySnapshot(snapshot);
            g_loadedOnce = true;
            return {true, luaChanged};
        }
        return {};
    }

    ManifestTimeouts GetManifestTimeouts() {
        std::lock_guard lock(g_mutex);
        return {
            manifestTimeoutResolve,
            manifestTimeoutConnect,
            manifestTimeoutSend,
            manifestTimeoutRecv,
        };
    }

    LogLevel GetLogLevel() {
        std::lock_guard lock(g_mutex);
        return logLevel;
    }

    std::string GetLogDir() {
        std::lock_guard lock(g_mutex);
        return logDir;
    }

    std::vector<std::string> GetLuaPaths() {
        std::lock_guard lock(g_mutex);
        return luaPaths;
    }

    std::string GetRemoteUrlTemplate() {
        std::lock_guard lock(g_mutex);
        return remoteUrlTemplate;
    }

    InjectionSettings GetInjectionSettings() {
        std::lock_guard lock(g_mutex);
        return {
            injectEnabled,
            injectLibraryX86,
            injectLibraryX64,
        };
    }

    bool GetStatsEnableApi() {
        std::lock_guard lock(g_mutex);
        return statsEnableApi;
    }

    CloudSettings GetCloudSettings() {
        std::lock_guard lock(g_mutex);
        return {
            cloudEnabled,
            cloudLibrary,
        };
    }

    bool IsGitHubManifestRepo() {
        std::lock_guard lock(g_mutex);
        return gitHubRepoInfo.isGitHub;
    }

    GitHubRepoInfo GetGitHubManifestRepoInfo() {
        std::lock_guard lock(g_mutex);
        return gitHubRepoInfo;
    }

    std::string GetManifestMirror() {
        std::lock_guard lock(g_mutex);
        return manifestMirror;
    }

    uint32_t GetManifestCheckIntervalHours() {
        std::lock_guard lock(g_mutex);
        return manifestCheckIntervalHours;
    }

}
