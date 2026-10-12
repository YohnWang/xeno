#pragma once

#include<string>
#include<string_view>

namespace xeno
{

class path
{
    static void remove_basename(std::string &s)
    {
        if(s.size() > 1 && s.back() == '/')
            s.pop_back();

        auto pos = s.find_last_of('/');
        if(pos == std::string::npos || pos == 0)
            s = "/";
        else
            s.resize(pos);
    }

    void append_item(std::string_view item)
    {
        if(item.size() == 0 || item == ".")
        {
            // do nothing
        }
        else if(item == "..")
        {
            remove_basename(npath);
        }
        else
        {
            if(npath.empty() || npath.back() != '/')
                npath.push_back('/');
            npath += item;
        }
    }

    std::string npath;
public:
    path() = delete;
    path(const char *s):path{std::string_view{s}}{}
    path(std::string_view s)
    {
        std::size_t start = 0;
        for(std::size_t i = 0; i < s.size(); ++i)
        {
            if(s[i] == '/')
            {
                append_item(s.substr(start, i - start));
                start = i + 1;
            }
        }
        append_item(s.substr(start));
        if(npath.empty())
            npath.push_back('/');
    }

    path(const path &p) = default;
    path(path &&p) = default;

    path& operator=(const path &p) = default;
    path& operator=(path &&p) = default;

    auto operator<=>(const path &p) const = default;
    bool operator==(const path &p) const = default;

    operator std::string() const
    {
        return npath;
    }

    operator std::string_view() const
    {
        return npath;
    }
};

}