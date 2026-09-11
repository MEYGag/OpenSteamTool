#include "ZipExtractor.h"
#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace ZipExtractor {

    static uint16_t ReadLE16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    static uint32_t ReadLE32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    }

    static bool InflateRaw(const uint8_t* src, size_t srcLen, size_t expectedSize, std::vector<uint8_t>& out) {
        out.clear();
        if (srcLen == 0) return true;

        z_stream strm{};
        strm.avail_in = static_cast<uInt>(srcLen);
        strm.next_in = const_cast<Bytef*>(src);

        if (inflateInit2(&strm, -15) != Z_OK) {
            return false;
        }

        const size_t initialCap = expectedSize > 0 ? expectedSize : (srcLen * 4);
        out.resize(initialCap > 0 ? initialCap : 65536);

        strm.avail_out = static_cast<uInt>(out.size());
        strm.next_out = out.data();

        int ret = Z_OK;
        while (ret == Z_OK) {
            ret = inflate(&strm, Z_SYNC_FLUSH);
            if (ret == Z_STREAM_END) {
                break;
            }
            if (ret == Z_OK || (ret == Z_BUF_ERROR && strm.avail_out == 0)) {
                if (out.size() >= 512 * 1024 * 1024) {
                    inflateEnd(&strm);
                    out.clear();
                    return false;
                }
                size_t oldSize = out.size();
                size_t newSize = oldSize * 2;
                out.resize(newSize);
                strm.next_out = out.data() + strm.total_out;
                strm.avail_out = static_cast<uInt>(newSize - strm.total_out);
                ret = Z_OK;
            } else {
                break;
            }
        }

        inflateEnd(&strm);
        if (strm.total_out > 0) {
            out.resize(strm.total_out);
            return true;
        }
        return false;
    }

    std::vector<ExtractedFile> ExtractFiles(const uint8_t* zipData, size_t zipSize, const std::string& extension) {
        std::vector<ExtractedFile> results;
        if (!zipData || zipSize < 30) return results;

        size_t offset = 0;
        while (offset + 30 <= zipSize) {
            uint32_t signature = ReadLE32(zipData + offset);
            // Local file header signature: 0x04034b50 (PK\x03\x04)
            if (signature != 0x04034b50) {
                break; // Central directory or end of archive reached
            }

            uint16_t compMethod = ReadLE16(zipData + offset + 8);
            uint32_t compSize   = ReadLE32(zipData + offset + 18);
            uint32_t uncompSize = ReadLE32(zipData + offset + 22);
            uint16_t nameLen    = ReadLE16(zipData + offset + 26);
            uint16_t extraLen   = ReadLE16(zipData + offset + 28);

            size_t headerSize = 30 + nameLen + extraLen;
            if (offset + headerSize > zipSize) break;

            std::string fullPath(reinterpret_cast<const char*>(zipData + offset + 30), nameLen);
            const uint8_t* payload = zipData + offset + headerSize;

            if (offset + headerSize + compSize > zipSize) {
                break;
            }

            // Extract file basename
            std::string basename = fullPath;
            size_t lastSlash = basename.find_last_of("/\\");
            if (lastSlash != std::string::npos) {
                basename = basename.substr(lastSlash + 1);
            }

            // Check extension match
            bool match = true;
            if (!extension.empty()) {
                if (basename.size() < extension.size() ||
                    basename.compare(basename.size() - extension.size(), extension.size(), extension) != 0) {
                    match = false;
                }
            }

            if (match && !basename.empty() && compSize > 0) {
                ExtractedFile file;
                file.filename = basename;

                if (compMethod == 0) { // Stored (uncompressed)
                    file.data.assign(payload, payload + compSize);
                    results.push_back(std::move(file));
                } else if (compMethod == 8) { // Deflated
                    if (InflateRaw(payload, compSize, uncompSize, file.data)) {
                        results.push_back(std::move(file));
                    }
                }
            }

            offset += headerSize + compSize;
        }

        return results;
    }

} // namespace ZipExtractor
