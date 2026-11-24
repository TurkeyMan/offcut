#pragma once
// Internal, bounded JSON reader shared by the native AI transports.
#include <algorithm>
#include <map>
#include "ai_client.h"
namespace offcut::detail
{
inline void utf8_codepoint(std::string& out, uint32_t cp)
{
    if (cp <= 0x7f)
        out += char(cp);
    else if (cp <= 0x7ff)
    {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 63));
    }
    else if (cp <= 0xffff)
    {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    }
    else
    {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 63));
        out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    }
}
struct json_value
{
    enum type
    {
        scalar,
        string,
        object,
        array
    } kind = scalar;
    std::string text;
    std::map<std::string, json_value> members;
    std::vector<json_value> items;
};
class json_reader
{
public:
    explicit json_reader(std::string_view text) : text_(text) {}
    json_value parse()
    {
        auto out = value(0);
        space();
        if (at_ != text_.size())
            fail();
        return out;
    }

private:
    [[noreturn]] void fail() const { throw std::runtime_error("The provider returned invalid JSON."); }
    void space()
    {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n'))
            ++at_;
    }
    bool take(char c)
    {
        space();
        if (at_ < text_.size() && text_[at_] == c)
        {
            ++at_;
            return true;
        }
        return false;
    }
    uint32_t hex4()
    {
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (at_ == text_.size())
                fail();
            const char c = text_[at_++];
            int n = -1;
            if (c >= '0' && c <= '9')
                n = c - '0';
            else if (c >= 'a' && c <= 'f')
                n = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                n = c - 'A' + 10;
            if (n < 0)
                fail();
            cp = cp * 16 + uint32_t(n);
        }
        return cp;
    }
    std::string quoted()
    {
        if (!take('"'))
            fail();
        std::string out;
        while (at_ < text_.size())
        {
            const unsigned char c = text_[at_++];
            if (c == '"')
                return out;
            if (c < 32)
                fail();
            if (c != '\\')
            {
                out += char(c);
                continue;
            }
            if (at_ == text_.size())
                fail();
            switch (text_[at_++])
            {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u':
                {
                    uint32_t cp = hex4();
                    if (cp >= 0xd800 && cp <= 0xdbff)
                    {
                        if (at_ + 2 > text_.size() || text_[at_++] != '\\' || text_[at_++] != 'u')
                            fail();
                        const auto low = hex4();
                        if (low < 0xdc00 || low > 0xdfff)
                            fail();
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    }
                    else if (cp >= 0xdc00 && cp <= 0xdfff)
                        fail();
                    utf8_codepoint(out, cp);
                    break;
                }
                default:
                    fail();
            }
        }
        fail();
    }
    json_value value(int depth)
    {
        if (depth > 64 || ++nodes_ > 100000)
            fail();
        space();
        if (at_ == text_.size())
            fail();
        json_value out;
        if (text_[at_] == '"')
        {
            out.kind = json_value::string;
            out.text = quoted();
            return out;
        }
        if (take('{'))
        {
            out.kind = json_value::object;
            if (take('}'))
                return out;
            do
            {
                auto key = quoted();
                if (!take(':'))
                    fail();
                out.members.insert_or_assign(std::move(key), value(depth + 1));
            } while (take(','));
            if (!take('}'))
                fail();
            return out;
        }
        if (take('['))
        {
            out.kind = json_value::array;
            if (take(']'))
                return out;
            do
            {
                out.items.push_back(value(depth + 1));
            } while (take(','));
            if (!take(']'))
                fail();
            return out;
        }
        const size_t start = at_;
        for (auto literal : {std::string_view("null"), std::string_view("true"), std::string_view("false")})
            if (text_.substr(at_, literal.size()) == literal)
            {
                at_ += literal.size();
                out.text = std::string(literal);
                return out;
            }
        if (text_[at_] == '-')
            ++at_;
        const auto digits = [&]
        {
            const size_t first = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
                ++at_;
            if (at_ == first)
                fail();
        };
        if (at_ < text_.size() && text_[at_] == '0')
            ++at_;
        else
            digits();
        if (at_ < text_.size() && text_[at_] == '.')
        {
            ++at_;
            digits();
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E'))
        {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-'))
                ++at_;
            digits();
        }
        out.text = std::string(text_.substr(start, at_ - start));
        return out;
    }
    std::string_view text_;
    size_t at_ = 0, nodes_ = 0;
};
inline const json_value* json_at(const json_value& root, std::initializer_list<std::string_view> path)
{
    const auto* node = &root;
    for (auto part : path)
    {
        if (node->kind == json_value::object)
        {
            const auto found = node->members.find(std::string(part));
            if (found == node->members.end())
                return nullptr;
            node = &found->second;
        }
        else if (node->kind == json_value::array)
        {
            size_t index = 0;
            if (part.empty())
                return nullptr;
            for (char c : part)
            {
                if (c < '0' || c > '9' || index > 100000)
                    return nullptr;
                index = index * 10 + size_t(c - '0');
            }
            if (index >= node->items.size())
                return nullptr;
            node = &node->items[index];
        }
        else
            return nullptr;
    }
    return node;
}
inline std::string json_dump(const json_value& node, std::string_view secret)
{
    if (node.kind == json_value::string)
        return ai_json_quote(redact_ai_secret(node.text, secret));
    if (node.kind == json_value::scalar)
        return node.text;
    std::string out = node.kind == json_value::object ? "{" : "[";
    bool first = true;
    if (node.kind == json_value::object)
        for (const auto& [key, value] : node.members)
        {
            if (!first)
                out += ',';
            first = false;
            out += ai_json_quote(redact_ai_secret(key, secret)) + ":" + json_dump(value, secret);
        }
    else
        for (const auto& item : node.items)
        {
            if (!first)
                out += ',';
            first = false;
            out += json_dump(item, secret);
        }
    return out + (node.kind == json_value::object ? "}" : "]");
}

}  // namespace offcut::detail
