#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace test {

/// Writes small .gltf/.glb models for import tests. Every primitive is non-indexed triangles.
class GltfFixture {
public:
    struct Primitive {
        std::vector<float> positions;              // xyz per vertex
        std::vector<std::vector<float>> uvSets;    // uv per vertex, TEXCOORD_0, _1, ...
        int material = -1;
    };

    /// `extra` is spliced into the material object, e.g. `"pbrMetallicRoughness":{...}`.
    void AddMaterial(const std::string& name, const std::string& extra = {}) {
        materials_.push_back(std::format(R"({{"name":"{}"{}{}}})", name, extra.empty() ? "" : ",", extra));
    }
    /// An image stored outside the model, by relative URI.
    int AddImageUri(const std::string& uri) {
        images_.push_back(std::format(R"({{"uri":"{}"}})", uri));
        textures_.push_back(std::format(R"({{"source":{}}})", images_.size() - 1));
        return static_cast<int>(textures_.size() - 1);
    }
    /// An image stored in the binary buffer, as glb exporters do.
    int AddImageBytes(const std::string& name, const std::string& mime, const std::string& bytes) {
        const auto view = AddView(bytes);
        images_.push_back(std::format(R"({{"name":"{}","mimeType":"{}","bufferView":{}}})", name, mime, view));
        textures_.push_back(std::format(R"({{"source":{}}})", images_.size() - 1));
        return static_cast<int>(textures_.size() - 1);
    }
    void AddPrimitive(const Primitive& primitive) {
        std::string attributes = std::format(R"("POSITION":{})", AddAccessor(primitive.positions, 3));
        for (std::size_t set = 0; set < primitive.uvSets.size(); ++set) {
            attributes += std::format(R"(,"TEXCOORD_{}":{})", set, AddAccessor(primitive.uvSets[set], 2));
        }
        primitives_.push_back(primitive.material >= 0
            ? std::format(R"({{"attributes":{{{}}},"material":{}}})", attributes, primitive.material)
            : std::format(R"({{"attributes":{{{}}}}})", attributes));
    }

    /// `model.gltf` plus `<binName>` beside it.
    void WriteGltf(const std::filesystem::path& file, const std::string& binName = "model.bin") const {
        WriteBytes(file.parent_path() / binName, bin_);
        WriteBytes(file, Json(std::format(R"("uri":"{}",)", binName)));
    }
    void WriteGlb(const std::filesystem::path& file) const {
        std::string json = Json({});
        json.resize((json.size() + 3) & ~std::size_t{3}, ' ');
        std::string bin = bin_;
        bin.resize((bin.size() + 3) & ~std::size_t{3}, '\0');
        std::string out;
        const auto u32 = [&out](std::uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); };
        out += "glTF";
        u32(2);
        u32(static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
        u32(static_cast<std::uint32_t>(json.size()));
        out += "JSON";
        out += json;
        u32(static_cast<std::uint32_t>(bin.size()));
        out.append("BIN\0", 4);
        out += bin;
        WriteBytes(file, out);
    }

private:
    std::string bin_;
    std::vector<std::string> views_, accessors_, materials_, textures_, images_, primitives_;

    std::size_t AddView(const std::string& bytes) {
        bin_.resize((bin_.size() + 3) & ~std::size_t{3}, '\0');
        views_.push_back(std::format(R"({{"buffer":0,"byteOffset":{},"byteLength":{}}})", bin_.size(), bytes.size()));
        bin_ += bytes;
        return views_.size() - 1;
    }
    std::size_t AddAccessor(const std::vector<float>& values, int components) {
        const auto view = AddView(std::string(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float)));
        std::string bounds;
        if (components == 3) {
            float min[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            float max[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
            for (std::size_t i = 0; i < values.size(); ++i) {
                min[i % 3] = (std::min)(min[i % 3], values[i]);
                max[i % 3] = (std::max)(max[i % 3], values[i]);
            }
            bounds = std::format(R"(,"min":[{},{},{}],"max":[{},{},{}])", min[0], min[1], min[2], max[0], max[1], max[2]);
        }
        accessors_.push_back(std::format(R"({{"bufferView":{},"componentType":5126,"count":{},"type":"{}"{}}})",
                                         view, values.size() / components, components == 3 ? "VEC3" : "VEC2", bounds));
        return accessors_.size() - 1;
    }
    static std::string Join(const std::vector<std::string>& items) {
        std::string out;
        for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
        return out;
    }
    std::string Json(const std::string& bufferUri) const {
        std::string json = std::format(
            R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"mesh":0}}],)"
            R"("meshes":[{{"primitives":[{}]}}],"accessors":[{}],"bufferViews":[{}],"buffers":[{{{}"byteLength":{}}}])",
            Join(primitives_), Join(accessors_), Join(views_), bufferUri, bin_.size());
        if (!materials_.empty()) json += std::format(R"(,"materials":[{}])", Join(materials_));
        if (!textures_.empty()) json += std::format(R"(,"textures":[{}],"images":[{}])", Join(textures_), Join(images_));
        return json + "}";
    }
    static void WriteBytes(const std::filesystem::path& file, const std::string& bytes) {
        std::ofstream out(file, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

} // namespace test
