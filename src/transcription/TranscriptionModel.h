#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fastrecord::transcription {

enum class Model : std::uint32_t {
    Tiny = 0,
    Base = 1,
    Small = 2,
};

struct ModelPresentation {
    Model model;
    std::wstring_view id;
    std::wstring_view name;
    std::wstring_view quality;
    std::wstring_view diskSize;
};

inline constexpr std::array<ModelPresentation, 3> kModelCatalog{{
    {Model::Tiny, L"tiny", L"Tiny", L"Rápida", L"~75 MiB"},
    {Model::Base, L"base", L"Base", L"Equilibrada", L"~142 MiB"},
    {Model::Small, L"small", L"Small", L"Mais precisa", L"~466 MiB"},
}};

constexpr bool isValidModel(Model model) noexcept {
    return static_cast<std::uint32_t>(model) < kModelCatalog.size();
}

constexpr Model modelFromPersistedValue(std::uint32_t value) noexcept {
    return value < kModelCatalog.size()
        ? static_cast<Model>(value)
        : Model::Base;
}

constexpr const ModelPresentation& modelPresentation(Model model) noexcept {
    for (const auto& presentation : kModelCatalog) {
        if (presentation.model == model) {
            return presentation;
        }
    }
    return kModelCatalog[1];
}

constexpr std::optional<Model> modelFromId(std::wstring_view id) noexcept {
    for (const auto& presentation : kModelCatalog) {
        if (presentation.id == id) {
            return presentation.model;
        }
    }
    return std::nullopt;
}

inline constexpr std::wstring_view kModelMessagePrefix = L"transcription-model:";

constexpr std::optional<Model> modelFromMessage(std::wstring_view message) noexcept {
    if (message.substr(0, kModelMessagePrefix.size()) != kModelMessagePrefix) {
        return std::nullopt;
    }
    return modelFromId(message.substr(kModelMessagePrefix.size()));
}

} // namespace fastrecord::transcription
