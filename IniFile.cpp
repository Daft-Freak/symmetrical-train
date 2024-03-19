#include <iostream> //
#include <fstream>

#include "IniFile.hpp"

IniFile::IniFile(const std::filesystem::path &path)
{
    std::fstream stream(path);
    load(stream);
}

IniFile::IniFile(std::istream &stream)
{
    load(stream);
}

const IniFile::Section *IniFile::getSection(std::string_view name) const
{
    auto it = sections.find(name);
    if(it != sections.end())
        return &it->second;

    return nullptr;
}

std::optional<std::string_view> IniFile::getValue(std::string_view sectionName, std::string_view key) const
{
    auto section = getSection(sectionName);

    if(section)
    {
        auto it = section->find(key);

        if(it != section->end())
            return it->second;
    }

    return {};
}

void IniFile::load(std::istream &stream)
{
    std::string line;

    auto curSection = sections.end();

    auto isComment = [](std::string_view s)
    {
        // ini comment or c++ style comment
        return s[0] == ';' || s.compare(0, 2, "//") == 0;
    };

    auto stripLeft = [](std::string_view s)
    {
        size_t off = 0;
        for(; off < s.length() && std::isspace(s[off]); off++);

        return s.substr(off);
    };

    auto stripRight = [](std::string_view s)
    {
        size_t off = s.length();
        for(; off > 0 && std::isspace(s[off - 1]); off--);

        return s.substr(0, off);
    };

    // get line, skipping leading whitespace
    while(std::getline(stream >> std::ws, line))
    {
        // skip comments
        if(isComment(line))
            continue;

        std::string_view lineView(line);

        if(line[0] == '[')
        {
            // new section
            auto end = line.find_first_of(']'); // TODO: escapes?

            if(end == std::string::npos)
            {
                std::cerr << "Bad section name: " << line << "\n";
                curSection = sections.end();
                continue;
            }

            auto sectionName = lineView.substr(1, end - 1);

            // check the rest of the line
            auto rest = stripLeft(lineView.substr(end + 1));

            if(!rest.empty() && !isComment(rest))
                std::cerr << "Unexpected text after section name \"" << sectionName << "\": " << rest << "\n";

            // get/create section
            curSection = sections.emplace(sectionName, Section{}).first;
        }
        else
        {
            // find delimiter
            auto splitPos = line.find_first_of('='); // TODO: escapes in key?

            // make sure there is one
            if(splitPos == std::string::npos)
            {
                std::cerr << "Bad key/value pair: " << line << "\n";
                continue;
            }

            // split into key/value
            auto key = lineView.substr(0, splitPos);
            auto value = lineView.substr(splitPos + 1);
            std::string_view rest;

            // strip whitespace
            key = stripRight(key);
            value = stripLeft(value);

            // quoted values
            if(!value.empty() && (value[0] == '"' || value[0] == '\''))
            {
                auto quote = value[0];
                size_t end = 1;
                // TODO: escapes?
                for(; end < value.length() && value[end] != quote; end++);

                if(end == value.length())
                {
                    std::cerr << "Bad string value for \"" << key << "\": " << value << "\n";
                    continue;
                }

                rest = stripLeft(value.substr(end + 1));
                value = value.substr(1, end - 1);

                // rest of the line should be empty/comments
                if(!rest.empty() && !isComment(rest))
                    std::cerr << "Unexpected text after string value for \"" << key << "\": " << rest << "\n";
            }
            else if(!value.empty())
            {
                // strip trailing comments for regular, non-quoted pairs
                for(size_t i = 0; i < value.length(); i++)
                {
                    if(isComment(value.substr(i)))
                    {
                        value = value.substr(0, i);
                        break;
                    }
                }

                value = stripRight(value);
            }

            if(curSection == sections.end())
            {
                std::cerr << "Ignoring \"" << key << "\" outside of valid section\n";
                continue;
            }

            // finally add the pair
            // TODO: ignoring duplicates
            auto ret = curSection->second.emplace(key, value);

            if(!ret.second)
                std::cerr << "Ignoring duplicate key \"" << key << "\" in section \"" << curSection->first << "\"\n";
        }
    }
}
