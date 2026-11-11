// SPDX-License-Identifier: GPL-2.0-or-later

#include "we_project.h"

#include "ascii_case.h"
#include "json_document.h"
#include "wallpaper_manifest.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

namespace wallpaperx::we {

namespace {

using Json = nlohmann::ordered_json;

Setting::Type typeOf(std::string_view name) {
    using enum Setting::Type;
    static constexpr std::pair<std::string_view, Setting::Type> kTypes[] = {
        {"color", Color},          {"bool", Bool}, {"slider", Slider}, {"combo", Combo},
        {"textinput", TextInput},  {"text", Text}, {"group", Group},   {"scenetexture", SceneTexture},
    };
    const auto it = std::ranges::find_if(kTypes, [name](const auto& entry) { return equalsIgnoringAsciiCase(entry.first, name); });
    return it == std::end(kTypes) ? Unknown : it->second;
}

// An integer `number`, one above long long's range read as its largest rather than wrapped.
long long wholeNumber(const Json& number) {
    constexpr auto kLargest = std::numeric_limits<long long>::max();
    if (number.is_number_unsigned() && number.get<unsigned long long>() > static_cast<unsigned long long>(kLargest)) {
        return kLargest;
    }
    return number.get<long long>();
}

// "r g b", each a finite number, separated by single spaces.
std::optional<Setting::Rgb> parseRgb(std::string_view text) {
    Setting::Rgb rgb{};
    std::size_t at = 0;
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        if (i > 0) {
            if (at >= text.size() || text[at] != ' ') return std::nullopt;
            ++at;
        }
        const auto [end, error] = std::from_chars(text.data() + at, text.data() + text.size(), rgb[i]);
        if (error != std::errc() || !std::isfinite(rgb[i])) return std::nullopt;
        at = std::size_t(end - text.data());
    }
    return at == text.size() ? std::optional(rgb) : std::nullopt;
}

std::unexpected<std::string> refuse(std::string_view what) {
    return std::unexpected(std::format("project.json {}", what));
}

// Names are not quoted back: they can hold anything, a bidirectional override included.
std::expected<Setting, std::string> parseSetting(const std::string& name, const Json& spec) {
    if (!spec.is_object()) return refuse("has a setting that is not an object.");
    Setting setting;
    setting.name = name;

    if (const auto type = spec.find("type"); type == spec.end()) {
        setting.type = Setting::Type::Label;
    } else if (!type->is_string()) {
        return refuse("has a setting whose type is not text.");
    } else {
        setting.typeName = type->get<std::string>();
        setting.type = typeOf(setting.typeName);
    }
    const auto text = [&spec](const char* key, std::string& into) {
        const auto it = spec.find(key);
        if (it == spec.end()) return true;
        if (!it->is_string()) return false;
        into = it->get<std::string>();
        return true;
    };
    if (!text("text", setting.text)) return refuse("has a setting whose label is not text.");
    if (!text("condition", setting.condition)) return refuse("has a setting whose condition is not text.");
    if (const auto order = spec.find("order"); order != spec.end()) {
        if (!order->is_number_integer()) return refuse("has a setting whose order is not a whole number.");
        setting.order = wholeNumber(*order);
    }

    const auto value = spec.find("value");
    const bool hasValue = value != spec.end();
    using enum Setting::Type;
    switch (setting.type) {
    case Color: {
        const auto rgb = hasValue && value->is_string() ? parseRgb(value->get_ref<const std::string&>()) : std::nullopt;
        if (!rgb) return refuse("has a colour setting whose value is not three numbers.");
        setting.value = *rgb;
        break;
    }
    case Bool:
        if (!hasValue || !value->is_boolean()) return refuse("has a switch setting whose value is not true or false.");
        setting.value = value->get<bool>();
        break;
    case Slider: {
        const auto number = [&spec](const char* key) -> std::optional<double> {
            const auto it = spec.find(key);
            if (it == spec.end() || !it->is_number() || !std::isfinite(it->get<double>())) return std::nullopt;
            return it->get<double>();
        };
        const auto minimum = number("min");
        const auto maximum = number("max");
        const auto current = number("value");
        if (!minimum || !maximum || !current) return refuse("has a slider setting without a numeric value, min and max.");
        setting.minimum = *minimum;
        setting.maximum = *maximum;
        setting.value = *current;
        if (spec.contains("step")) {
            setting.step = number("step");
            if (!setting.step) return refuse("has a slider setting whose step is not a number.");
        }
        if (const auto precision = spec.find("precision"); precision != spec.end()) {
            if (!precision->is_number_integer() || wholeNumber(*precision) < 0 ||
                wholeNumber(*precision) > std::numeric_limits<int>::max()) {
                return refuse("has a slider setting whose precision is not a whole number.");
            }
            setting.precision = int(wholeNumber(*precision));
        }
        if (const auto fraction = spec.find("fraction"); fraction != spec.end()) {
            if (!fraction->is_boolean()) return refuse("has a slider setting whose fraction is not true or false.");
            setting.fraction = fraction->get<bool>();
        }
        break;
    }
    case Combo: {
        if (!hasValue || !value->is_string()) return refuse("has a choice setting whose value is not text.");
        setting.value = value->get<std::string>();
        const auto options = spec.find("options");
        if (options == spec.end() || !options->is_array()) return refuse("has a choice setting with no options.");
        for (const Json& option : *options) {
            const auto label = option.is_object() ? option.find("label") : option.end();
            const auto chosen = option.is_object() ? option.find("value") : option.end();
            if (!option.is_object() || label == option.end() || !label->is_string() || chosen == option.end() ||
                !chosen->is_string()) {
                return refuse("has a choice setting with an option that is not a label and a text value.");
            }
            setting.options.push_back({label->get<std::string>(), chosen->get<std::string>()});
        }
        break;
    }
    case TextInput:
    case SceneTexture:
        if (!hasValue || !value->is_string()) return refuse("has a text setting whose value is not text.");
        setting.value = value->get<std::string>();
        break;
    case Text:
    case Group:
    case Label:
    case Unknown:
        break;
    }
    return setting;
}

// general.properties into `project`, each malformed setting left out with its sentence.
void readSettings(const Json& doc, Project& project) {
    const auto general = doc.find("general");
    if (general == doc.end() || general->is_null()) return;
    if (!general->is_object()) {
        project.settingProblems.push_back("project.json has a \"general\" that is not an object.");
        return;
    }
    const auto properties = general->find("properties");
    if (properties == general->end() || properties->is_null()) return;
    if (!properties->is_object()) {
        project.settingProblems.push_back("project.json has settings that are not an object.");
        return;
    }
    for (const auto& [name, spec] : properties->items()) {
        if (auto setting = parseSetting(name, spec)) {
            project.settings.push_back(std::move(*setting));
        } else {
            project.settingProblems.push_back(std::move(setting.error()));
        }
    }
    std::ranges::stable_sort(project.settings, {}, &Setting::order);
}

} // namespace

std::expected<Project, std::string> parseProject(std::string_view json) {
    auto parsed = parseJsonDocument(json, kProjectName);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    const Json& doc = *parsed;
    if (!doc.is_object()) return refuse("is not a JSON object.");

    Project project;
    const auto type = doc.find("type");
    if (type == doc.end() || !type->is_string() || type->get_ref<const std::string&>().empty()) {
        return refuse("has no type.");
    }
    project.typeName = type->get<std::string>();
    project.kind = equalsIgnoringAsciiCase(project.typeName, "scene")   ? Project::Kind::Scene
                   : equalsIgnoringAsciiCase(project.typeName, "video") ? Project::Kind::Video
                   : equalsIgnoringAsciiCase(project.typeName, "web")   ? Project::Kind::Web
                                                                        : Project::Kind::Other;

    // Required of a video only, so another kind is still told apart by why it cannot be shown.
    if (const auto file = doc.find("file");
        file != doc.end() && file->is_string() && manifest::isPlainRelativePath(file->get_ref<const std::string&>())) {
        project.file = file->get<std::string>();
    } else if (project.kind == Project::Kind::Video) {
        return refuse("does not name its file as a plain path inside the item.");
    }

    if (const auto preview = doc.find("preview");
        preview != doc.end() && !preview->is_null() && !(preview->is_string() && preview->get_ref<const std::string&>().empty())) {
        if (!preview->is_string() || !manifest::isPlainRelativePath(preview->get_ref<const std::string&>())) {
            return refuse("does not name its preview as a plain path inside the item.");
        }
        project.preview = preview->get<std::string>();
    }
    if (const auto title = doc.find("title"); title != doc.end()) {
        if (!title->is_string()) return refuse("has a title that is not text.");
        project.title = title->get<std::string>();
    }
    if (const auto tags = doc.find("tags"); tags != doc.end()) {
        if (!tags->is_array() || !std::ranges::all_of(*tags, &Json::is_string)) {
            return refuse("has tags that are not a list of text.");
        }
        for (const Json& tag : *tags) project.tags.push_back(tag.get<std::string>());
    }
    readSettings(doc, project);
    return project;
}

} // namespace wallpaperx::we
