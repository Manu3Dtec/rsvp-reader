#pragma once
#include <string>

// Lightweight EPUB 2/3 text extractor for ESP32-S3.
class EpubBook {
public:
    // Legacy helper: returns the whole book as UTF-8 text.
    static std::string extract_text(const char *epub_path, std::string *title = nullptr);

    // Memory-saving path used by the reader: extracts one spine document at a time
    // and writes plain UTF-8 text directly to cache_path.
    static bool extract_to_file(const char *epub_path,
                                const char *cache_path,
                                std::string *title = nullptr,
                                std::string *error = nullptr);
};
