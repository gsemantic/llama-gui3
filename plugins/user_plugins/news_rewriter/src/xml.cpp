#include "xml.h"

#include <cstdlib>

namespace news_rewriter {

namespace {

std::string local_name(const std::string& full) {
    const std::size_t colon = full.find(':');
    return colon == std::string::npos ? full : full.substr(colon + 1);
}

// Декодирует XML-сущности в тексте.
std::string decode_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            const std::size_t semi = s.find(';', i);
            if (semi == std::string::npos || semi - i > 12) {
                out += s[i++];
                continue;
            }
            const std::string ent = s.substr(i + 1, semi - i - 1);
            if (ent == "amp")      out += '&';
            else if (ent == "lt")  out += '<';
            else if (ent == "gt")  out += '>';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (ent.size() > 1 && ent[0] == '#') {
                const bool hex = ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X');
                const char* p = ent.c_str() + (hex ? 2 : 1);
                char* end = nullptr;
                const unsigned long code = std::strtoul(p, &end, hex ? 16 : 10);
                if (end && *end == '\0' && code > 0) {
                    if (code < 0x80) out += static_cast<char>(code);
                    else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                }
            } else {
                out += s.substr(i, semi - i + 1);
            }
            i = semi + 1;
        } else {
            out += s[i++];
        }
    }
    return out;
}

class XmlParser {
public:
    explicit XmlParser(const std::string& xml) : s_(xml) {}

    bool parse(XmlNode& root) {
        skip_ws();
        if (!skip_declarations()) return false;
        XmlNode node;
        if (!parse_element(node)) return false;
        root = std::move(node);
        return true;
    }

private:
    const std::string& s_;
    std::size_t pos_ = 0;

    bool at_end() const { return pos_ >= s_.size(); }

    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' ||
                                    s_[pos_] == '\n' || s_[pos_] == '\r')) {
            pos_++;
        }
    }

    // Пропускает XML-объявление, DOCTYPE, комментарии, PI до первого элемента.
    bool skip_declarations() {
        for (;;) {
            skip_ws();
            if (at_end() || s_[pos_] != '<') return true;
            if (s_.compare(pos_, 2, "<?") == 0) {
                const std::size_t end = s_.find("?>", pos_ + 2);
                if (end == std::string::npos) return false;
                pos_ = end + 2;
            } else if (s_.compare(pos_, 9, "<!DOCTYPE") == 0) {
                // до закрывающей ">" вне кавычек
                const std::size_t end = find_matching_bracket(pos_ + 9, '>');
                if (end == std::string::npos) return false;
                pos_ = end + 1;
            } else if (s_.compare(pos_, 4, "<!--") == 0) {
                const std::size_t end = s_.find("-->", pos_ + 4);
                if (end == std::string::npos) return false;
                pos_ = end + 3;
            } else {
                return true;
            }
        }
    }

    std::size_t find_matching_bracket(std::size_t from, char closing) {
        for (std::size_t i = from; i < s_.size(); i++) {
            if (s_[i] == closing) return i;
        }
        return std::string::npos;
    }

    bool parse_name(std::string& out) {
        if (at_end() || !is_name_start(s_[pos_])) return false;
        const std::size_t start = pos_;
        while (pos_ < s_.size() && is_name_char(s_[pos_])) pos_++;
        out = s_.substr(start, pos_ - start);
        return true;
    }

    static bool is_name_start(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool is_name_char(char c) {
        return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'
            || c == ':';
    }

    bool parse_attrs(std::map<std::string, std::string>& attrs) {
        for (;;) {
            skip_ws();
            if (at_end()) return false;
            if (s_[pos_] == '>' || s_[pos_] == '/') return true;
            std::string name;
            if (!parse_name(name)) return false;
            skip_ws();
            if (at_end() || s_[pos_] != '=') return false;
            pos_++;
            skip_ws();
            if (at_end() || (s_[pos_] != '"' && s_[pos_] != '\'')) return false;
            const char quote = s_[pos_++];
            const std::size_t start = pos_;
            while (pos_ < s_.size() && s_[pos_] != quote) pos_++;
            if (at_end()) return false;
            attrs[name] = decode_entities(s_.substr(start, pos_ - start));
            pos_++;
        }
    }

    bool parse_element(XmlNode& node) {
        skip_ws();
        if (at_end() || s_[pos_] != '<') return false;
        pos_++;
        if (!parse_name(node.full_name)) return false;
        node.name = local_name(node.full_name);

        if (!parse_attrs(node.attrs)) return false;

        bool self_closing = false;
        if (!at_end() && s_[pos_] == '/') {
            self_closing = true;
            pos_++;
        }
        if (at_end() || s_[pos_] != '>') return false;
        pos_++;

        if (self_closing) return true;

        // Содержимое: текст, вложенные элементы, CDATA, комментарии.
        for (;;) {
            skip_ws();
            if (at_end()) return false;
            if (s_[pos_] == '<') {
                if (s_.compare(pos_, 2, "</") == 0) {
                    // закрывающий тег
                    const std::size_t close = s_.find('>', pos_ + 2);
                    if (close == std::string::npos) return false;
                    const std::string closing = s_.substr(pos_ + 2, close - pos_ - 2);
                    if (local_name(closing) != node.name) return false;
                    pos_ = close + 1;
                    return true;
                }
                if (s_.compare(pos_, 4, "<!--") == 0) {
                    const std::size_t end = s_.find("-->", pos_ + 4);
                    if (end == std::string::npos) return false;
                    pos_ = end + 3;
                    continue;
                }
                if (s_.compare(pos_, 9, "<![CDATA[") == 0) {
                    const std::size_t end = s_.find("]]>", pos_ + 9);
                    if (end == std::string::npos) return false;
                    node.text += s_.substr(pos_ + 9, end - pos_ - 9);
                    pos_ = end + 3;
                    continue;
                }
                if (s_.compare(pos_, 2, "<?") == 0) {
                    const std::size_t end = s_.find("?>", pos_ + 2);
                    if (end == std::string::npos) return false;
                    pos_ = end + 2;
                    continue;
                }
                // вложенный элемент
                XmlNode child;
                if (!parse_element(child)) return false;
                node.children.push_back(std::move(child));
                continue;
            }
            // текст до следующего '<'
            const std::size_t next = s_.find('<', pos_);
            const std::size_t len = next == std::string::npos ? s_.size() - pos_ : next - pos_;
            node.text += s_.substr(pos_, len);
            pos_ += len;
        }
    }
};

} // namespace

bool parse_xml(const std::string& xml, XmlNode& root) {
    XmlParser parser(xml);
    return parser.parse(root);
}

const XmlNode* find_child(const XmlNode& node, const std::string& local_name) {
    for (const auto& c : node.children) {
        if (c.name == local_name) return &c;
    }
    return nullptr;
}

std::vector<const XmlNode*> find_children(const XmlNode& node,
                                          const std::string& local_name) {
    std::vector<const XmlNode*> out;
    for (const auto& c : node.children) {
        if (c.name == local_name) out.push_back(&c);
    }
    return out;
}

std::string child_text(const XmlNode& node, const std::string& local_name) {
    const XmlNode* c = find_child(node, local_name);
    return c ? full_text(*c) : std::string();
}

std::string full_text(const XmlNode& node) {
    std::string out = node.text;
    for (const auto& c : node.children) {
        out += full_text(c);
    }
    return decode_entities(out);
}

} // namespace news_rewriter
