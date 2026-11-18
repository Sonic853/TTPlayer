#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::skin {
// The skin format uses 004C6A82/004C7177 names and first-attribute lookup,
// not namespace-aware XML. Only element/attribute data is exposed to skins.
struct LegacyXmlNode {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> attributes;
    std::vector<std::unique_ptr<LegacyXmlNode>> children;
    std::wstring Attribute(std::wstring_view key) const {
        for (const auto& [attribute, value] : attributes) if (attribute == key) return value;
        return {};
    }
    std::vector<LegacyXmlNode*> Children(std::wstring_view filter = {}) const {
        std::vector<LegacyXmlNode*> result;
        for (const auto& child : children)
            if (filter.empty() || child->name == filter) result.push_back(child.get());
        return result;
    }
};

class LegacyXmlDocument {
public:
    static constexpr size_t MaxBytes = 16 * 1024 * 1024;
    explicit LegacyXmlDocument(std::wstring text) : text_(std::move(text)) {
        if (text_.empty() || text_.size() > MaxBytes) Fail("invalid length");
        Space();
        while (Special()) Space();
        root_ = Element(0);
        // 004A794C consumes the first element; trailing roots/data are ignored.
    }
    std::vector<LegacyXmlNode*> Nodes(std::wstring_view path) const {
        std::vector<LegacyXmlNode*> result;
        if (!root_) return result;
        if (!path.empty() && path.front() == L'/') path.remove_prefix(1);
        const auto slash = path.find(L'/');
        const auto first = path.substr(0, slash);
        if (first != L"*[1]" && first != root_->name) return result;
        result.push_back(root_.get());
        if (slash == path.npos) return result;
        path.remove_prefix(slash + 1);
        while (!path.empty()) {
            const auto end = path.find(L'/');
            const auto name = path.substr(0, end);
            std::vector<LegacyXmlNode*> next;
            for (const auto* node : result) {
                auto children = node->Children(name);
                next.insert(next.end(), children.begin(), children.end());
            }
            result = std::move(next);
            if (end == path.npos) break;
            path.remove_prefix(end + 1);
        }
        return result;
    }
    LegacyXmlNode* First(std::wstring_view path) const {
        auto nodes = Nodes(path);
        return nodes.empty() ? nullptr : nodes.front();
    }
private:
    std::wstring text_;
    size_t pos_{};
    size_t nodes_{};
    size_t attributes_{};
    std::unique_ptr<LegacyXmlNode> root_;

    static bool White(wchar_t ch) {
        return ch == L' ' || (ch >= L'\t' && ch <= L'\r');
    }
    static bool Letter(wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'_';
    }
    static bool SameClosingName(std::wstring_view left, std::wstring_view right) {
        // 004C7177 -> 004C6B86(..., false): only closing-tag comparison
        // ignores ASCII case. Node paths and attribute lookup stay exact.
        const auto fold = [](wchar_t ch) {
            return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + L'a' - L'A') : ch;
        };
        return left.size() == right.size() &&
            std::equal(left.begin(), left.end(), right.begin(),
                [&](wchar_t a, wchar_t b) { return fold(a) == fold(b); });
    }
    bool At(std::wstring_view token) const { return text_.compare(pos_, token.size(), token) == 0; }
    void Space() { while (pos_ < text_.size() && White(text_[pos_])) ++pos_; }
    [[noreturn]] void Fail(const char* reason) const {
        size_t line = 1, column = 1;
        for (size_t i = 0; i < std::min(pos_, text_.size()); ++i) {
            if (text_[i] == L'\n') { ++line; column = 1; } else ++column;
        }
        throw std::runtime_error(std::string("invalid Skin.xml: ") + reason +
            " (line " + std::to_string(line) + ", column " + std::to_string(column) + ")");
    }
    void Skip(std::wstring_view end, size_t prefix) {
        const auto close = text_.find(end, pos_ + prefix);
        if (close == text_.npos) Fail("unterminated section");
        pos_ = close + end.size();
    }
    bool Special() {
        if (At(L"<!--")) { Skip(L"-->", 4); return true; }
        if (At(L"<?")) { Skip(L"?>", 2); return true; }
        if (At(L"<![CDATA[")) { Skip(L"]]>", 9); return true; }
        if (At(L"<!")) {
            // Unknown declarations are ignored, never resolved or fetched.
            pos_ += 2;
            wchar_t quote{};
            int brackets{};
            while (pos_ < text_.size()) {
                const auto ch = text_[pos_++];
                if (quote) { if (ch == quote) quote = 0; }
                else if (ch == L'\'' || ch == L'"') quote = ch;
                else if (ch == L'[') ++brackets;
                else if (ch == L']' && brackets) --brackets;
                else if (ch == L'>' && !brackets) return true;
            }
            Fail("unterminated declaration");
        }
        return false;
    }
    std::wstring Name() {
        const auto start = pos_;
        if (pos_ == text_.size() || !Letter(text_[pos_])) Fail("invalid name");
        do { ++pos_; } while (pos_ < text_.size() && (Letter(text_[pos_]) ||
            (text_[pos_] >= L'0' && text_[pos_] <= L'9') || text_[pos_] == L'-' || text_[pos_] == L':'));
        return text_.substr(start, pos_ - start);
    }
    static std::wstring Value(std::wstring_view raw) {
        constexpr std::pair<std::wstring_view, wchar_t> entities[] = {
            {L"&amp;", L'&'}, {L"&lt;", L'<'}, {L"&gt;", L'>'},
            {L"&quot;", L'"'}, {L"&apos;", L'\''}};
        std::wstring result;
        result.reserve(raw.size());
        for (size_t i = 0; i < raw.size();) {
            bool replaced = false;
            if (raw[i] == L'&') for (const auto& [token, ch] : entities)
                if (raw.substr(i, token.size()) == token) {
                    result += ch; i += token.size(); replaced = true; break;
                }
            if (!replaced) result += raw[i++];
        }
        return result;
    }
    std::unique_ptr<LegacyXmlNode> Element(size_t depth) {
        if (depth >= 128 || ++nodes_ > 65536) Fail("element limit exceeded");
        if (!At(L"<")) Fail("expected element");
        ++pos_;
        auto node = std::make_unique<LegacyXmlNode>();
        node->name = Name();
        for (;;) {
            Space();
            if (At(L"/>")) { pos_ += 2; return node; }
            if (At(L">")) { ++pos_; break; }
            if (++attributes_ > 262144 || node->attributes.size() >= 4096)
                Fail("attribute limit exceeded");
            auto name = Name();
            Space();
            if (!At(L"=")) Fail("expected attribute value");
            ++pos_; Space();
            if (pos_ >= text_.size()) Fail("missing attribute value");
            const wchar_t quote = text_[pos_] == L'\'' || text_[pos_] == L'"' ? text_[pos_++] : 0;
            const auto start = pos_;
            if (quote) {
                while (pos_ < text_.size() && text_[pos_] != quote) ++pos_;
                if (pos_ == text_.size()) Fail("unterminated attribute");
            } else {
                while (pos_ < text_.size() && !White(text_[pos_]) && text_[pos_] != L'>' &&
                    text_[pos_] != L'"' && text_[pos_] != L'\'' && !At(L"/>")) ++pos_;
            }
            const auto raw = std::wstring_view(text_).substr(start, pos_ - start);
            // 004C76CB copies unquoted values literally; only quoted values
            // enter the named-entity decoder. Keep numeric/unknown entities.
            auto value = quote ? Value(raw) : std::wstring(raw);
            if (std::none_of(node->attributes.begin(), node->attributes.end(),
                [&](const auto& attribute) { return attribute.first == name; }))
                node->attributes.emplace_back(std::move(name), std::move(value));
            if (pos_ < text_.size() && (quote || text_[pos_] == L'\'' || text_[pos_] == L'"')) ++pos_;
        }
        for (;;) {
            if (pos_ >= text_.size()) Fail("unterminated element");
            if (At(L"</")) {
                pos_ += 2;
                if (!SameClosingName(Name(), node->name)) Fail("mismatched closing element");
                Space();
                if (!At(L">")) Fail("invalid closing element");
                ++pos_; return node;
            }
            if (Special()) continue;
            if (At(L"<")) node->children.push_back(Element(depth + 1));
            else ++pos_; // text nodes are not skin descriptors (004C5E0E/004C5E4B)
        }
    }
};
} // namespace ttplayer::skin
