#pragma once

#include <filesystem>
#include <istream>
#include <map>
#include <optional>

class IniFile final
{
public:
    using Section = std::map<std::string, std::string, std::less<>>;

    IniFile(const std::filesystem::path &path);
    IniFile(std::istream &stream);

    const Section *getSection(std::string_view name) const;

    std::optional<std::string_view> getValue(std::string_view sectionName, std::string_view key) const;

private:
    void load(std::istream &stream);

    std::map<std::string, Section, std::less<>> sections;
};