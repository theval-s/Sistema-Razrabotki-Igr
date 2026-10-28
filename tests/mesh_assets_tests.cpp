#include <engine/assets/AssetPipeline.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

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
        const auto path = directory / "model.obj";
        std::ofstream file(path);
        file << positions << "\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
        file.close();
        ASSERT_TRUE(file);
        AssetDesc desc;
        desc.path = path;
        desc.centerOnLoad = center;
        std::string error;
        ASSERT_TRUE(AssimpMeshImporter{}.ReadCpu(desc, nullptr, payload, error)) << error;
        ASSERT_EQ(payload.kind, CpuPayload::Kind::Mesh);
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
} // namespace
