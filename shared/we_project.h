// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A Wallpaper Engine item's project.json (docs/phase4/spec_project_json.md). Standard library and
// nlohmann only.

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wallpaperx::we {

inline constexpr const char* kProjectName = "project.json";
// Far above any real project.json; a larger one is refused, not read.
inline constexpr std::size_t kMaxProjectBytes = 1 << 20;
// Steam's app id for Wallpaper Engine: its Workshop items are under workshop/content/<this>.
inline constexpr const char* kAppId = "431960";

// One user setting under general.properties.
struct Setting {
    // Label is a heading with no type at all; Unknown is a type this reader does not know.
    enum class Type { Color, Bool, Slider, Combo, TextInput, Text, Group, SceneTexture, Label, Unknown };
    struct Option {
        std::string label;
        std::string value;
        friend bool operator==(const Option&, const Option&) = default;
    };
    using Rgb = std::array<double, 3>;
    // By type: Rgb for Color, bool for Bool, double for Slider, text for Combo (the chosen option's
    // value), TextInput and SceneTexture; nothing for the rest.
    using Value = std::variant<std::monostate, bool, double, std::string, Rgb>;

    // What scenes bind to it by.
    std::string name;
    Type type = Type::Unknown;
    // As written, for Unknown.
    std::string typeName;
    // May hold HTML; never to be shown as rich text.
    std::string text;
    long long order = 0;
    Value value;
    std::string condition;
    // Slider only.
    double minimum = 0.0;
    double maximum = 0.0;
    std::optional<double> step;
    std::optional<int> precision;
    bool fraction = true;
    // Combo only.
    std::vector<Option> options;

    friend bool operator==(const Setting&, const Setting&) = default;
};

struct Project {
    enum class Kind { Scene, Video, Web, Other };

    Kind kind = Kind::Other;
    // As written; compared without case.
    std::string typeName;
    // Plain relative paths, not yet checked to name a file. A scene's file is inside its package.
    // Empty for a kind other than video that names none as a plain path.
    std::string file;
    // Empty when project.json names none, or names it as null or "".
    std::string preview;
    std::string title;
    std::vector<std::string> tags;
    // Ascending by `order`, ties in the file's order; a malformed one is left out.
    std::vector<Setting> settings;
    // A sentence for each setting left out, or for all of them when "general" or its "properties"
    // is malformed.
    std::vector<std::string> settingProblems;
};

// The error is a sentence for the user. Setting types are matched without case. A malformed setting
// never refuses the item.
std::expected<Project, std::string> parseProject(std::string_view json);

} // namespace wallpaperx::we
