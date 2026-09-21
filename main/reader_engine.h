#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ReaderEngine {
public:
    ReaderEngine();
    ~ReaderEngine();

    bool open(const char *path, size_t saved_position = 0);
    void close();
    bool valid() const { return buffer_ != nullptr && length_ > 0; }

    bool current(std::string &word) const;
    bool next(std::string &word);
    bool previous(std::string &word);
    bool sentence_start(std::string &word);

    size_t position() const { return current_start_; }
    size_t length() const { return length_; }
    float progress() const;
    size_t chapter_count() const { return chapter_offsets_.size(); }
    size_t chapter_index() const;
    const std::string &chapter_title(size_t chapter) const;
    bool chapter_bounds(size_t chapter, size_t &start, size_t &end) const;
    bool seek(size_t byte_position);
    const std::string &path() const { return path_; }

    static void split_orp(const std::string &word, std::string &before,
                          std::string &orp, std::string &after);
    static uint32_t delay_ms(const std::string &word, uint16_t wpm,
                             uint16_t sentence_extra_pct = 100,
                             uint16_t clause_extra_pct = 40);

private:
    bool token_at(size_t start, std::string &word, size_t *next_pos) const;
    size_t skip_ws_forward(size_t pos) const;
    size_t token_start_before(size_t pos) const;

    char *buffer_;
    size_t length_;
    size_t current_start_;
    std::string path_;
    std::vector<size_t> chapter_offsets_;
    std::vector<std::string> chapter_titles_;
};
