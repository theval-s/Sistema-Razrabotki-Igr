#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <engine/core/Content.hpp>

namespace engine::content::detail {

struct Mounted {
    std::shared_ptr<const Source> source;
    std::string path; // relative to the mount root
};

/// The mount whose root contains `path`, longest root first; empty source when none does.
[[nodiscard]] Mounted Find(const std::filesystem::path& path);

} // namespace engine::content::detail
