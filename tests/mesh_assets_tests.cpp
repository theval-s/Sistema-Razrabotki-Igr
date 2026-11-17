#include <engine/assets/AssetPipeline.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>

#include "gltf_fixture.hpp"

namespace {
using namespace engine;
using DirectX::SimpleMath::Vector3;

class MeshImport : public ::testing::Test {
protected:
    std::filesystem::path directory;
    CpuPayload payload;

    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("sri-mesh-test-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    void Import(const std::string& positions, bool center = false) {
        ASSERT_NO_FATAL_FAILURE(WriteObj(positions + "\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"));
        ASSERT_NO_FATAL_FAILURE(ImportFile(directory / "model.obj", center));
    }

    void WriteObj(const std::string& text) {
        std::ofstream file(directory / "model.obj");
        file << text;
        file.close();
        ASSERT_TRUE(file);
    }

    void ImportFile(const std::filesystem::path& path, bool center = false) {
        AssetDesc desc;
        desc.path = path;
        desc.centerOnLoad = center;
        std::string error;
        ASSERT_TRUE(AssimpMeshImporter{}.ReadCpu(desc, nullptr, payload, error)) << error;
        ASSERT_EQ(payload.kind, CpuPayload::Kind::Mesh);
    }

    void ExpectSectionUvs(std::size_t section, const std::vector<float>& uvs) {
        const auto& range = payload.mesh.sections.at(section);
        ASSERT_EQ(range.vertexCount * 2, uvs.size());
        for (uint32_t i = 0; i < range.vertexCount; ++i) {
            EXPECT_FLOAT_EQ(payload.mesh.vertices[range.firstVertex + i].uv.x, uvs[i * 2]) << "vertex " << i;
            EXPECT_FLOAT_EQ(payload.mesh.vertices[range.firstVertex + i].uv.y, uvs[i * 2 + 1]) << "vertex " << i;
        }
    }

    void ExpectBounds(Vector3 min, Vector3 max) {
        EXPECT_FLOAT_EQ(payload.mesh.bounds.min.x, min.x);
        EXPECT_FLOAT_EQ(payload.mesh.bounds.min.y, min.y);
        EXPECT_FLOAT_EQ(payload.mesh.bounds.min.z, min.z);
        EXPECT_FLOAT_EQ(payload.mesh.bounds.max.x, max.x);
        EXPECT_FLOAT_EQ(payload.mesh.bounds.max.y, max.y);
        EXPECT_FLOAT_EQ(payload.mesh.bounds.max.z, max.z);
        for (const auto& vertex : payload.mesh.vertices) {
            EXPECT_GE(vertex.pos.x, payload.mesh.bounds.min.x);
            EXPECT_GE(vertex.pos.y, payload.mesh.bounds.min.y);
            EXPECT_GE(vertex.pos.z, payload.mesh.bounds.min.z);
            EXPECT_LE(vertex.pos.x, payload.mesh.bounds.max.x);
            EXPECT_LE(vertex.pos.y, payload.mesh.bounds.max.y);
            EXPECT_LE(vertex.pos.z, payload.mesh.bounds.max.z);
        }
    }
};

TEST_F(MeshImport, PositiveCoordinatesDoNotIncludeOrigin) {
    ASSERT_NO_FATAL_FAILURE(Import("v 2 3 4\nv 6 3 4\nv 2 7 12\n"));
    ExpectBounds({2, 3, 4}, {6, 7, 12});
}

TEST_F(MeshImport, NegativeCoordinatesDoNotIncludeOrigin) {
    ASSERT_NO_FATAL_FAILURE(Import("v -8 -7 -9\nv -2 -7 -9\nv -8 -1 -3\n"));
    ExpectBounds({-8, -7, -9}, {-2, -1, -3});
}

TEST_F(MeshImport, CenterOnLoadBoundsMatchNormalizedVertices) {
    ASSERT_NO_FATAL_FAILURE(Import("v 2 3 4\nv 6 3 4\nv 2 7 12\n", true));
    ExpectBounds({-0.25f, -0.25f, -0.5f}, {0.25f, 0.25f, 0.5f});
}

TEST_F(MeshImport, PlanarMeshHasZeroThickness) {
    ASSERT_NO_FATAL_FAILURE(Import("v 2 3 5\nv 6 3 5\nv 2 7 5\n", true));
    ExpectBounds({-0.5f, -0.5f, 0}, {0.5f, 0.5f, 0});
}

TEST_F(MeshImport, BoundsCoverAllImportedMeshes) {
    ASSERT_NO_FATAL_FAILURE(Import(
        "o first\nusemtl first\nv -8 -7 -9\nv -2 -7 -9\nv -8 -1 -3\n"
        "vt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"
        "o second\nusemtl second\nv 2 3 4\nv 6 3 4\nv 2 7 12\nf 4/1 5/2 6/3\n"));
    ExpectBounds({-8, -7, -9}, {6, 7, 12});
}

TEST_F(MeshImport, OneSectionPerUsedMaterialSamplingItsTextureUvSet) {
    test::GltfFixture model;
    model.AddMaterial("Unused");
    const int texture = model.AddImageUri("gold.png");
    model.AddMaterial("Gold", std::format(R"("pbrMetallicRoughness":{{"baseColorTexture":{{"index":{},"texCoord":1}}}})", texture));
    model.AddMaterial("Gem");
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0, 0, 0, 0, 0, 0}, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f}}, 1});
    model.AddPrimitive({{0, 0, 1, 1, 0, 1, 0, 1, 1}, {{0.7f, 0.8f, 0.9f, 0.1f, 0.2f, 0.3f}}, 2});
    model.WriteGltf(directory / "model.gltf");
    ASSERT_NO_FATAL_FAILURE(ImportFile(directory / "model.gltf"));

    EXPECT_EQ(payload.mesh.materialSlots, (std::vector<std::string>{"Gold", "Gem"}));
    ASSERT_EQ(payload.mesh.sections.size(), 2u);
    for (uint32_t slot = 0; slot < 2; ++slot) {
        const auto& section = payload.mesh.sections[slot];
        EXPECT_EQ(section.materialSlot, slot);
        EXPECT_EQ(section.firstVertex, slot * 3);
        EXPECT_EQ(section.firstIndex, slot * 3);
        ASSERT_EQ(section.indexCount, 3u);
        for (uint32_t i = 0; i < section.indexCount; ++i) {
            const auto index = payload.mesh.indices[section.firstIndex + i];
            EXPECT_GE(index, section.firstVertex);
            EXPECT_LT(index, section.firstVertex + section.vertexCount);
        }
    }
    // glTF UVs reach the GPU as written: both share D3D's top-left origin.
    ExpectSectionUvs(0, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f});
    ExpectSectionUvs(1, {0.7f, 0.8f, 0.9f, 0.1f, 0.2f, 0.3f});
}

TEST_F(MeshImport, MissingTextureUvSetFallsBackToTheFirst) {
    test::GltfFixture model;
    const int texture = model.AddImageUri("gold.png");
    model.AddMaterial("Gold", std::format(R"("pbrMetallicRoughness":{{"baseColorTexture":{{"index":{},"texCoord":1}}}})", texture));
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f}}, 0});
    model.WriteGltf(directory / "model.gltf");
    ASSERT_NO_FATAL_FAILURE(ImportFile(directory / "model.gltf"));
    ExpectSectionUvs(0, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f});
}

TEST_F(MeshImport, ModelWithoutUvsLoadsWithZeroUvs) {
    ASSERT_NO_FATAL_FAILURE(WriteObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    ASSERT_NO_FATAL_FAILURE(ImportFile(directory / "model.obj"));
    ASSERT_EQ(payload.mesh.sections.size(), 1u);
    ExpectSectionUvs(0, {0, 0, 0, 0, 0, 0});
}

TEST_F(MeshImport, GlbLoadsLikeGltf) {
    test::GltfFixture model;
    model.AddMaterial("Only");
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f}}, 0});
    model.WriteGlb(directory / "model.glb");
    ASSERT_NO_FATAL_FAILURE(ImportFile(directory / "model.glb"));
    EXPECT_EQ(payload.mesh.materialSlots, (std::vector<std::string>{"Only"}));
    ExpectSectionUvs(0, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f});
}
} // namespace
