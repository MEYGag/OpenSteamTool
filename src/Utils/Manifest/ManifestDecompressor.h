#pragma once

#include <cstdint>
#include <filesystem>

namespace ManifestDecompressor {

    // Attempts to decompress a local manifest file if compressed (zlib/deflate/pro wrapper).
    // Replaces the file in-place if successfully decompressed.
    // Returns true if the file was decompressed or is already valid uncompressed/encrypted.
    bool SanitizeManifestFile(const std::filesystem::path& filePath);

    // Sanitizes a specific depot manifest in <Steam>/depotcache/<depotId>_<manifestGid>.manifest.
    void SanitizeDepotManifest(uint32_t depotId, uint64_t manifestGid);

    // Scans all .manifest files in <Steam>/depotcache/ and sanitizes any compressed ones.
    void ScanAndSanitizeDepotCache();

} // namespace ManifestDecompressor
