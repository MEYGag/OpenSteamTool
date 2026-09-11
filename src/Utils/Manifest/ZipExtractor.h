#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ZipExtractor {

    struct ExtractedFile {
        std::string filename;
        std::vector<uint8_t> data;
    };

    // Extracts all files matching the given extension (e.g. ".manifest") from a zip archive buffer in memory.
    // If extension is empty, extracts all files.
    std::vector<ExtractedFile> ExtractFiles(const uint8_t* zipData, size_t zipSize, const std::string& extension = ".manifest");

} // namespace ZipExtractor
