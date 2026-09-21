#include "epub_reader.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "epub_book.h"

namespace {
bool cache_is_fresh(const char *epub_path, const char *cache_path) {
    struct stat book {};
    struct stat cache {};
    if (stat(epub_path, &book) != 0 || stat(cache_path, &cache) != 0) return false;
    if (cache.st_size <= 16) return false;
    // FAT timestamps may have coarse resolution, therefore >= is intentional.
    return cache.st_mtime >= book.st_mtime;
}

bool chapter_cache_is_current(const std::string &path) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char header[8]{};
    const size_t got = std::fread(header, 1, 7, file);
    std::fclose(file);
    return got == 7 && std::strncmp(header, "RSVPCH3", 7) == 0;
}
}

bool EpubReader::to_text_cache(const char *epub_path, const char *cache_path, std::string &error) {
    error.clear();
    if (!epub_path || !cache_path) {
        error = "Ungueltiger EPUB- oder Cache-Pfad.";
        return false;
    }

    const std::string chapter_path = std::string(cache_path) + ".chap";
    struct stat chapter_stat {};
    if (cache_is_fresh(epub_path, cache_path) && stat(chapter_path.c_str(), &chapter_stat) == 0 &&
        chapter_stat.st_size > 0 && chapter_cache_is_current(chapter_path)) {
        return true;
    }
    std::remove(cache_path);
    std::remove(chapter_path.c_str());

    std::string title;
    if (!EpubBook::extract_to_file(epub_path, cache_path, &title, &error)) {
        if (error.empty()) {
            error = "EPUB konnte nicht gelesen werden. DRM oder EPUB-Struktur wird moeglicherweise nicht unterstuetzt.";
        }
        return false;
    }
    return true;
}
