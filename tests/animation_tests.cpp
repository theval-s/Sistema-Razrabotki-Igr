#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include "animation/AnimationData.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/span.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace engine;
using DirectX::SimpleMath::Matrix;
using DirectX::SimpleMath::Vector3;
using Json = nlohmann::json;
constexpr const char* SkeletonPath = "game/models/TestBot/model.skeleton.json";
constexpr const char* IdlePath = "game/models/TestBot/idle.animation.json";
constexpr const char* WalkPath = "game/models/TestBot/walk.animation.json";

struct GameRootScope {
    std::filesystem::path previous = AssetPathResolver::GetGameRoot();
    explicit GameRootScope(const std::filesystem::path& root) { AssetPathResolver::SetGameRoot(root); }
    ~GameRootScope() { AssetPathResolver::SetGameRoot(previous); }
};

void ExpectMatrixNear(const Matrix& actual, const Matrix& expected, float tolerance = 3e-4f) {
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            SCOPED_TRACE(::testing::Message() << "matrix [" << row << "][" << column << "]");
            EXPECT_TRUE(std::isfinite(actual.m[row][column]));
            EXPECT_NEAR(actual.m[row][column], expected.m[row][column], tolerance);
        }
    }
}

bool PosesDiffer(const AnimationPose& a, const AnimationPose& b) {
    if (a.jointModels.size() != b.jointModels.size()) return true;
    for (std::size_t i = 0; i < a.jointModels.size(); ++i)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::abs(a.jointModels[i].m[r][c] - b.jointModels[i].m[r][c]) > 1e-4f) return true;
    return false;
}

void ExpectPoseNear(const AnimationPose& actual, const AnimationPose& expected) {
    EXPECT_EQ(actual.skeletonSignature, expected.skeletonSignature);
    ASSERT_EQ(actual.jointModels.size(), expected.jointModels.size());
    for (std::size_t i = 0; i < actual.jointModels.size(); ++i)
        ExpectMatrixNear(actual.jointModels[i], expected.jointModels[i]);
}

class AnimationImport : public ::testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-animation-" + NewUuid());
    std::filesystem::path bot = root / "models/TestBot";
    GameRootScope gameRoot{root};
    AssetCatalog catalog;
    std::string error;

    void SetUp() override {
        ASSERT_TRUE(std::filesystem::create_directories(bot));
        const auto fixture = std::filesystem::path(SRI_SOURCE_ROOT) / "example_game/models/TestBot";
        // Only this small fixture is copied; corrupt-input tests edit disposable files.
        std::filesystem::copy(fixture, bot, std::filesystem::copy_options::recursive |
                                          std::filesystem::copy_options::overwrite_existing);
        ASSERT_TRUE(catalog.Scan(root, {}, error)) << error;
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    bool Import(AssetKind kind, const char* name, CpuPayload& payload) {
        AssetDesc desc;
        desc.path = bot / name;
        desc.catalog = &catalog;
        return SkeletalAssetImporter{kind}.ReadCpu(desc, nullptr, payload, error);
    }

    Json ReadJson(const char* name) {
        std::ifstream file(bot / name);
        return Json::parse(file);
    }

    void WriteJson(const char* name, const Json& json) {
        std::ofstream file(bot / name);
        file << json.dump();
        file.close();
        ASSERT_TRUE(file);
    }

    // Rewrite just the embedded JSON chunk; preserve binary accessors, buffer,
    // chunk alignment, and lengths so rejection exercises interpolation validation.
    template <class Edit>
    void EditGlb(Edit edit) {
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(ReadBinaryFile(bot / "model.glb", bytes, error)) << error;
        const auto word = [&](std::size_t offset) {
            return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
                (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
        };
        ASSERT_GE(bytes.size(), 20u);
        const auto oldLength = word(12);
        ASSERT_LE(20u + oldLength, bytes.size());
        auto json = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + oldLength);
        edit(json);
        auto text = json.dump();
        while (text.size() % 4) text += ' ';
        std::vector<std::uint8_t> output(bytes.begin(), bytes.begin() + 20);
        output.insert(output.end(), text.begin(), text.end());
        output.insert(output.end(), bytes.begin() + 20 + oldLength, bytes.end());
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
};

TEST_F(AnimationImport, CatalogAndIndependentImportsAgreeOnSourceSignature) {
    struct Entry { const char* name; CatalogAssetKind kind; };
    for (const auto& entry : {Entry{"model.skeleton.json", CatalogAssetKind::Skeleton},
                             Entry{"idle.animation.json", CatalogAssetKind::AnimationClip},
                             Entry{"walk.animation.json", CatalogAssetKind::AnimationClip},
                             Entry{"model.skinned-mesh.json", CatalogAssetKind::SkinnedMesh}}) {
        const auto* found = catalog.FindPath(std::string("game/models/TestBot/") + entry.name);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->metadata.kind, entry.kind);
        EXPECT_EQ(CatalogKindForFile(bot / entry.name), entry.kind);
        EXPECT_EQ(catalog.Find(found->metadata.id, entry.kind), found);
    }
    CpuPayload skeleton, repeated, idle, walk, mesh;
    ASSERT_TRUE(Import(AssetKind::Skeleton, "model.skeleton.json", skeleton)) << error;
    ASSERT_TRUE(Import(AssetKind::Skeleton, "model.skeleton.json", repeated)) << error;
    ASSERT_TRUE(Import(AssetKind::AnimationClip, "idle.animation.json", idle)) << error;
    ASSERT_TRUE(Import(AssetKind::AnimationClip, "walk.animation.json", walk)) << error;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", mesh)) << error;
    EXPECT_EQ(skeleton.kind, CpuPayload::Kind::Skeleton);
    EXPECT_EQ(idle.kind, CpuPayload::Kind::AnimationClip);
    EXPECT_EQ(mesh.kind, CpuPayload::Kind::SkinnedMesh);
    ASSERT_NE(skeleton.skeleton.data, nullptr);
    ASSERT_NE(idle.animationClip.data, nullptr);
    ASSERT_NE(walk.animationClip.data, nullptr);
    EXPECT_NE(skeleton.skeleton.signature, 0u);
    EXPECT_EQ(repeated.skeleton.signature, skeleton.skeleton.signature);
    EXPECT_NE(repeated.skeleton.data, skeleton.skeleton.data);
    EXPECT_EQ(idle.animationClip.skeletonSignature, skeleton.skeleton.signature);
    EXPECT_EQ(walk.animationClip.skeletonSignature, skeleton.skeleton.signature);
    EXPECT_EQ(mesh.skinnedMesh.skeletonSignature, skeleton.skeleton.signature);
    EXPECT_EQ(skeleton.skeleton.jointCount, skeleton.skeleton.data->skeleton.num_joints());
    EXPECT_EQ(idle.animationClip.data->animation.num_tracks(), skeleton.skeleton.jointCount);
    EXPECT_EQ(walk.animationClip.data->animation.num_tracks(), skeleton.skeleton.jointCount);
    EXPECT_FLOAT_EQ(idle.animationClip.duration, 1.f);
    EXPECT_FLOAT_EQ(walk.animationClip.duration, 1.f);
}

TEST_F(AnimationImport, RestSkinRetainsSourceRootAndMeshBindTransform) {
    CpuPayload skeleton, mesh;
    ASSERT_TRUE(Import(AssetKind::Skeleton, "model.skeleton.json", skeleton)) << error;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", mesh)) << error;
    const auto& rig = skeleton.skeleton.data->skeleton;
    std::vector<ozz::math::Float4x4> models(static_cast<std::size_t>(rig.num_joints()));
    ozz::animation::LocalToModelJob job;
    job.skeleton = &rig;
    job.input = rig.joint_rest_poses();
    job.output = ozz::make_span(models);
    ASSERT_TRUE(job.Run());
    // Known fixture values, independent of the importer's own restPalette math.
    const Matrix rootModel = Matrix::CreateRotationZ(DirectX::XM_PIDIV2) * Matrix::CreateTranslation(1, 2, 3);
    const Matrix meshModel = Matrix::CreateTranslation(.25f, 0, 0) * rootModel;
    ASSERT_FALSE(models.empty());
    ExpectMatrixNear(detail::ToEngineMatrix(models.front()), rootModel);
    ASSERT_EQ(mesh.skinnedMesh.sections.size(), 2u);
    bool hasBlendedVertex = false;
    Vector3 minimum{}, maximum{};
    bool first = true;
    for (const auto& section : mesh.skinnedMesh.sections) {
        ASSERT_NE(section.binding, nullptr);
        const auto& binding = *section.binding;
        ASSERT_FALSE(binding.joints.empty());
        ASSERT_EQ(binding.joints.size(), binding.inverseBind.size());
        ASSERT_EQ(binding.joints.size(), binding.restPalette.size());
        ASSERT_EQ(binding.joints.size(), binding.jointBounds.size());
        EXPECT_LE(binding.joints.size(), MaxSkinningJoints);
        for (std::size_t i = 0; i < binding.joints.size(); ++i) {
            ASSERT_LT(binding.joints[i], models.size());
            ExpectMatrixNear(binding.inverseBind[i] * detail::ToEngineMatrix(models[binding.joints[i]]), meshModel);
            ExpectMatrixNear(binding.restPalette[i], meshModel);
        }
        ASSERT_FALSE(section.vertices.empty());
        ASSERT_FALSE(section.indices.empty());
        EXPECT_EQ(section.indices.size() % 3, 0u);
        for (auto index : section.indices) EXPECT_LT(index, section.vertices.size());
        for (const auto& vertex : section.vertices) {
            EXPECT_FLOAT_EQ(vertex.position.w, 1.f);
            EXPECT_TRUE(std::isfinite(vertex.normal.x) && std::isfinite(vertex.normal.y) && std::isfinite(vertex.normal.z));
            EXPECT_NEAR(vertex.normal.Length(), 1.f, 1e-4f);
            EXPECT_TRUE(std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y));
            float sum = 0;
            int influences = 0;
            Vector3 rest{};
            const Vector3 position{vertex.position.x, vertex.position.y, vertex.position.z};
            for (std::size_t i = 0; i < vertex.weights.size(); ++i) {
                EXPECT_TRUE(std::isfinite(vertex.weights[i]));
                EXPECT_GE(vertex.weights[i], 0.f);
                sum += vertex.weights[i];
                if (vertex.weights[i] <= 0) continue;
                ++influences;
                ASSERT_LT(vertex.joints[i], binding.joints.size());
                rest += Vector3::Transform(position, binding.restPalette[vertex.joints[i]]) * vertex.weights[i];
            }
            EXPECT_NEAR(sum, 1.f, 1e-6f);
            hasBlendedVertex |= influences > 1;
            const auto expected = Vector3::Transform(position, meshModel);
            EXPECT_NEAR(rest.x, expected.x, 3e-4f);
            EXPECT_NEAR(rest.y, expected.y, 3e-4f);
            EXPECT_NEAR(rest.z, expected.z, 3e-4f);
            if (first) { minimum = maximum = rest; first = false; }
            else { minimum = Vector3::Min(minimum, rest); maximum = Vector3::Max(maximum, rest); }
        }
    }
    EXPECT_TRUE(hasBlendedVertex);
    EXPECT_NEAR(mesh.skinnedMesh.bounds.min.x, minimum.x, 3e-4f);
    EXPECT_NEAR(mesh.skinnedMesh.bounds.min.y, minimum.y, 3e-4f);
    EXPECT_NEAR(mesh.skinnedMesh.bounds.min.z, minimum.z, 3e-4f);
    EXPECT_NEAR(mesh.skinnedMesh.bounds.max.x, maximum.x, 3e-4f);
    EXPECT_NEAR(mesh.skinnedMesh.bounds.max.y, maximum.y, 3e-4f);
    EXPECT_NEAR(mesh.skinnedMesh.bounds.max.z, maximum.z, 3e-4f);
}

TEST_F(AnimationImport, MalformedDescriptorsFailWithoutReplacingPayload) {
    const auto original = ReadJson("idle.animation.json");
    auto wrongVersion = original; wrongVersion["version"] = 2;
    auto missingClip = original; missingClip.erase("clip");
    auto missingAnimation = original; missingAnimation["clip"] = "does-not-exist";
    auto wrongKind = original;
    const auto* source = catalog.FindPath("game/models/TestBot/model.glb");
    ASSERT_NE(source, nullptr);
    wrongKind["skeleton"]["id"] = source->metadata.id.ToString();
    for (const auto& invalid : {wrongVersion, missingClip, missingAnimation, wrongKind}) {
        SCOPED_TRACE(invalid.dump());
        ASSERT_NO_FATAL_FAILURE(WriteJson("idle.animation.json", invalid));
        CpuPayload payload;
        payload.kind = CpuPayload::Kind::Skeleton;
        payload.skeleton.signature = 123;
        EXPECT_FALSE(Import(AssetKind::AnimationClip, "idle.animation.json", payload));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(payload.kind, CpuPayload::Kind::Skeleton);
        EXPECT_EQ(payload.skeleton.signature, 123u);
    }
    std::ofstream(bot / "idle.animation.json") << "{\"version\":1,\"version\":1}";
    CpuPayload payload;
    EXPECT_FALSE(Import(AssetKind::AnimationClip, "idle.animation.json", payload));
    EXPECT_NE(error.find("duplicate"), std::string::npos) << error;
}

TEST_F(AnimationImport, UnsupportedInterpolationIsRejectedInOtherwiseValidGlb) {
    // STEP has the same accessor layout as LINEAR, unlike CUBICSPLINE, so this
    // remains a valid GLB and cannot fail merely because key counts are wrong.
    ASSERT_NO_FATAL_FAILURE(EditGlb([](Json& json) {
        for (auto& animation : json.at("animations"))
            if (animation.at("name") == "idle")
                for (auto& sampler : animation.at("samplers")) sampler["interpolation"] = "STEP";
    }));
    CpuPayload payload;
    EXPECT_FALSE(Import(AssetKind::AnimationClip, "idle.animation.json", payload));
    EXPECT_NE(error.find("interpolation"), std::string::npos) << error;
}

class AnimationRuntime : public AnimationImport {
protected:
    Engine host{EngineConfig{.taskThreads = 1}};
    AnimationSubsystem* animation = host.AddSubsystem<AnimationSubsystem>();
    AssetReference skeletonRef, idleRef, walkRef;

    void SetUp() override {
        AnimationImport::SetUp();
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_TRUE(host.Initialize());
        auto& assets = host.GetSceneAssets();
        ASSERT_TRUE(assets.LoadSkeleton(SkeletonPath));
        ASSERT_TRUE(assets.LoadAnimationClip(IdlePath));
        ASSERT_TRUE(assets.LoadAnimationClip(WalkPath));
        const auto reference = [&](const char* path) {
            const auto* entry = assets.GetCatalog().FindPath(path);
            return entry ? AssetReference{entry->metadata.id, entry->path} : AssetReference{};
        };
        skeletonRef = reference(SkeletonPath);
        idleRef = reference(IdlePath);
        walkRef = reference(WalkPath);
        ASSERT_TRUE(skeletonRef.id.IsValid());
        ASSERT_TRUE(idleRef.id.IsValid());
        ASSERT_TRUE(walkRef.id.IsValid());
    }

    void TearDown() override {
        host.Shutdown();
        AnimationImport::TearDown();
    }

    entt::entity AddBot(const char* name, bool loop = true) {
        auto& scene = host.GetScene();
        const auto entity = scene.CreateEntity(name);
        scene.AddComponent<Animator>(entity, Animator{skeletonRef, idleRef, 1.f, loop, true});
        return entity;
    }

    void Step(double seconds) { animation->LateUpdate(FrameContext{.deltaSeconds = seconds}); }
};

TEST_F(AnimationRuntime, EntitiesHaveIndependentClocksPauseSeekAndImmutablePoseSnapshots) {
    const auto a = AddBot("first"), b = AddBot("second");
    Step(0);
    ASSERT_TRUE(animation->GetPose(a));
    const auto snapshot = animation->GetPose(a);
    const AnimationPose saved = *snapshot;
    ASSERT_TRUE(animation->SetPaused(b, true));
    Step(.25);
    ASSERT_TRUE(animation->GetPlaybackState(a));
    ASSERT_TRUE(animation->GetPlaybackState(b));
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, .25);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(b)->timeSeconds, 0);
    EXPECT_TRUE(PosesDiffer(*animation->GetPose(a), *animation->GetPose(b)));
    EXPECT_NE(animation->GetPose(a), snapshot);
    ExpectPoseNear(*snapshot, saved);
    ASSERT_TRUE(animation->Seek(a, .5, error)) << error;
    Step(0);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, .5);
    // Torso bob is local +Y, which the fixture root rotates onto world -X.
    const auto rig = host.GetSceneAssets().LoadSkeleton(SkeletonPath);
    ASSERT_TRUE(rig);
    int torso = -1;
    for (int i = 0; i < rig->data->skeleton.num_joints(); ++i)
        if (std::string(rig->data->skeleton.joint_names()[i]) == "Torso") torso = i;
    ASSERT_GE(torso, 0);
    EXPECT_NEAR(animation->GetPose(a)->jointModels[torso]._41, .975f, 3e-4f);
    EXPECT_NEAR(animation->GetPose(a)->jointModels[torso]._42, 2.f, 3e-4f);
    ASSERT_TRUE(animation->SetSpeed(a, 2.f, error)) << error;
    ASSERT_TRUE(animation->SetPaused(b, false));
    Step(.125);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, .75);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(b)->timeSeconds, .125);
    ExpectPoseNear(*snapshot, saved);
}

TEST_F(AnimationRuntime, LoopWrapsWhileNonLoopingClipClampsAndStops) {
    const auto looping = AddBot("looping"), once = AddBot("once", false);
    Step(0);
    ASSERT_TRUE(animation->Seek(looping, 2.75, error)) << error;
    ASSERT_TRUE(animation->Seek(once, .75, error)) << error;
    Step(.5);
    ASSERT_TRUE(animation->GetPlaybackState(looping));
    ASSERT_TRUE(animation->GetPlaybackState(once));
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(looping)->timeSeconds, .25);
    EXPECT_TRUE(animation->GetPlaybackState(looping)->playing);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(once)->timeSeconds, 1.0);
    EXPECT_FALSE(animation->GetPlaybackState(once)->playing);
    const auto end = animation->GetPose(once);
    Step(5);
    ExpectPoseNear(*animation->GetPose(once), *end);
    EXPECT_FALSE(animation->Seek(looping, -1, error));
    EXPECT_FALSE(animation->Seek(looping, std::numeric_limits<double>::quiet_NaN(), error));
    EXPECT_FALSE(animation->SetSpeed(looping, std::numeric_limits<float>::infinity(), error));
    EXPECT_FALSE(animation->SetSpeed(looping, -1, error));
}

TEST_F(AnimationRuntime, CrossfadeBlendsPosesPausesAndFinishesWithoutChangingOtherEntities) {
    const auto fading = AddBot("fading"), idle = AddBot("idle reference"), walk = AddBot("walk reference");
    Step(0);
    ASSERT_TRUE(animation->Play(walk, walkRef, 0, true, error)) << error;
    ASSERT_TRUE(animation->Play(fading, walkRef, 1.f, true, error)) << error;
    EXPECT_FALSE(animation->Play(fading, idleRef, .2f, true, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(animation->Seek(fading, .2, error));
    Step(.5);
    const auto old = animation->GetPose(fading);
    ASSERT_TRUE(old);
    const AnimationPose oldCopy = *old;
    ASSERT_TRUE(animation->GetPlaybackState(fading));
    EXPECT_TRUE(animation->GetPlaybackState(fading)->transitioning);
    EXPECT_TRUE(PosesDiffer(*old, *animation->GetPose(idle)));
    EXPECT_TRUE(PosesDiffer(*old, *animation->GetPose(walk)));
    const auto rig = host.GetSceneAssets().LoadSkeleton(SkeletonPath);
    ASSERT_TRUE(rig);
    bool checkedTorso = false;
    for (int i = 0; i < rig->data->skeleton.num_joints(); ++i) {
        if (std::string(rig->data->skeleton.joint_names()[i]) != "Torso") continue;
        // At half fade, blend the fixture's .025 and .04 local-Y bobs.
        EXPECT_NEAR(old->jointModels[i]._41, 1.f - (.025f + .04f) * .5f, 3e-4f);
        checkedTorso = true;
    }
    EXPECT_TRUE(checkedTorso);
    ASSERT_TRUE(animation->SetPaused(fading, true));
    Step(.25);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(fading)->timeSeconds, .5);
    EXPECT_TRUE(animation->GetPlaybackState(fading)->transitioning);
    ExpectPoseNear(*animation->GetPose(fading), *old);
    ASSERT_TRUE(animation->SetPaused(fading, false));
    Step(.5);
    EXPECT_FALSE(animation->GetPlaybackState(fading)->transitioning);
    // Match the independent walk player to the target's wrapped clock.
    ASSERT_TRUE(animation->Seek(walk, 0, error)) << error;
    Step(0);
    ExpectPoseNear(*animation->GetPose(fading), *animation->GetPose(walk));
    ExpectPoseNear(*old, oldCopy);
    EXPECT_TRUE(animation->GetPlaybackState(idle)->error.empty());
}

TEST_F(AnimationRuntime, RemovalStopAndResetDiscardTransientState) {
    const auto a = AddBot("removed component"), b = AddBot("destroyed entity");
    Step(.25);
    const auto retained = animation->GetPose(a);
    ASSERT_TRUE(retained);
    const AnimationPose saved = *retained;
    ASSERT_TRUE(animation->Play(a, walkRef, 1, true, error)) << error;
    host.GetScene().RemoveComponent<Animator>(a);
    EXPECT_FALSE(animation->GetPose(a));
    EXPECT_FALSE(animation->GetPlaybackState(a));
    host.GetScene().DestroyEntity(b);
    EXPECT_FALSE(animation->GetPose(b));
    EXPECT_FALSE(animation->GetPlaybackState(b));
    host.GetScene().AddComponent<Animator>(a, Animator{skeletonRef, idleRef});
    Step(0);
    ASSERT_TRUE(animation->GetPlaybackState(a));
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, 0);
    EXPECT_FALSE(animation->GetPlaybackState(a)->transitioning);
    ASSERT_TRUE(animation->Stop(a));
    Step(.25);
    EXPECT_FALSE(animation->GetPlaybackState(a)->playing);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, 0);
    auto* renderer = host.GetScene().AddComponent<SkinnedMeshRenderer>(a);
    ASSERT_NE(renderer, nullptr);
    renderer->posedBounds = Aabb{};
    animation->ResetSimulation();
    EXPECT_FALSE(animation->GetPose(a));
    EXPECT_FALSE(animation->GetPlaybackState(a));
    EXPECT_FALSE(renderer->posedBounds);
    Step(0);
    ASSERT_TRUE(animation->GetPlaybackState(a));
    EXPECT_TRUE(animation->GetPlaybackState(a)->playing);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, 0);
    ExpectPoseNear(*retained, saved);
}

TEST_F(AnimationRuntime, RejectsClipFromDifferentSourceRevisionEvenWithSameJointCount) {
    // Existing resources remain immutable: load a separate descriptor after a
    // metadata-only source edit, retaining the already-loaded original clip.
    ASSERT_NO_FATAL_FAILURE(EditGlb([](Json& json) { json["asset"]["generator"] = "new source revision"; }));
    ASSERT_NO_FATAL_FAILURE(WriteJson("revised.skeleton.json", ReadJson("model.skeleton.json")));
    const AssetMetadata metadata{NewUuid(), CatalogAssetKind::Skeleton};
    ASSERT_TRUE(WriteAssetMetadata(bot / "revised.skeleton.json", metadata, error)) << error;
    auto& assets = host.GetSceneAssets();
    ASSERT_TRUE(assets.RescanCatalog(error)) << error;
    const auto revised = assets.LoadSkeleton("game/models/TestBot/revised.skeleton.json");
    const auto original = assets.LoadSkeleton(SkeletonPath);
    ASSERT_TRUE(revised);
    ASSERT_TRUE(original);
    ASSERT_EQ(revised->jointCount, original->jointCount);
    ASSERT_NE(revised->signature, original->signature);
    const auto entity = host.GetScene().CreateEntity("mismatched source");
    host.GetScene().AddComponent<Animator>(entity,
        Animator{{metadata.id, "game/models/TestBot/revised.skeleton.json"}, idleRef});
    Step(0);
    const auto state = animation->GetPlaybackState(entity);
    ASSERT_TRUE(state);
    EXPECT_NE(state->error.find("does not match"), std::string::npos) << state->error;
    EXPECT_DOUBLE_EQ(state->timeSeconds, 0);
    EXPECT_FLOAT_EQ(state->duration, 0);
    Step(.5);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, 0);
}

TEST_F(AnimationRuntime, ExplicitPlayRetriesFailedSkeletonAndRetiresItsOldHandle) {
    auto& assets = host.GetSceneAssets();
    assets.UnloadAll();
    const auto validDescriptor = ReadJson("model.skeleton.json");
    auto invalidDescriptor = validDescriptor;
    invalidDescriptor["version"] = 99;
    ASSERT_NO_FATAL_FAILURE(WriteJson("model.skeleton.json", invalidDescriptor));

    const auto entity = host.GetScene().CreateEntity("repairable animator");
    // No initial clip request: isolate skeleton failure from clip-source failure.
    host.GetScene().AddComponent<Animator>(entity, Animator{skeletonRef, idleRef, 1.f, true, false});
    const auto failedHandle = assets.RequestSkeleton(skeletonRef);
    ASSERT_TRUE(failedHandle);
    Step(0); // The instance retains this loading generation.
    ASSERT_FALSE(assets.LoadSkeleton(SkeletonPath)); // Drain the actual request synchronously.
    ASSERT_EQ(assets.GetState(failedHandle), AssetState::Failed);
    Step(0);
    const auto failed = animation->GetPlaybackState(entity);
    ASSERT_TRUE(failed);
    EXPECT_FALSE(failed->ready);
    EXPECT_FALSE(failed->error.empty());
    EXPECT_FALSE(animation->GetPose(entity));

    ASSERT_NO_FATAL_FAILURE(WriteJson("model.skeleton.json", validDescriptor));
    Step(.25);
    // Repair alone must not create a per-frame retry loop.
    EXPECT_EQ(assets.GetState(failedHandle), AssetState::Failed);
    EXPECT_FALSE(animation->GetPose(entity));
    ASSERT_TRUE(animation->Play(entity, idleRef, 0, true, error)) << error;
    ASSERT_TRUE(assets.LoadSkeleton(SkeletonPath));
    ASSERT_TRUE(assets.LoadAnimationClip(IdlePath));
    const auto repairedHandle = assets.RequestSkeleton(skeletonRef);
    EXPECT_NE(repairedHandle, failedHandle);
    EXPECT_EQ(assets.GetState(repairedHandle), AssetState::Ready);
    EXPECT_EQ(assets.GetState(failedHandle), AssetState::Failed);
    EXPECT_FALSE(assets.Peek(failedHandle));
    Step(0);
    const auto recovered = animation->GetPlaybackState(entity);
    ASSERT_TRUE(recovered);
    EXPECT_TRUE(recovered->ready);
    EXPECT_TRUE(recovered->playing);
    EXPECT_TRUE(recovered->error.empty()) << recovered->error;
    EXPECT_DOUBLE_EQ(recovered->timeSeconds, 0);
    ASSERT_TRUE(animation->GetPose(entity));
    Step(.25);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, .25);
}

TEST_F(AnimationRuntime, SceneReplacementAndSameFrameReattachmentOwnNewStateAndConservativeBounds) {
    CpuPayload cpu;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", cpu)) << error;
    // Bounds need only immutable CPU skin bindings; no mesh upload or renderer.
    SkinnedMeshAsset mesh;
    mesh.bounds = cpu.skinnedMesh.bounds;
    mesh.skeletonSignature = cpu.skinnedMesh.skeletonSignature;
    mesh.skeletonReference = cpu.skinnedMesh.skeletonReference;
    for (const auto& section : cpu.skinnedMesh.sections)
        mesh.sections.push_back({{}, section.materialSlot, section.binding});

    const auto oldEntity = AddBot("before replacement");
    Step(.375);
    const auto retained = animation->GetPose(oldEntity);
    ASSERT_TRUE(retained);
    const AnimationPose saved = *retained;
    const auto oldSkeletonHandle = host.GetSceneAssets().RequestSkeleton(skeletonRef);
    ASSERT_TRUE(host.GetSceneAssets().Peek(oldSkeletonHandle));
    ASSERT_TRUE(animation->SetPaused(oldEntity, true));
    ASSERT_TRUE(animation->SetSpeed(oldEntity, 3.f, error)) << error;
    ASSERT_TRUE(animation->Play(oldEntity, walkRef, 2.f, true, error)) << error;

    SceneEntityDefinition replacement;
    replacement.id = host.GetScene().GetId(oldEntity); // Same authored identity, new registry lifetime.
    replacement.name = "after replacement";
    replacement.animator = Animator{skeletonRef, walkRef};
    SceneDocument document;
    document.entities.push_back(replacement);
    std::string text;
    ASSERT_TRUE(SerializeSceneDocument(document, text, error)) << error;
    const auto sceneFile = root / "replacement.scene.json";
    ASSERT_TRUE(CreateNewFile(sceneFile, text, error)) << error;
    ASSERT_TRUE(host.LoadScene(sceneFile, error)) << error;
    EXPECT_FALSE(animation->GetPose(oldEntity));
    EXPECT_FALSE(animation->GetPlaybackState(oldEntity));
    EXPECT_FALSE(host.GetSceneAssets().Peek(oldSkeletonHandle));
    ExpectPoseNear(*retained, saved);

    auto& assets = host.GetSceneAssets();
    ASSERT_TRUE(assets.LoadSkeleton(SkeletonPath));
    ASSERT_TRUE(assets.LoadAnimationClip(WalkPath));
    const auto entity = host.GetScene().FindById(replacement.id.ToString());
    ASSERT_TRUE(host.GetScene().IsValid(entity));
    ASSERT_NE(host.GetScene().TryGetComponent<Animator>(entity), nullptr);
    EXPECT_EQ(*host.GetScene().TryGetComponent<Animator>(entity), *replacement.animator);
    Step(0);
    const auto initial = animation->GetPose(entity);
    ASSERT_TRUE(initial);
    EXPECT_NE(initial, retained);
    EXPECT_NE(assets.RequestSkeleton(skeletonRef), oldSkeletonHandle);
    ASSERT_TRUE(animation->GetPlaybackState(entity));
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, 0);
    EXPECT_FALSE(animation->GetPlaybackState(entity)->transitioning);
    EXPECT_TRUE(animation->GetPlaybackState(entity)->playing);
    EXPECT_TRUE(animation->GetPlaybackState(entity)->error.empty());
    Step(.125);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, .125); // Speed reset to authored 1.

    for (const double time : {0.0, .125, .25, .5, .75}) {
        SCOPED_TRACE(::testing::Message() << "walk time " << time);
        ASSERT_TRUE(animation->Seek(entity, time, error)) << error;
        Step(0);
        const auto pose = animation->GetPose(entity);
        ASSERT_TRUE(pose);
        const auto bounds = SkinnedMeshBounds(mesh, pose.get());
        for (const auto& section : cpu.skinnedMesh.sections) {
            ASSERT_TRUE(section.binding);
            const auto& binding = *section.binding;
            for (const auto& vertex : section.vertices) {
                const Vector3 position{vertex.position.x, vertex.position.y, vertex.position.z};
                Vector3 skinned{};
                for (std::size_t i = 0; i < vertex.weights.size(); ++i) {
                    if (vertex.weights[i] <= 0) continue;
                    const auto paletteIndex = vertex.joints[i];
                    ASSERT_LT(paletteIndex, binding.joints.size());
                    ASSERT_LT(binding.joints[paletteIndex], pose->jointModels.size());
                    // Independently skin each CPU vertex; bounds union must enclose
                    // the weighted position, including multi-joint influences.
                    const auto matrix = binding.inverseBind[paletteIndex] * pose->jointModels[binding.joints[paletteIndex]];
                    skinned += Vector3::Transform(position, matrix) * vertex.weights[i];
                }
                EXPECT_TRUE(std::isfinite(skinned.x) && std::isfinite(skinned.y) && std::isfinite(skinned.z));
                constexpr float tolerance = 5e-4f;
                EXPECT_GE(skinned.x, bounds.min.x - tolerance);
                EXPECT_GE(skinned.y, bounds.min.y - tolerance);
                EXPECT_GE(skinned.z, bounds.min.z - tolerance);
                EXPECT_LE(skinned.x, bounds.max.x + tolerance);
                EXPECT_LE(skinned.y, bounds.max.y + tolerance);
                EXPECT_LE(skinned.z, bounds.max.z + tolerance);
            }
        }
    }
    ASSERT_TRUE(animation->GetPose(entity));
    EXPECT_TRUE(PosesDiffer(*animation->GetPose(entity), *initial));

    const Animator identical = *host.GetScene().TryGetComponent<Animator>(entity);
    ASSERT_TRUE(animation->SetSpeed(entity, 4.f, error)) << error;
    ASSERT_TRUE(animation->SetPaused(entity, true));
    host.GetScene().RemoveComponent<Animator>(entity);
    host.GetScene().AddComponent<Animator>(entity, identical); // No intervening update.
    EXPECT_FALSE(animation->GetPlaybackState(entity));
    EXPECT_FALSE(animation->GetPose(entity));
    Step(0);
    ASSERT_TRUE(animation->GetPlaybackState(entity));
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, 0);
    EXPECT_TRUE(animation->GetPlaybackState(entity)->playing);
    EXPECT_FALSE(animation->GetPlaybackState(entity)->transitioning);
    ASSERT_TRUE(animation->GetPose(entity));
    ExpectPoseNear(*animation->GetPose(entity), *initial);
    Step(.125);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, .125);
    ExpectPoseNear(*retained, saved);
}

class AnimationPreview : public AnimationRuntime {
protected:
    void SetUp() override {
        host.SetEditorMode();
        host.AddSubsystem<ScriptSubsystem>();
        AnimationRuntime::SetUp();
    }
    void PreviewStep(double real, double simulation = 0) {
        animation->LateUpdate(FrameContext{.realDeltaSeconds = real, .deltaSeconds = simulation});
    }
};

TEST_F(AnimationRuntime, PreviewRequiresEditorEditMode) {
    const auto entity = AddBot("standalone");
    EXPECT_FALSE(animation->Preview(entity, idleRef, 0, true, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
}

TEST_F(AnimationPreview, RealClockOnlyAdvancesExplicitEntityAndNeverChangesAuthoring) {
    const auto a = AddBot("preview"), b = AddBot("frozen");
    PreviewStep(0);
    const auto authored = *host.GetScene().TryGetComponent<Animator>(a);
    const auto initial = animation->GetPose(a);
    const auto frozen = animation->GetPose(b);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(frozen);
    const double simulationTime = host.GetFrame().simulationTimeSeconds;
    ASSERT_TRUE(animation->Preview(a, walkRef, 0, false, error)) << error;
    ASSERT_TRUE(animation->SetSpeed(a, 2, error)) << error;
    PreviewStep(.125, .75); // Even a supplied simulation delta cannot animate other Edit entities.
    EXPECT_EQ(animation->GetPreviewEntity(), a);
    ASSERT_TRUE(animation->GetPlaybackState(a));
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, .25);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(b)->timeSeconds, 0);
    ExpectPoseNear(*animation->GetPose(b), *frozen);
    EXPECT_TRUE(PosesDiffer(*animation->GetPose(a), *initial));
    EXPECT_EQ(*host.GetScene().TryGetComponent<Animator>(a), authored);
    EXPECT_DOUBLE_EQ(host.GetFrame().simulationTimeSeconds, simulationTime);

    ASSERT_TRUE(animation->SetPaused(a, true));
    PreviewStep(.3);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, .25);
    ASSERT_TRUE(animation->Seek(a, .6, error)) << error;
    PreviewStep(.2);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, .6);
    animation->EndPreview();
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
    ASSERT_TRUE(animation->GetPose(a)); // EndPreview restores synchronously when assets are ready.
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, 0);
    ExpectPoseNear(*animation->GetPose(a), *initial);
    EXPECT_EQ(*host.GetScene().TryGetComponent<Animator>(a), authored);
}

TEST_F(AnimationPreview, ReplacingPreviewRestoresPreviousEntityAndCrossfadeUsesRealClock) {
    const auto a = AddBot("first"), b = AddBot("second");
    PreviewStep(0);
    const auto initial = animation->GetPose(a);
    ASSERT_TRUE(animation->Preview(a, idleRef, 0, true, error)) << error;
    PreviewStep(.25);
    ASSERT_TRUE(animation->Preview(a, walkRef, .5f, true, error)) << error;
    PreviewStep(.25);
    EXPECT_TRUE(animation->GetPlaybackState(a)->transitioning);
    ASSERT_TRUE(animation->SetPaused(a, true));
    const auto paused = animation->GetPose(a);
    PreviewStep(.5);
    ExpectPoseNear(*animation->GetPose(a), *paused);
    ASSERT_TRUE(animation->SetPaused(a, false));
    PreviewStep(.25);
    EXPECT_FALSE(animation->GetPlaybackState(a)->transitioning);
    ASSERT_TRUE(animation->Preview(b, walkRef, 0, true, error)) << error;
    EXPECT_EQ(animation->GetPreviewEntity(), b);
    ExpectPoseNear(*animation->GetPose(a), *initial);
    PreviewStep(.125);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(a)->timeSeconds, 0);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(b)->timeSeconds, .125);
}

TEST_F(AnimationPreview, EndRestoresRestPoseWhenPlayOnStartIsDisabled) {
    const auto entity = AddBot("rest");
    host.GetScene().Modify<Animator>(entity, [](auto& animator) { animator.playOnStart = false; });
    PreviewStep(0);
    const auto rest = animation->GetPose(entity);
    ASSERT_TRUE(rest);
    ASSERT_TRUE(animation->Preview(entity, walkRef, 0, true, error)) << error;
    PreviewStep(.375);
    EXPECT_TRUE(PosesDiffer(*animation->GetPose(entity), *rest));
    animation->EndPreview();
    ASSERT_TRUE(animation->GetPlaybackState(entity));
    EXPECT_FALSE(animation->GetPlaybackState(entity)->playing);
    EXPECT_FLOAT_EQ(animation->GetPlaybackState(entity)->duration, 0);
    ExpectPoseNear(*animation->GetPose(entity), *rest);
}

TEST_F(AnimationPreview, LifecycleResetSceneReplacementAndEnteringPlayClearPreview) {
    auto entity = AddBot("lifecycle");
    const auto begin = [&] { return animation->Preview(entity, walkRef, 0, true, error); };
    ASSERT_TRUE(begin()) << error;
    host.GetScene().Modify<Animator>(entity, [](auto& animator) { animator.speed = 3; });
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
    ASSERT_TRUE(begin()) << error;
    const auto authored = *host.GetScene().TryGetComponent<Animator>(entity);
    host.GetScene().RemoveComponent<Animator>(entity);
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
    host.GetScene().AddComponent<Animator>(entity, authored);
    ASSERT_TRUE(begin()) << error;
    animation->ResetSimulation();
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
    EXPECT_FALSE(animation->GetPlaybackState(entity));
    ASSERT_TRUE(begin()) << error;
    ASSERT_TRUE(host.PlayCurrentScene(root, {}, error)) << error;
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
    entity = host.GetScene().FindFirstByName("lifecycle");
    EXPECT_FALSE(begin()); // Preview API rejects Play, including paused Play.
    ASSERT_TRUE(host.GetSceneAssets().LoadSkeleton(SkeletonPath));
    ASSERT_TRUE(host.GetSceneAssets().LoadAnimationClip(IdlePath));
    host.SetPaused(true);
    PreviewStep(.5, .5);
    ASSERT_TRUE(animation->GetPlaybackState(entity));
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, 0);
    host.SetPaused(false);
    PreviewStep(.125, .125);
    EXPECT_DOUBLE_EQ(animation->GetPlaybackState(entity)->timeSeconds, .375); // Authored speed 3.
    host.StopPlay();
    entity = host.GetScene().FindFirstByName("lifecycle");
    ASSERT_TRUE(begin()) << error;
    host.GetScene().DestroyEntity(entity);
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
    entity = AddBot("scene replacement");
    ASSERT_TRUE(begin()) << error;
    host.GetScene().Clear();
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
    entity = AddBot("shutdown");
    ASSERT_TRUE(begin()) << error;
    host.Shutdown();
    EXPECT_TRUE(animation->GetPreviewEntity() == entt::null);
}

TEST_F(AnimationImport, SameJointCountWithDifferentSourceSignatureFallsBackToRestPalette) {
    CpuPayload original, mesh, changed;
    ASSERT_TRUE(Import(AssetKind::Skeleton, "model.skeleton.json", original)) << error;
    ASSERT_TRUE(Import(AssetKind::SkinnedMesh, "model.skinned-mesh.json", mesh)) << error;
    ASSERT_NO_FATAL_FAILURE(EditGlb([](Json& json) { json["asset"]["generator"] = "different source revision"; }));
    ASSERT_TRUE(Import(AssetKind::Skeleton, "model.skeleton.json", changed)) << error;
    EXPECT_EQ(original.skeleton.jointCount, changed.skeleton.jointCount);
    EXPECT_NE(original.skeleton.signature, changed.skeleton.signature);
    AnimationPose incompatible;
    incompatible.skeletonSignature = changed.skeleton.signature;
    incompatible.jointModels.assign(changed.skeleton.jointCount, Matrix::CreateTranslation(100, 200, 300));
    for (const auto& section : mesh.skinnedMesh.sections) {
        const auto palette = BuildSkinningPalette(*section.binding, original.skeleton.signature, &incompatible);
        ASSERT_TRUE(palette);
        ASSERT_EQ(palette->matrices.size(), section.binding->restPalette.size());
        for (std::size_t i = 0; i < palette->matrices.size(); ++i)
            ExpectMatrixNear(palette->matrices[i], section.binding->restPalette[i]);
    }
}
} // namespace
