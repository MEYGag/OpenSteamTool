#include "ManifestDecompressor.h"
#include "dllmain.h"
#include "Utils/Logging/Log.h"

#include <zlib.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <vector>

namespace ManifestDecompressor {

    // Stream decompress raw Deflate payload starting at the given byte offset
    static bool TryInflateAt(const uint8_t* raw, size_t rawLen, size_t offset, std::vector<uint8_t>& result) {
        result.clear();
        if (!raw || offset >= rawLen) return false;

        z_stream strm{};
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = static_cast<uInt>(rawLen - offset);
        strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw + offset));

        // windowBits = -15 specifies raw Deflate (no zlib/gzip headers)
        if (inflateInit2(&strm, -15) != Z_OK) {
            return false;
        }

        const size_t kChunkSize = 65536;
        const size_t kMaxBytes = 512L * 1024 * 1024;
        uint8_t buf[kChunkSize];

        int ret = Z_OK;
        while (strm.avail_in > 0 || ret == Z_OK) {
            strm.avail_out = static_cast<uInt>(kChunkSize);
            strm.next_out = reinterpret_cast<Bytef*>(buf);

            ret = inflate(&strm, Z_SYNC_FLUSH);
            size_t bytesRead = kChunkSize - strm.avail_out;
            if (bytesRead > 0) {
                if (result.size() + bytesRead > kMaxBytes) {
                    inflateEnd(&strm);
                    result.clear();
                    return false;
                }
                result.insert(result.end(), buf, buf + bytesRead);
            }

            if (ret == Z_STREAM_END || ret == Z_BUF_ERROR) {
                break;
            }
            if (ret != Z_OK) {
                break;
            }
        }

        inflateEnd(&strm);
        return !result.empty();
    }

    bool SanitizeManifestFile(const std::filesystem::path& filePath) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(filePath, ec)) {
            return false;
        }

        const auto fileSize = std::filesystem::file_size(filePath, ec);
        if (ec || fileSize < 16 || fileSize > 512 * 1024 * 1024) {
            return false;
        }

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        std::vector<uint8_t> raw(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(raw.data()), fileSize);
        file.close();

        // Check for Steam encrypted container magic (0x71F617B0 / 0x71F617B1)
        if (raw.size() >= 4) {
            uint32_t magic = raw[0] | (raw[1] << 8) | (raw[2] << 16) | (raw[3] << 24);
            if (magic == 0x71F617B0 || magic == 0x71F617B1) {
                return true;
            }
        }

        // Common offsets: 10 (Pro wrapper prefix), 2 (stripped zlib header), 0 (raw deflate)
        const size_t offsets[] = { 10, 2, 0 };

        for (size_t offset : offsets) {
            std::vector<uint8_t> inflated;
            if (TryInflateAt(raw.data(), raw.size(), offset, inflated) &&
                inflated.size() > raw.size() && inflated.size() >= 64) {
                std::filesystem::path tempPath = filePath;
                tempPath += ".tmp";

                std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
                if (outFile.is_open()) {
                    outFile.write(reinterpret_cast<const char*>(inflated.data()), inflated.size());
                    outFile.close();

                    std::filesystem::rename(tempPath, filePath, ec);
                    if (!ec) {
                        LOG_MANIFEST_INFO("Sanitized manifest {}: decompressed {} bytes -> {} bytes (offset {})",
                            filePath.filename().string(), raw.size(), inflated.size(), offset);
                        return true;
                    } else {
                        LOG_MANIFEST_WARN("Failed to replace manifest file {}: ec={}",
                            filePath.filename().string(), ec.value());
                    }
                }
            }
        }

        return false;
    }

    void SanitizeDepotManifest(uint32_t depotId, uint64_t manifestGid) {
        if (SteamInstallPath[0] == '\0' || !manifestGid) return;

        const std::filesystem::path manifestPath =
            std::filesystem::path(SteamInstallPath) / "depotcache" / std::format("{}_{}.manifest", depotId, manifestGid);

        SanitizeManifestFile(manifestPath);
    }

    void SanitizeDepotCacheDirectory(const std::filesystem::path& depotCacheDir) {
        std::error_code ec;
        if (!std::filesystem::exists(depotCacheDir, ec) || !std::filesystem::is_directory(depotCacheDir, ec)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(depotCacheDir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".manifest") {
                SanitizeManifestFile(entry.path());
            }
        }
    }

    void ScanAndSanitizeDepotCache() {
        if (SteamInstallPath[0] == '\0') return;
        const std::filesystem::path depotCacheDir = std::filesystem::path(SteamInstallPath) / "depotcache";
        SanitizeDepotCacheDirectory(depotCacheDir);
    }

} // namespace ManifestDecompressor
