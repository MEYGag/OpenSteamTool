#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ManifestCacheManager {

    // Initializes the manager and loads existing manifestcache.lua
    void Init(const std::string& steamInstallPath);

    // Updates cached depot manifest IDs for a given AppID and rewrites manifestcache.lua
    void SetAppDepotManifests(uint32_t appId, const std::map<uint32_t, uint64_t>& depotGids);

    // Checks all tracked AppIDs against steamapps/appmanifest_{AppId}.acf and removes uninstalled games
    void PruneUninstalledApps();

    // Checks whether the specified AppID has an appmanifest_{AppId}.acf in any Steam library folder
    bool IsAppInstalled(uint32_t appId);

    // Checks whether the specified AppID has all its depot manifest files present in depotcache/
    bool HasAppManifestsCached(uint32_t appId);

    // Finds the parent AppID for a given DepotID if known in manifest cache
    uint32_t FindAppIdForDepot(uint32_t depotId);

    // Gets or sets the timestamp of the last periodic manifest update check
    uint64_t GetLastCheckTime();
    void SetLastCheckTime(uint64_t timestamp);

    // Returns the full path to config/lua/manifestcache.lua
    std::string GetCacheFilePathString();

} // namespace ManifestCacheManager
