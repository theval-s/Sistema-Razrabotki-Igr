#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/FileSystem.hpp>
#include "animation/AnimationData.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace engine;
using Json = nlohmann::json;
using DirectX::SimpleMath::Matrix;
using DirectX::SimpleMath::Vector3;

void ExpectVectorNear(const Vector3& actual, const Vector3& expected) {
    EXPECT_NEAR(actual.x, expected.x, 4e-4f);
    EXPECT_NEAR(actual.y, expected.y, 4e-4f);
    EXPECT_NEAR(actual.z, expected.z, 4e-4f);
}

void ExpectLinearNear(const Matrix& actual, const Matrix& expected) {
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(actual.m[r][c], expected.m[r][c], 4e-4f) << "linear [" << r << "][" << c << "]";
}

std::vector<Vector3> DefaultPositions(const SkinnedMeshCpu& mesh) {
    std::vector<Vector3> result;
    for (const auto& section : mesh.sections) for (const auto& vertex : section.vertices) {
        const Vector3 position(vertex.position.x, vertex.position.y, vertex.position.z);
        Vector3 skinned{};
        for (std::size_t i = 0; i < vertex.weights.size(); ++i)
            if (vertex.weights[i] > 0)
                skinned += Vector3::Transform(position, section.binding->restPalette[vertex.joints[i]]) * vertex.weights[i];
        result.push_back(skinned);
    }
    return result;
}

class AnimationImportRegression : public ::testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-import-regression-" + NewUuid());
    std::filesystem::path bot = root / "models/TestBot";
    std::filesystem::path previousRoot = AssetPathResolver::GetGameRoot();
    AssetCatalog catalog;
    std::string error;

    void SetUp() override {
        ASSERT_TRUE(std::filesystem::create_directories(bot));
        std::filesystem::copy(std::filesystem::path(SRI_SOURCE_ROOT) / "example_game/models/TestBot", bot,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        AssetPathResolver::SetGameRoot(root);
        ASSERT_TRUE(catalog.Scan(root, {}, error)) << error;
    }
    void TearDown() override {
        AssetPathResolver::SetGameRoot(previousRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    bool Import(AssetKind kind, const char* file, CpuPayload& payload) {
        AssetDesc desc;
        desc.path = bot / file;
        desc.catalog = &catalog;
        return SkeletalAssetImporter(kind).ReadCpu(desc, nullptr, payload, error);
    }
    template <class Edit>
    void EditGlb(Edit edit) {
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(ReadBinaryFile(bot / "model.glb", bytes, error)) << error;
        ASSERT_GE(bytes.size(), 20u);
        const auto length = std::uint32_t(bytes[12]) | (std::uint32_t(bytes[13]) << 8) |
            (std::uint32_t(bytes[14]) << 16) | (std::uint32_t(bytes[15]) << 24);
        ASSERT_LE(20u + length, bytes.size());
        auto json = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + length);
        edit(json);
        auto text = json.dump();
        while (text.size() % 4) text += ' ';
        std::vector<std::uint8_t> output(bytes.begin(), bytes.begin() + 20);
        output.insert(output.end(), text.begin(), text.end());
        output.insert(output.end(), bytes.begin() + 20 + length, bytes.end());
        const auto put = [&](std::size_t offset, std::uint32_t value) {
            for (int i = 0; i < 4; ++i) output[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
        };
        put(8, static_cast<std::uint32_t>(output.size()));
        put(12, static_cast<std::uint32_t>(text.size()));
        std::ofstream file(bot / "model.glb", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(output.size()));
        file.close();
        ASSERT_TRUE(file);
    }
    void Models(const SkeletonAsset& asset, const AnimationClipAsset* clip, float ratio, std::vector<Matrix>& result) {
        const auto& skeleton = asset.data->skeleton;
        std::vector<ozz::math::SoaTransform> locals(static_cast<std::size_t>(skeleton.num_soa_joints()));
        if (clip) {
            ozz::animation::SamplingJob::Context context(skeleton.num_joints());
            ozz::animation::SamplingJob sample;
            sample.animation = &clip->data->animation;
            sample.context = &context;
            sample.ratio = ratio;
            sample.output = ozz::make_span(locals);
            ASSERT_TRUE(sample.Run());
        } else {
            const auto rest = skeleton.joint_rest_poses();
            for (std::size_t i = 0; i < locals.size(); ++i) locals[i] = rest[i];
        }
        std::vector<ozz::math::Float4x4> models(static_cast<std::size_t>(skeleton.num_joints()));
        ozz::animation::LocalToModelJob job;
        job.skeleton = &skeleton;
        job.input = ozz::make_span(locals);
        job.output = ozz::make_span(models);
        ASSERT_TRUE(job.Run());
        result.clear();
        for (const auto& model : models) result.push_back(detail::ToEngineMatrix(model));
    }
};

TEST_F(AnimationImportRegression, SkinIgnoresMeshNodeTranslation) {
    CpuPayload original, edited;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", original)) << error;
    ASSERT_NO_FATAL_FAILURE(EditGlb([](Json& json) {
        bool found = false;
        for (auto& node : json.at("nodes")) if (node.value("name", "") == "RobotMesh") {
            ASSERT_EQ(node.at("translation")[0], .25);
            node["translation"][0] = .5;
            found = true;
        }
        ASSERT_TRUE(found);
    }));
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", edited)) << error;
    EXPECT_NE(original.skinnedMesh.skeletonSignature, edited.skinnedMesh.skeletonSignature);
    const auto before = DefaultPositions(original.skinnedMesh), after = DefaultPositions(edited.skinnedMesh);
    ASSERT_EQ(before.size(), after.size());
    ASSERT_FALSE(before.empty());
    for (std::size_t i = 0; i < before.size(); ++i) ExpectVectorNear(after[i], before[i]);
    ExpectVectorNear(edited.skinnedMesh.bounds.min, original.skinnedMesh.bounds.min);
    ExpectVectorNear(edited.skinnedMesh.bounds.max, original.skinnedMesh.bounds.max);
}

TEST_F(AnimationImportRegression, DefaultJointPoseMayDifferFromBindAndDeformsDefaultGeometry) {
    CpuPayload original, edited;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", original)) << error;
    ASSERT_NO_FATAL_FAILURE(EditGlb([](Json& json) {
        bool found = false;
        for (auto& node : json.at("nodes")) if (node.value("name", "") == "Torso") {
            node["translation"] = {.3, 0, 0};
            found = true;
        }
        ASSERT_TRUE(found);
        // Inverse binds and mesh node are deliberately untouched.
    }));
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", edited)) << error;
    const auto before = DefaultPositions(original.skinnedMesh), after = DefaultPositions(edited.skinnedMesh);
    ASSERT_EQ(before.size(), after.size());
    ASSERT_FALSE(before.empty());
    // Root rotates local +X onto asset +Y. All fixture joints descend from Torso.
    const Vector3 shift(0, .3f, 0);
    for (std::size_t i = 0; i < before.size(); ++i) ExpectVectorNear(after[i], before[i] + shift);
    ExpectVectorNear(edited.skinnedMesh.bounds.min, original.skinnedMesh.bounds.min + shift);
    ExpectVectorNear(edited.skinnedMesh.bounds.max, original.skinnedMesh.bounds.max + shift);
}

TEST_F(AnimationImportRegression, RigidGeometryStillUsesMeshNodeTransform) {
    ASSERT_NO_FATAL_FAILURE(EditGlb([](Json& json) {
        for (auto& node : json.at("nodes")) if (node.value("name", "") == "RobotMesh") node.erase("skin");
        for (auto& mesh : json.at("meshes")) for (auto& primitive : mesh.at("primitives")) {
            primitive.at("attributes").erase("JOINTS_0");
            primitive.at("attributes").erase("WEIGHTS_0");
        }
    }));
    CpuPayload original, edited;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", original)) << error;
    ASSERT_NO_FATAL_FAILURE(EditGlb([](Json& json) {
        for (auto& node : json.at("nodes")) if (node.value("name", "") == "RobotMesh") node["translation"][0] = .5;
    }));
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", edited)) << error;
    const auto before = DefaultPositions(original.skinnedMesh), after = DefaultPositions(edited.skinnedMesh);
    ASSERT_EQ(before.size(), after.size());
    ASSERT_FALSE(before.empty());
    for (std::size_t i = 0; i < before.size(); ++i) ExpectVectorNear(after[i], before[i] + Vector3(0, .25f, 0));
}

class ReflectedPartialClipRegression : public AnimationImportRegression, public ::testing::WithParamInterface<bool> {};

TEST_P(ReflectedPartialClipRegression, TranslationOnlySamplingPreservesAuthoredReflectedLinearPart) {
    const bool unnamed = GetParam();
    ASSERT_NO_FATAL_FAILURE(EditGlb([unnamed](Json& json) {
        std::size_t torso = json.at("nodes").size();
        for (std::size_t i = 0; i < json.at("nodes").size(); ++i) {
            auto& node = json.at("nodes")[i];
            if (node.value("name", "") == "Torso") {
                torso = i;
                node["scale"] = {-1, 1, 1};
                node.erase("rotation"); // Implicit identity must remain identity.
            }
            if (unnamed) node.erase("name");
        }
        ASSERT_LT(torso, json.at("nodes").size());
        for (auto& animation : json.at("animations")) if (animation.at("name") == "idle") {
            Json channels = Json::array();
            for (const auto& channel : animation.at("channels"))
                if (channel.at("target").at("node") == torso && channel.at("target").at("path") == "translation")
                    channels.push_back(channel);
            ASSERT_EQ(channels.size(), 1u);
            animation["channels"] = std::move(channels);
        }
    }));
    CpuPayload skeleton, clip, mesh;
    ASSERT_TRUE(Import(AssetKind::Skeleton, "model.skeleton.json", skeleton)) << error;
    ASSERT_TRUE(Import(AssetKind::AnimationClip, "idle.animation.json", clip)) << error;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", mesh)) << error;
    EXPECT_EQ(skeleton.skeleton.signature, clip.animationClip.skeletonSignature);
    EXPECT_EQ(skeleton.skeleton.signature, mesh.skinnedMesh.skeletonSignature);
    std::vector<Matrix> rest;
    ASSERT_NO_FATAL_FAILURE(Models(skeleton.skeleton, nullptr, 0, rest));
    bool reflected = false;
    for (const auto& model : rest) reflected |= model.Determinant() < 0;
    ASSERT_TRUE(reflected);
    bool translated = false;
    for (float ratio : {0.f, .25f, .5f, .75f, 1.f}) {
        SCOPED_TRACE(ratio);
        std::vector<Matrix> sampled;
        ASSERT_NO_FATAL_FAILURE(Models(skeleton.skeleton, &clip.animationClip, ratio, sampled));
        ASSERT_EQ(sampled.size(), rest.size());
        for (std::size_t i = 0; i < rest.size(); ++i) {
            ExpectLinearNear(sampled[i], rest[i]);
            translated |= std::abs(sampled[i]._41 - rest[i]._41) > .01f;
        }
    }
    EXPECT_TRUE(translated); // Exercise a changing translation, not a constant/rest-only clip.
}
INSTANTIATE_TEST_SUITE_P(NamedAndGeneratedNodeNames, ReflectedPartialClipRegression, ::testing::Bool());

TEST_F(AnimationImportRegression, SourceAndDescriptorSuffixesAreCaseInsensitive) {
    const auto rename = [&](const char* from, const char* to) {
        for (const auto& suffix : {std::string{}, std::string(".meta")}) {
            const auto temporary = bot / (std::string(from) + suffix + ".case-temp");
            std::filesystem::rename(bot / (std::string(from) + suffix), temporary);
            std::filesystem::rename(temporary, bot / (std::string(to) + suffix));
        }
    };
    rename("model.glb", "MODEL.GLB");
    rename("model.skeleton.json", "MODEL.SKELETON.JSON");
    rename("model.skinned-mesh.json", "MODEL.SKINNED-MESH.JSON");
    rename("idle.animation.json", "IDLE.ANIMATION.JSON");
    ASSERT_TRUE(catalog.Scan(root, {}, error)) << error;
    CpuPayload skeleton, clip, mesh;
    ASSERT_TRUE(Import(AssetKind::Skeleton, "MODEL.SKELETON.JSON", skeleton)) << error;
    ASSERT_TRUE(Import(AssetKind::AnimationClip, "IDLE.ANIMATION.JSON", clip)) << error;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "MODEL.SKINNED-MESH.JSON", mesh)) << error;
    EXPECT_EQ(skeleton.skeleton.signature, clip.animationClip.skeletonSignature);
    EXPECT_EQ(skeleton.skeleton.signature, mesh.skinnedMesh.skeletonSignature);
}
} // namespace
