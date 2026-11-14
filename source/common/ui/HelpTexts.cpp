#include "common/ui/HelpTexts.h"

namespace vs::ui
{
namespace
{
void replaceAll (std::string& s, std::string_view from, std::string_view to)
{
    for (size_t pos = s.find (from); pos != std::string::npos; pos = s.find (from, pos + to.size()))
        s.replace (pos, from.size(), to);
}

std::string twoDigits (int n)
{
    return std::string (1, static_cast<char> ('0' + n / 10)) + static_cast<char> ('0' + n % 10);
}
} // namespace

HelpTexts HelpTexts::parse (std::string_view md)
{
    HelpTexts h;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= md.size())
    {
        const size_t end = md.find ('\n', pos);
        std::string_view line = md.substr (pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? md.size() + 1 : end + 1;
        ++lineNo;
        if (! line.empty() && line.back() == '\r')
            line.remove_suffix (1);

        constexpr std::string_view prefix = "- `";
        if (line.substr (0, prefix.size()) != prefix)
            continue;
        const size_t idEnd = line.find ("`: ", prefix.size());
        if (idEnd == std::string_view::npos)
            continue;
        const std::string id (line.substr (prefix.size(), idEnd - prefix.size()));
        // An id is one token (letters, digits, '.', '_', and the <part> / NN placeholders): a line
        // of prose that merely starts like an entry is not one.
        if (id.empty() || id.find_first_not_of ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._<>") != std::string::npos)
            continue;
        const std::string text (line.substr (idEnd + 3));

        std::vector<std::string> ids;
        constexpr std::string_view partPrefix = "<part>.";
        if (id.rfind (partPrefix, 0) == 0)
        {
            ids.push_back ("lower." + id.substr (partPrefix.size()));
            ids.push_back ("upper." + id.substr (partPrefix.size()));
        }
        else
            ids.push_back (id);

        for (const auto& one : ids)
        {
            if (one.find ("NN") != std::string::npos)
            {
                for (int step = 1; step <= 16; ++step)
                {
                    Entry e { one, text, lineNo };
                    replaceAll (e.id, "NN", twoDigits (step));
                    replaceAll (e.text, "NN", std::to_string (step));
                    h.list.push_back (std::move (e));
                }
            }
            else
                h.list.push_back ({ one, text, lineNo });
        }
    }
    return h;
}

const std::string* HelpTexts::find (std::string_view id) const noexcept
{
    for (const auto& e : list)
        if (e.id == id)
            return &e.text;
    return nullptr;
}

int HelpTexts::countOf (std::string_view id) const noexcept
{
    int n = 0;
    for (const auto& e : list)
        n += e.id == id ? 1 : 0;
    return n;
}

int HelpTexts::sentenceCount (std::string_view t) noexcept
{
    int n = 0;
    for (size_t i = 0; i < t.size(); ++i)
    {
        const char c = t[i];
        if (c != '.' && c != '!' && c != '?')
            continue;
        const bool atEnd = i + 1 == t.size();
        if (atEnd || t[i + 1] == ' ')
            ++n;
    }
    return n;
}

int HelpTexts::characterCount (std::string_view t) noexcept
{
    int n = 0;
    for (const char c : t)
        n += (static_cast<unsigned char> (c) & 0xC0u) != 0x80u ? 1 : 0;
    return n;
}
} // namespace vs::ui
