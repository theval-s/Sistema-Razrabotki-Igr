#include <engine/project/RuntimeSettings.hpp>

#include <cstdint>
#include <exception>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace engine {
    namespace {

        using Json = nlohmann::json;

        // Reads a dimension from JSON.
        // Missing fields retain their default value.
        bool ReadDimension(
            const Json& data,
            const char* name,
            std::uint32_t& value,
            std::string& error) {

            const auto it = data.find(name);

            if (it == data.end()) {
                return true;
            }

            if (!it->is_number_integer()) {
                error = std::string("Runtime setting '") + name + "' must be a positive integer";
                return false;
            }

            // Handle unsigned JSON integers separately.
            if (it->is_number_unsigned()) {
                const auto number = it->get<std::uint64_t>();

                if (number == 0 || number > std::numeric_limits<std::uint32_t>::max()) {
                    error = std::string("Runtime setting '") + name + "' is outside the valid range";
                    return false;
                }

                value = static_cast<std::uint32_t>(number);
                return true;
            }

            const auto number = it->get<std::int64_t>();

            if (number <= 0 ||
                static_cast<std::uint64_t>(number) >
                std::numeric_limits<std::uint32_t>::max()) {

                error = std::string("Runtime setting '") + name + "' is outside the valid range";
                return false;
            }

            value = static_cast<std::uint32_t>(number);
            return true;
        }

        bool ReadBool(
            const Json& data,
            const char* name,
            bool& value,
            std::string& error) {

            const auto it = data.find(name);

            if (it == data.end()) {
                return true;
            }

            if (!it->is_boolean()) {
                error = std::string("Runtime setting '") + name +
                    "' must be a boolean";
                return false;
            }

            value = it->get<bool>();
            return true;
        }

    }  // namespace

    bool ParseRuntimeSettings(
        std::string_view json,
        RuntimeSettings& value,
        std::string& error) {

        try {
            const Json data = Json::parse(json);

            if (!data.is_object()) {
                error = "Runtime settings must be a JSON object";
                return false;
            }

            // Start with default values.
            RuntimeSettings settings;

            if (const auto it = data.find("title"); it != data.end()) {
                if (!it->is_string()) {
                    error = "Runtime setting 'title' must be a string";
                    return false;
                }

                settings.title = it->get<std::string>();
            }

            if (!ReadDimension(data, "width", settings.width, error) ||
                !ReadDimension(data, "height", settings.height, error) ||
                !ReadBool(data, "fullscreen", settings.fullscreen, error) ||
                !ReadBool(data, "captureMouse", settings.captureMouse, error)) {
                return false;
            }

            value = std::move(settings);
            error.clear();
            return true;

        }
        catch (const std::exception& e) {
            error = std::string("Invalid runtime settings: ") + e.what();
            return false;
        }
    }

    bool SerializeRuntimeSettings(
        const RuntimeSettings& value,
        std::string& json,
        std::string& error) {

        if (value.width == 0 || value.height == 0) {
            error = "Runtime settings width and height must be positive";
            return false;
        }

        try {
            const Json data = {
                { "title", value.title },
                { "width", value.width },
                { "height", value.height },
                { "fullscreen", value.fullscreen },
                { "captureMouse", value.captureMouse }
            };

            json = data.dump(2) + '\n';

            error.clear();
            return true;

        }
        catch (const std::exception& e) {
            error = std::string("Cannot serialize runtime settings: ") + e.what();
            return false;
        }
    }

}  // namespace engine