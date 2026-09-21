#include "epub_book.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "miniz.h"

namespace {

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t csize = 0;
    uint32_t usize = 0;
    uint32_t local = 0;
};

uint16_t u16(const unsigned char *p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t u32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

class ZipReader {
public:
    ~ZipReader() {
        if (file_) std::fclose(file_);
    }

    bool open(const char *path) {
        file_ = std::fopen(path, "rb");
        if (!file_) return false;

        std::fseek(file_, 0, SEEK_END);
        const long size = std::ftell(file_);
        if (size < 22) return false;

        const long tail_size = std::min<long>(size, 65557);
        std::vector<unsigned char> tail(static_cast<size_t>(tail_size));
        std::fseek(file_, size - tail_size, SEEK_SET);
        if (std::fread(tail.data(), 1, tail.size(), file_) != tail.size()) return false;

        long eocd = -1;
        for (long i = static_cast<long>(tail.size()) - 22; i >= 0; --i) {
            if (u32(&tail[static_cast<size_t>(i)]) == 0x06054b50u) {
                eocd = i;
                break;
            }
        }
        if (eocd < 0) return false;

        const uint16_t count = u16(&tail[static_cast<size_t>(eocd) + 10]);
        const uint32_t central_dir = u32(&tail[static_cast<size_t>(eocd) + 16]);
        if (count == 0 || count > 4096) return false;

        std::fseek(file_, static_cast<long>(central_dir), SEEK_SET);
        entries_.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            unsigned char header[46];
            if (std::fread(header, 1, sizeof(header), file_) != sizeof(header)) return false;
            if (u32(header) != 0x02014b50u) return false;

            const uint16_t name_len = u16(header + 28);
            const uint16_t extra_len = u16(header + 30);
            const uint16_t comment_len = u16(header + 32);

            ZipEntry entry;
            entry.method = u16(header + 10);
            entry.csize = u32(header + 20);
            entry.usize = u32(header + 24);
            entry.local = u32(header + 42);

            std::vector<char> name(static_cast<size_t>(name_len) + 1, 0);
            if (name_len && std::fread(name.data(), 1, name_len, file_) != name_len) return false;
            entry.name.assign(name.data(), name_len);
            std::fseek(file_, static_cast<long>(extra_len + comment_len), SEEK_CUR);
            entries_.push_back(std::move(entry));
        }
        return true;
    }

    bool read(const std::string &name, std::string &out) {
        const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ZipEntry &z) {
            return z.name == name;
        });
        if (it == entries_.end()) return false;

        constexpr uint32_t kMaxEntry = 8u * 1024u * 1024u;
        if (it->usize > kMaxEntry || it->csize > kMaxEntry) return false;

        std::fseek(file_, static_cast<long>(it->local), SEEK_SET);
        unsigned char local_header[30];
        if (std::fread(local_header, 1, sizeof(local_header), file_) != sizeof(local_header)) return false;
        if (u32(local_header) != 0x04034b50u) return false;

        const uint16_t name_len = u16(local_header + 26);
        const uint16_t extra_len = u16(local_header + 28);
        std::fseek(file_, static_cast<long>(name_len + extra_len), SEEK_CUR);

        std::vector<unsigned char> input(it->csize);
        if (it->csize && std::fread(input.data(), 1, input.size(), file_) != input.size()) return false;

        out.assign(it->usize, '\0');
        if (it->method == 0) {
            if (it->csize != it->usize) return false;
            if (it->usize) std::memcpy(out.data(), input.data(), it->usize);
            return true;
        }
        if (it->method != 8) return false;

        const size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(), input.data(), input.size(), 0);
        if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
        out.resize(got);
        return true;
    }

private:
    FILE *file_ = nullptr;
    std::vector<ZipEntry> entries_;
};

std::string dirname_of(const std::string &path) {
    const auto pos = path.find_last_of('/');
    return pos == std::string::npos ? "" : path.substr(0, pos + 1);
}

std::string normalize_join(const std::string &base_file, const std::string &relative) {
    if (relative.empty()) return {};
    std::string path = dirname_of(base_file) + relative;
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string part = path.substr(start, end == std::string::npos ? path.size() - start : end - start);
        if (part == "..") {
            if (!segments.empty()) segments.pop_back();
        } else if (!part.empty() && part != ".") {
            segments.push_back(part);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string result;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i) result.push_back('/');
        result += segments[i];
    }
    return result;
}

std::string attr_value(const std::string &tag, const char *key) {
    if (!key || !*key) return {};
    const std::string wanted(key);
    size_t pos = 0;

    while (pos < tag.size()) {
        pos = tag.find(wanted, pos);
        if (pos == std::string::npos) return {};

        // Attribute name must not be part of another XML name.
        const bool left_ok = pos == 0 ||
            std::isspace(static_cast<unsigned char>(tag[pos - 1])) ||
            tag[pos - 1] == '<' || tag[pos - 1] == '/';
        const size_t after_name = pos + wanted.size();
        const bool right_ok = after_name >= tag.size() ||
            std::isspace(static_cast<unsigned char>(tag[after_name])) ||
            tag[after_name] == '=';
        if (!left_ok || !right_ok) {
            pos = after_name;
            continue;
        }

        size_t p = after_name;
        while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
        if (p >= tag.size() || tag[p] != '=') {
            pos = after_name;
            continue;
        }
        ++p;
        while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
        if (p >= tag.size()) return {};

        const char quote = tag[p];
        if (quote != '\'' && quote != '"') return {};
        const size_t end = tag.find(quote, p + 1);
        if (end == std::string::npos) return {};
        return tag.substr(p + 1, end - p - 1);
    }
    return {};
}

bool find_xml_start_tag(const std::string &xml, const char *local_name, std::string &tag_out) {
    if (!local_name || !*local_name) return false;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const size_t name_begin = pos + 1;
        if (name_begin >= xml.size()) return false;
        const char first = xml[name_begin];
        if (first == '/' || first == '!' || first == '?') {
            pos = name_begin + 1;
            continue;
        }

        size_t name_end = name_begin;
        while (name_end < xml.size()) {
            const char c = xml[name_end];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/') break;
            ++name_end;
        }
        if (name_end == name_begin) {
            pos = name_begin + 1;
            continue;
        }

        std::string name = xml.substr(name_begin, name_end - name_begin);
        const size_t colon = name.find_last_of(':');
        if (colon != std::string::npos) name = name.substr(colon + 1);

        if (name == local_name) {
            const size_t tag_end = xml.find('>', name_end);
            if (tag_end == std::string::npos) return false;
            tag_out = xml.substr(pos, tag_end - pos + 1);
            return true;
        }
        pos = name_end;
    }
    return false;
}


bool find_next_xml_start_tag(const std::string &xml, const char *local_name, size_t &cursor, std::string &tag_out) {
    if (!local_name || !*local_name) return false;
    size_t pos = cursor;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const size_t name_begin = pos + 1;
        if (name_begin >= xml.size()) return false;
        const char first = xml[name_begin];
        if (first == '/' || first == '!' || first == '?') { pos = name_begin + 1; continue; }
        size_t name_end = name_begin;
        while (name_end < xml.size()) {
            const char c = xml[name_end];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/') break;
            ++name_end;
        }
        if (name_end == name_begin) { pos = name_begin + 1; continue; }
        std::string name = xml.substr(name_begin, name_end - name_begin);
        const size_t colon = name.find_last_of(':');
        if (colon != std::string::npos) name = name.substr(colon + 1);
        if (name == local_name) {
            const size_t tag_end = xml.find('>', name_end);
            if (tag_end == std::string::npos) return false;
            tag_out = xml.substr(pos, tag_end - pos + 1);
            cursor = tag_end + 1;
            return true;
        }
        pos = name_end;
    }
    cursor = xml.size();
    return false;
}

std::string uri_decode(std::string value) {
    const size_t fragment = value.find_first_of("#?");
    if (fragment != std::string::npos) value.resize(fragment);
    std::string out;
    out.reserve(value.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int a = hex(value[i + 1]);
            const int b = hex(value[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back(static_cast<char>((a << 4) | b));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string decode_entities(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (input[i] != '&') {
            out.push_back(input[i++]);
            continue;
        }
        const size_t semi = input.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16) {
            out.push_back(input[i++]);
            continue;
        }
        const std::string entity = input.substr(i + 1, semi - i - 1);
        if (entity == "nbsp") out.push_back(' ');
        else if (entity == "amp") out.push_back('&');
        else if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "quot") out.push_back('"');
        else if (entity == "apos") out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
            const char *number = entity.c_str() + (hex ? 2 : 1);
            char *end = nullptr;
            const unsigned long cp = std::strtoul(number, &end, hex ? 16 : 10);
            if (end && *end == '\0' && cp == 0xA0ul) out.push_back(' ');
            else if (end && *end == '\0' && cp > 0 && cp <= 0x10FFFFul) append_utf8(out, static_cast<uint32_t>(cp));
            else out.append(input, i, semi - i + 1);
        } else {
            out.append(input, i, semi - i + 1);
        }
        i = semi + 1;
    }
    return out;
}

bool starts_with_ci(const std::string &value, const char *prefix) {
    const size_t n = std::strlen(prefix);
    if (value.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    }
    return true;
}

std::string html_to_text(const std::string &html) {
    std::string raw;
    raw.reserve(html.size() / 2);
    bool in_tag = false;
    bool skip_block = false;
    std::string tag;

    for (size_t i = 0; i < html.size(); ++i) {
        const char ch = html[i];
        if (!in_tag && ch == '<') {
            in_tag = true;
            tag.clear();
            continue;
        }
        if (in_tag) {
            if (ch != '>') {
                if (tag.size() < 256) tag.push_back(ch);
                continue;
            }
            in_tag = false;
            std::string lower = tag;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            while (!lower.empty() && std::isspace(static_cast<unsigned char>(lower.front()))) lower.erase(lower.begin());

            if (starts_with_ci(lower, "script") || starts_with_ci(lower, "style")) skip_block = true;
            if (starts_with_ci(lower, "/script") || starts_with_ci(lower, "/style")) skip_block = false;
            if (!skip_block && (starts_with_ci(lower, "br") || starts_with_ci(lower, "/p") || starts_with_ci(lower, "/div") ||
                                starts_with_ci(lower, "/li") || starts_with_ci(lower, "/h1") || starts_with_ci(lower, "/h2") ||
                                starts_with_ci(lower, "/h3") || starts_with_ci(lower, "/h4") || starts_with_ci(lower, "/h5") ||
                                starts_with_ci(lower, "/h6"))) {
                raw.push_back('\n');
            }
            continue;
        }
        if (!skip_block) raw.push_back(ch);
    }

    raw = decode_entities(raw);
    auto replace_all = [&](const char *from, const char *to) {
        const size_t from_len = std::strlen(from);
        const size_t to_len = std::strlen(to);
        size_t pos = 0;
        while (from_len && (pos = raw.find(from, pos)) != std::string::npos) {
            raw.replace(pos, from_len, to);
            pos += to_len;
        }
    };
    replace_all("\xC2\xA0", " ");
    replace_all("\xE2\x80\x87", " ");
    replace_all("\xE2\x80\x89", " ");
    replace_all("\xE2\x80\xAF", " ");
    replace_all("\xE2\x80\x8B", "");
    replace_all("\xE2\x81\xA0", "");
    std::string clean;
    clean.reserve(raw.size());
    bool last_space = true;
    bool last_newline = false;
    for (unsigned char ch : raw) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            while (!clean.empty() && clean.back() == ' ') clean.pop_back();
            if (!clean.empty() && !last_newline) clean.push_back('\n');
            last_space = true;
            last_newline = true;
        } else if (ch < 0x80 && std::isspace(ch)) {
            if (!last_space && !last_newline) clean.push_back(' ');
            last_space = true;
        } else {
            clean.push_back(static_cast<char>(ch));
            last_space = false;
            last_newline = false;
        }
    }
    return clean;
}

std::string between(const std::string &source, const std::string &begin, const std::string &end) {
    size_t pos = source.find(begin);
    if (pos == std::string::npos) return {};
    pos += begin.size();
    const size_t finish = source.find(end, pos);
    return finish == std::string::npos ? std::string() : source.substr(pos, finish - pos);
}


struct TocEntry {
    std::string zip_path;
    std::string fragment;
    std::string title;
};

bool has_token(const std::string &value, const char *token) {
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && std::isspace(static_cast<unsigned char>(value[pos]))) ++pos;
        const size_t end = value.find_first_of(" \t\r\n", pos);
        const size_t count = (end == std::string::npos ? value.size() : end) - pos;
        if (count == std::strlen(token) && value.compare(pos, count, token) == 0) return true;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

std::string clean_toc_title(const std::string &markup) {
    std::string title = html_to_text(markup);
    for (char &c : title) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    while (!title.empty() && title.front() == ' ') title.erase(title.begin());
    while (!title.empty() && title.back() == ' ') title.pop_back();
    if (title.size() > 240) title.resize(240);
    return title;
}

TocEntry toc_reference(const std::string &base_file, const std::string &href,
                       const std::string &title) {
    TocEntry out;
    const size_t hash = href.find('#');
    const std::string file = hash == std::string::npos ? href : href.substr(0, hash);
    if (hash != std::string::npos) out.fragment = uri_decode(href.substr(hash + 1));
    out.zip_path = normalize_join(base_file, uri_decode(file));
    out.title = clean_toc_title(title);
    return out;
}

std::vector<TocEntry> parse_epub3_nav(const std::string &nav_path, const std::string &xml) {
    std::vector<TocEntry> out;
    std::string lower = xml;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    size_t nav = 0;
    while ((nav = lower.find("<nav", nav)) != std::string::npos) {
        const size_t tag_end = lower.find('>', nav);
        if (tag_end == std::string::npos) break;
        const std::string tag = xml.substr(nav, tag_end - nav + 1);
        const std::string type = attr_value(tag, "epub:type") + " " + attr_value(tag, "type");
        if (type.find("toc") == std::string::npos) { nav = tag_end + 1; continue; }
        const size_t nav_end = lower.find("</nav", tag_end + 1);
        if (nav_end == std::string::npos) break;
        size_t cursor = tag_end + 1;
        std::string anchor;
        while (cursor < nav_end && find_next_xml_start_tag(xml, "a", cursor, anchor)) {
            if (cursor > nav_end) break;
            const std::string href = attr_value(anchor, "href");
            const size_t close = lower.find("</a", cursor);
            if (close == std::string::npos || close > nav_end) break;
            if (!href.empty()) out.push_back(toc_reference(nav_path, href, xml.substr(cursor, close - cursor)));
            const size_t close_end = lower.find('>', close);
            cursor = close_end == std::string::npos ? nav_end : close_end + 1;
        }
        break;
    }
    return out;
}

std::vector<TocEntry> parse_epub2_ncx(const std::string &ncx_path, const std::string &xml) {
    std::vector<TocEntry> out;
    size_t cursor = 0;
    std::string content;
    while (find_next_xml_start_tag(xml, "content", cursor, content)) {
        const std::string href = attr_value(content, "src");
        if (href.empty()) continue;
        std::string title;
        const size_t before = cursor > content.size() ? cursor - content.size() : 0;
        const size_t text_open = xml.rfind("<text", before);
        if (text_open != std::string::npos) {
            const size_t text_start = xml.find('>', text_open);
            const size_t text_end = text_start == std::string::npos ? std::string::npos : xml.find("</text", text_start + 1);
            if (text_start != std::string::npos && text_end != std::string::npos)
                title = xml.substr(text_start + 1, text_end - text_start - 1);
        }
        out.push_back(toc_reference(ncx_path, href, title));
    }
    return out;
}

size_t fragment_text_offset(const std::string &xhtml, const std::string &fragment) {
    if (fragment.empty()) return 0;
    const std::string id1 = "id=\"" + fragment + "\"";
    const std::string id2 = "id='" + fragment + "'";
    size_t pos = xhtml.find(id1);
    if (pos == std::string::npos) pos = xhtml.find(id2);
    if (pos == std::string::npos) return 0;
    const size_t tag = xhtml.rfind('<', pos);
    return html_to_text(xhtml.substr(0, tag == std::string::npos ? pos : tag)).size();
}

}  // namespace

std::string EpubBook::extract_text(const char *path, std::string *title) {
    ZipReader zip;
    if (!zip.open(path)) return {};

    std::string container;
    if (!zip.read("META-INF/container.xml", container)) return {};

    std::string rootfile_tag;
    if (!find_xml_start_tag(container, "rootfile", rootfile_tag)) return {};
    const std::string opf_path = attr_value(rootfile_tag, "full-path");
    if (opf_path.empty()) return {};

    std::string opf;
    if (!zip.read(opf_path, opf)) return {};

    if (title) {
        std::string parsed = between(opf, "<dc:title>", "</dc:title>");
        if (parsed.empty()) parsed = between(opf, "<title>", "</title>");
        *title = decode_entities(parsed);
    }

    std::map<std::string, std::string> manifest;
    size_t pos = 0;
    std::string tag;
    while (find_next_xml_start_tag(opf, "item", pos, tag)) {
        const std::string id = attr_value(tag, "id");
        const std::string href = uri_decode(decode_entities(attr_value(tag, "href")));
        if (!id.empty() && !href.empty()) manifest[id] = href;
    }

    std::vector<std::string> spine;
    pos = 0;
    while (find_next_xml_start_tag(opf, "itemref", pos, tag)) {
        const std::string idref = attr_value(tag, "idref");
        if (!idref.empty()) spine.push_back(idref);
    }
    if (spine.empty()) return {};

    std::string all;
    all.reserve(512 * 1024);
    constexpr size_t kMaxTextBytes = 6u * 1024u * 1024u;

    for (const auto &id : spine) {
        const auto it = manifest.find(id);
        if (it == manifest.end()) continue;
        std::string xhtml;
        if (!zip.read(normalize_join(opf_path, it->second), xhtml)) continue;
        std::string text = html_to_text(xhtml);
        if (text.size() < 2) continue;
        if (all.size() + text.size() + 2 > kMaxTextBytes) break;
        all += text;
        all += "\n\n";
    }
    return all;
}

bool EpubBook::extract_to_file(const char *path, const char *cache_path, std::string *title, std::string *error) {
    if (error) error->clear();
    if (!path || !cache_path) {
        if (error) *error = "Ungueltiger EPUB- oder Cache-Pfad.";
        return false;
    }

    ZipReader zip;
    if (!zip.open(path)) {
        if (error) *error = "EPUB-ZIP konnte nicht geoeffnet werden.";
        return false;
    }

    std::string container;
    if (!zip.read("META-INF/container.xml", container)) {
        if (error) *error = "META-INF/container.xml fehlt oder ist nicht lesbar.";
        return false;
    }

    std::string rootfile_tag;
    if (!find_xml_start_tag(container, "rootfile", rootfile_tag)) {
        if (error) *error = "Keine EPUB-rootfile-Angabe gefunden.";
        return false;
    }
    const std::string opf_path = attr_value(rootfile_tag, "full-path");
    if (opf_path.empty()) {
        if (error) *error = "EPUB-Paketdatei konnte nicht bestimmt werden.";
        return false;
    }

    std::string opf;
    if (!zip.read(opf_path, opf)) {
        if (error) *error = "EPUB-Paketdatei konnte nicht gelesen werden.";
        return false;
    }

    if (title) {
        std::string parsed = between(opf, "<dc:title>", "</dc:title>");
        if (parsed.empty()) parsed = between(opf, "<title>", "</title>");
        *title = decode_entities(parsed);
    }

    std::map<std::string, std::string> manifest;
    std::map<std::string, std::string> manifest_properties;
    size_t pos = 0;
    std::string tag;
    while (find_next_xml_start_tag(opf, "item", pos, tag)) {
        const std::string id = attr_value(tag, "id");
        const std::string href = uri_decode(decode_entities(attr_value(tag, "href")));
        if (!id.empty() && !href.empty()) {
            manifest[id] = href;
            manifest_properties[id] = attr_value(tag, "properties");
        }
    }

    std::vector<std::string> spine;
    pos = 0;
    while (find_next_xml_start_tag(opf, "itemref", pos, tag)) {
        const std::string idref = attr_value(tag, "idref");
        if (!idref.empty()) spine.push_back(idref);
    }
    if (spine.empty()) {
        if (error) *error = "EPUB enthaelt keine lesbare Spine.";
        return false;
    }

    // EPUB 3 navigation document first, EPUB 2 NCX second. Spine boundaries
    // remain a fallback for books without a usable table of contents.
    std::vector<TocEntry> toc;
    for (const auto &item : manifest_properties) {
        if (!has_token(item.second, "nav")) continue;
        std::string nav_xml;
        const std::string nav_path = normalize_join(opf_path, manifest[item.first]);
        if (zip.read(nav_path, nav_xml)) toc = parse_epub3_nav(nav_path, nav_xml);
        if (!toc.empty()) break;
    }
    if (toc.empty()) {
        std::string spine_tag;
        if (find_xml_start_tag(opf, "spine", spine_tag)) {
            const std::string toc_id = attr_value(spine_tag, "toc");
            const auto ncx = manifest.find(toc_id);
            if (ncx != manifest.end()) {
                std::string ncx_xml;
                const std::string ncx_path = normalize_join(opf_path, ncx->second);
                if (zip.read(ncx_path, ncx_xml)) toc = parse_epub2_ncx(ncx_path, ncx_xml);
            }
        }
    }

    FILE *out = std::fopen(cache_path, "wb");
    if (!out) {
        if (error) *error = "Cache-Datei konnte auf der microSD nicht angelegt werden.";
        return false;
    }

    constexpr size_t kMaxTextBytes = 6u * 1024u * 1024u;
    size_t total_written = 0;
    size_t chapters_written = 0;
    bool write_ok = true;
    std::vector<size_t> spine_offsets;
    std::vector<std::pair<size_t, std::string>> toc_chapters;

    for (const auto &id : spine) {
        const auto it = manifest.find(id);
        if (it == manifest.end()) continue;

        const std::string zip_path = normalize_join(opf_path, it->second);
        std::string xhtml;
        if (!zip.read(zip_path, xhtml)) continue;

        std::string text = html_to_text(xhtml);
        const size_t section_start = total_written;
        for (const TocEntry &entry : toc) {
            if (entry.zip_path != zip_path) continue;
            const size_t local = std::min(fragment_text_offset(xhtml, entry.fragment),
                                          text.empty() ? size_t{0} : text.size() - 1);
            toc_chapters.emplace_back(section_start + local, entry.title);
        }
        xhtml.clear();
        if (text.size() < 2) continue;

        if (total_written + text.size() + 2 > kMaxTextBytes) {
            const size_t remaining = kMaxTextBytes > total_written ? kMaxTextBytes - total_written : 0;
            if (remaining > 2) text.resize(remaining - 2);
            else break;
        }

        if (!text.empty()) {
            spine_offsets.push_back(total_written);
            const size_t n = std::fwrite(text.data(), 1, text.size(), out);
            if (n != text.size()) {
                write_ok = false;
                break;
            }
            total_written += n;
            static const char sep[] = "\n\n";
            if (std::fwrite(sep, 1, 2, out) != 2) {
                write_ok = false;
                break;
            }
            total_written += 2;
            ++chapters_written;
        }

        // Yield between chapters so long EPUB parsing cannot starve the idle task.
        vTaskDelay(pdMS_TO_TICKS(1));
        if (total_written >= kMaxTextBytes) break;
    }

    const int close_result = std::fclose(out);
    if (!write_ok || close_result != 0 || chapters_written == 0 || total_written <= 16) {
        std::remove(cache_path);
        if (error) *error = "EPUB-Text konnte nicht sicher extrahiert werden.";
        return false;
    }

    std::sort(toc_chapters.begin(), toc_chapters.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::pair<size_t, std::string>> chapters;
    for (auto &entry : toc_chapters) {
        if (entry.first >= total_written) continue;
        if (!chapters.empty() && entry.first == chapters.back().first) {
            if (chapters.back().second.empty()) chapters.back().second = entry.second;
            continue;
        }
        chapters.push_back(std::move(entry));
    }
    if (chapters.empty()) {
        for (const size_t offset : spine_offsets)
            chapters.emplace_back(offset, std::string());
    }

    const std::string chapter_path = std::string(cache_path) + ".chap";
    FILE *chap = std::fopen(chapter_path.c_str(), "wb");
    if (chap) {
        std::fputs("RSVPCH3\n", chap);
        for (const auto &entry : chapters) {
            std::fprintf(chap, "%u\t%s\n", static_cast<unsigned>(entry.first), entry.second.c_str());
        }
        std::fclose(chap);
    }

    return true;
}
