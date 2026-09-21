#include "reader_engine.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"

namespace {
constexpr size_t kMaxBookBytes = 6 * 1024 * 1024;
constexpr size_t kMaxWordBytes = 192;

bool is_space(unsigned char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
}

size_t utf8_char_len(unsigned char lead) {
    if ((lead & 0x80u) == 0) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

bool ends_with_any(const std::string &s, const char *chars) {
    if (s.empty()) return false;
    const unsigned char c = static_cast<unsigned char>(s.back());
    return c < 0x80 && std::strchr(chars, static_cast<int>(c));
}

bool ends_with_utf8(const std::string &s, const char *suffix) {
    if (!suffix) return false;
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool is_sentence_end_byte(char c) {
    return c == '.' || c == '!' || c == '?';
}
} // namespace

ReaderEngine::ReaderEngine() : buffer_(nullptr), length_(0), current_start_(0) {}
ReaderEngine::~ReaderEngine() { close(); }

bool ReaderEngine::open(const char *path, size_t saved_position) {
    close();
    if (!path) return false;

    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::rewind(f);
    if (file_size <= 0 || static_cast<size_t>(file_size) > kMaxBookBytes) {
        std::fclose(f);
        return false;
    }

    length_ = static_cast<size_t>(file_size);
    buffer_ = static_cast<char *>(heap_caps_malloc(length_ + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer_) buffer_ = static_cast<char *>(heap_caps_malloc(length_ + 1, MALLOC_CAP_8BIT));
    if (!buffer_) {
        length_ = 0;
        std::fclose(f);
        return false;
    }

    const size_t got = std::fread(buffer_, 1, length_, f);
    std::fclose(f);
    if (got != length_) {
        close();
        return false;
    }
    buffer_[length_] = '\0';

    path_ = path;
    chapter_offsets_.clear();
    chapter_titles_.clear();
    {
        const std::string chapter_path = path_ + ".chap";
        FILE *cf = std::fopen(chapter_path.c_str(), "rb");
        if (cf) {
            char line[384];
            bool first_line = true;
            while (std::fgets(line, sizeof(line), cf)) {
                if (first_line && std::strncmp(line, "RSVPCH3", 7) == 0) {
                    first_line = false;
                    continue;
                }
                first_line = false;
                char *end = nullptr;
                const unsigned long value = std::strtoul(line, &end, 10);
                if (end == line) continue;
                const size_t offset = static_cast<size_t>(value);
                if (offset >= length_ || (!chapter_offsets_.empty() && offset <= chapter_offsets_.back())) continue;
                std::string title;
                if (*end == '\t') {
                    title.assign(end + 1);
                    while (!title.empty() && (title.back() == '\n' || title.back() == '\r')) title.pop_back();
                }
                chapter_offsets_.push_back(offset);
                chapter_titles_.push_back(std::move(title));
            }
            std::fclose(cf);
        }
    }
    current_start_ = std::min(saved_position, length_);
    if (current_start_ > 0 && current_start_ < length_ && !is_space(buffer_[current_start_ - 1])) {
        while (current_start_ < length_ && !is_space(buffer_[current_start_])) ++current_start_;
    }
    current_start_ = skip_ws_forward(current_start_);
    if (current_start_ >= length_) current_start_ = skip_ws_forward(0);
    return current_start_ < length_;
}
void ReaderEngine::close() {
    if (buffer_) heap_caps_free(buffer_);
    buffer_ = nullptr;
    length_ = 0;
    current_start_ = 0;
    path_.clear();
    chapter_offsets_.clear();
    chapter_titles_.clear();
}

size_t ReaderEngine::skip_ws_forward(size_t pos) const {
    while (pos < length_ && is_space(static_cast<unsigned char>(buffer_[pos]))) ++pos;
    return pos;
}

bool ReaderEngine::token_at(size_t start, std::string &word, size_t *next_pos) const {
    if (!buffer_ || start >= length_) return false;
    start = skip_ws_forward(start);
    if (start >= length_) return false;
    size_t end = start;
    while (end < length_ && !is_space(static_cast<unsigned char>(buffer_[end]))) ++end;
    const size_t count = std::min(end - start, kMaxWordBytes);
    word.assign(buffer_ + start, count);
    if (next_pos) *next_pos = skip_ws_forward(end);
    return !word.empty();
}

bool ReaderEngine::current(std::string &word) const {
    return token_at(current_start_, word, nullptr);
}

bool ReaderEngine::next(std::string &word) {
    size_t next_pos = 0;
    std::string ignored;
    if (!token_at(current_start_, ignored, &next_pos) || next_pos >= length_) return false;
    current_start_ = next_pos;
    return current(word);
}

size_t ReaderEngine::token_start_before(size_t pos) const {
    if (!buffer_ || pos == 0) return 0;
    size_t i = pos;
    while (i > 0 && is_space(static_cast<unsigned char>(buffer_[i - 1]))) --i;
    while (i > 0 && !is_space(static_cast<unsigned char>(buffer_[i - 1]))) --i;
    return i;
}

bool ReaderEngine::previous(std::string &word) {
    if (current_start_ == 0) return current(word);
    current_start_ = token_start_before(current_start_);
    return current(word);
}

bool ReaderEngine::sentence_start(std::string &word) {
    if (!buffer_ || length_ == 0) return false;
    if (current_start_ == 0) return current(word);

    size_t i = current_start_;
    // Move left over whitespace before the current token.
    while (i > 0 && is_space(static_cast<unsigned char>(buffer_[i - 1]))) --i;

    // Search backwards for the previous terminal punctuation mark. The next
    // non-space byte after that punctuation is the beginning of the sentence.
    while (i > 0) {
        --i;
        if (is_sentence_end_byte(buffer_[i])) {
            size_t start = skip_ws_forward(i + 1);
            if (start < current_start_) {
                current_start_ = start;
                return current(word);
            }
        }
    }

    current_start_ = skip_ws_forward(0);
    return current(word);
}

size_t ReaderEngine::chapter_index() const {
    if (chapter_offsets_.empty()) return 0;
    const auto it = std::upper_bound(chapter_offsets_.begin(), chapter_offsets_.end(), current_start_);
    if (it == chapter_offsets_.begin()) return 1;
    return static_cast<size_t>(it - chapter_offsets_.begin());
}

const std::string &ReaderEngine::chapter_title(size_t chapter) const {
    static const std::string empty;
    return chapter < chapter_titles_.size() ? chapter_titles_[chapter] : empty;
}

bool ReaderEngine::chapter_bounds(size_t chapter, size_t &start, size_t &end) const {
    if (!valid()) return false;
    if (chapter_offsets_.empty()) {
        if (chapter != 0) return false;
        start = 0;
        end = length_;
        return true;
    }
    if (chapter >= chapter_offsets_.size()) return false;
    start = chapter_offsets_[chapter];
    end = chapter + 1 < chapter_offsets_.size() ? chapter_offsets_[chapter + 1] : length_;
    return end > start;
}

bool ReaderEngine::seek(size_t byte_position) {
    if (!valid()) return false;
    size_t target = std::min(byte_position, length_ - 1);
    // Do not begin in the middle of a UTF-8 character or word.
    if (target > 0 && !is_space(static_cast<unsigned char>(buffer_[target - 1])) &&
        !is_space(static_cast<unsigned char>(buffer_[target]))) {
        while (target < length_ && !is_space(static_cast<unsigned char>(buffer_[target]))) ++target;
    }
    target = skip_ws_forward(target);
    if (target >= length_) target = token_start_before(length_);
    current_start_ = target;
    return current_start_ < length_;
}

float ReaderEngine::progress() const {
    if (!length_) return 0.0f;
    return static_cast<float>(current_start_) / static_cast<float>(length_);
}

void ReaderEngine::split_orp(const std::string &word, std::string &before,
                             std::string &orp, std::string &after) {
    before.clear(); orp.clear(); after.clear();
    if (word.empty()) return;

    size_t offsets[96];
    size_t count = 0;
    for (size_t i = 0; i < word.size() && count < 95;) {
        offsets[count++] = i;
        const size_t step = utf8_char_len(static_cast<unsigned char>(word[i]));
        i += std::min(step, word.size() - i);
    }
    offsets[count] = word.size();
    if (count == 0) return;

    // Spritz-style focal point: shifts slightly right as words get longer.
    size_t index = 0;
    if (count <= 1) index = 0;
    else if (count <= 5) index = 1;
    else if (count <= 9) index = 2;
    else if (count <= 13) index = 3;
    else index = 4;
    if (index >= count) index = count - 1;

    before = word.substr(0, offsets[index]);
    orp = word.substr(offsets[index], offsets[index + 1] - offsets[index]);
    after = word.substr(offsets[index + 1]);
}

uint32_t ReaderEngine::delay_ms(const std::string &word, uint16_t wpm,
                                uint16_t sentence_extra_pct,
                                uint16_t clause_extra_pct) {
    const uint16_t safe_wpm = std::max<uint16_t>(100, std::min<uint16_t>(1200, wpm));
    float multiplier = 1.0f;

    const bool sentence_end = ends_with_any(word, ".!?") ||
                              ends_with_utf8(word, "…”") ||
                              ends_with_utf8(word, "!”") ||
                              ends_with_utf8(word, "?”");
    const bool clause_end = ends_with_any(word, ",;:") ||
                            ends_with_any(word, "-") ||
                            ends_with_utf8(word, "–") ||
                            ends_with_utf8(word, "—");

    if (sentence_end) multiplier += static_cast<float>(sentence_extra_pct) / 100.0f;
    else if (clause_end) multiplier += static_cast<float>(clause_extra_pct) / 100.0f;
    else if (word.size() >= 13) multiplier = 1.20f;
    else if (word.size() >= 9) multiplier = 1.10f;

    return static_cast<uint32_t>((60000.0f / safe_wpm) * multiplier);
}
