#pragma once

#include <cstdint>

namespace GitHubManifestClient {

    // Ensures manifest files for the given AppID are fetched from the configured GitHub repository,
    // extracted into <Steam>/depotcache/, decompressed to plain protobuf, and recorded in manifestcache.lua.
    // Returns true if manifests were successfully obtained and written, or false otherwise.
    bool EnsureManifestsForApp(uint32_t appId, bool forceRefresh = false);

} // namespace GitHubManifestClient
