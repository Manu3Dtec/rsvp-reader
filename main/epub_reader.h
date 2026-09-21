#pragma once

#include <string>

class EpubReader {
public:
    // Extracts readable UTF-8 text from an EPUB and writes it to a cache TXT file.
    // Returns true on success. On failure, 'error' contains a user-readable reason.
    static bool to_text_cache(const char *epub_path, const char *cache_path, std::string &error);
};
