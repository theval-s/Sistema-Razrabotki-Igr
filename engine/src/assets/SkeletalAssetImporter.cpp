#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include "../animation/AnimationData.hpp"
#include "ModelSlots.hpp"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <nlohmann/json.hpp>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/animation_optimizer.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/span.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace engine {
namespace {
using Json = nlohmann::json;
using DirectX::SimpleMath::Matrix;
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Vector4;
using RawSkeleton = ozz::animation::offline::RawSkeleton;
using RawAnimation = ozz::animation::offline::RawAnimation;
constexpr unsigned ImportFlags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
    aiProcess_SortByPType | aiProcess_FlipUVs | aiProcess_FlipWindingOrder;
constexpr std::size_t MaxSourceBytes = 256 * 1024 * 1024;
constexpr std::size_t MaxJsonBytes = 16 * 1024 * 1024;
constexpr std::size_t MaxVertices = 4 * 1024 * 1024;
constexpr std::size_t MaxKeys = 4 * 1024 * 1024;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string LowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

Json ParseJson(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 64, "skeletal JSON nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key)
            Require(keys.back().insert(value.get<std::string>()).second,
                    "duplicate JSON field: " + value.get<std::string>());
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text.begin(), text.end(), callback);
}

void Fields(const Json& value, std::initializer_list<const char*> fields) {
    Require(value.is_object() && value.size() == fields.size(), "unexpected or missing skeletal descriptor fields");
    for (const auto* field : fields)
        Require(value.contains(field), std::string("missing skeletal descriptor field: ") + field);
}

Json Descriptor(const std::filesystem::path& path, AssetKind kind) {
    const std::string suffix = kind == AssetKind::Skeleton ? ".skeleton.json" :
        kind == AssetKind::AnimationClip ? ".animation.json" : ".skinned-mesh.json";
    Require(LowerAscii(ToUtf8(path)).ends_with(suffix), "expected " + suffix + " descriptor");
    std::string text, error;
    Require(ReadTextFile(path, text, error, 1024 * 1024), error);
    auto root = ParseJson(text);
    if (kind == AssetKind::Skeleton) Fields(root, {"version", "source"});
    else if (kind == AssetKind::AnimationClip) Fields(root, {"version", "skeleton", "clip"});
    else Fields(root, {"version", "skeleton"});
    Require(root.at("version").is_number_integer() && root.at("version") == 1,
            "skeletal descriptor version must be integer 1");
    return root;
}

AssetReference Reference(const Json& json) {
    Fields(json, {"id", "path"});
    AssetReference reference;
    std::string error;
    Require(ParseAssetReference(json.dump(), reference, error), error);
    return reference;
}

const CatalogEntry& Resolve(const AssetDesc& desc, const AssetReference& reference, CatalogAssetKind kind) {
    Require(desc.catalog != nullptr, "skeletal descriptors require an asset catalog");
    const auto* entry = desc.catalog->Find(reference.id, kind);
    Require(entry != nullptr, "missing or wrong-kind skeletal reference: " + reference.id.ToString());
    return *entry;
}

// Explicit, endian-independent FNV-1a encoding. Convention version covers row-vector
// engine matrices, retained source root, import flags and the same-source policy.
struct Signature {
    std::uint64_t value = 14695981039346656037ull;
    void Byte(std::uint8_t byte) { value = (value ^ byte) * 1099511628211ull; }
    void U32(std::uint32_t word) { for (int i = 0; i != 4; ++i) Byte(static_cast<std::uint8_t>(word >> (i * 8))); }
    void Text(std::string_view text) {
        U32(static_cast<std::uint32_t>(text.size()));
        for (unsigned char byte : text) Byte(byte);
    }
    void Float(float scalar) { U32(std::bit_cast<std::uint32_t>(scalar == 0.f ? 0.f : scalar)); }
};

std::uint32_t Word(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "truncated GLB header");
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
        (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

Json GlbJson(const std::vector<std::uint8_t>& bytes) {
    Require(bytes.size() >= 20 && Word(bytes, 0) == 0x46546c67 && Word(bytes, 4) == 2 &&
            Word(bytes, 8) == bytes.size(), "source must be a valid glTF 2 GLB");
    Require(Word(bytes, 16) == 0x4e4f534a, "GLB first chunk must be JSON");
    const auto length = Word(bytes, 12);
    Require(length <= MaxJsonBytes && length <= bytes.size() - 20, "GLB JSON chunk exceeds size limit or is truncated");
    auto json = ParseJson(std::string_view(reinterpret_cast<const char*>(bytes.data() + 20), length));
    Require(json.is_object() && json.at("asset").at("version") == "2.0", "only glTF 2.0 is supported");
    if (json.contains("buffers")) {
        Require(json.at("buffers").is_array() && json.at("buffers").size() <= 1,
                "version 1 skeletal sources require a single embedded GLB buffer");
        for (const auto& buffer : json.at("buffers"))
            Require(!buffer.contains("uri"), "external/data URI buffers are unsupported; embed the buffer in GLB");
    }
    if (json.contains("images")) for (const auto& image : json.at("images"))
        Require(!image.contains("uri"), "version 1 skeletal sources require embedded GLB images (no URI dependencies)");
    Require(!json.contains("extensionsRequired") || json.at("extensionsRequired").empty(),
            "required glTF extensions are unsupported by skeletal conversion version 1");
    return json;
}

bool Finite(const aiVector3D& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
ozz::math::Float3 Float3(const aiVector3D& v) {
    Require(Finite(v), "nonfinite skeletal vector");
    return {v.x, v.y, v.z};
}
ozz::math::Quaternion Quaternion(const aiQuaternion& q) {
    Require(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w),
            "nonfinite skeletal quaternion");
    const double length = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
    Require(length > 1e-12, "zero-length skeletal quaternion");
    return {static_cast<float>(q.x / length), static_cast<float>(q.y / length),
            static_cast<float>(q.z / length), static_cast<float>(q.w / length)};
}

Matrix ToRow(const aiMatrix4x4& m) {
    // Each source column becomes an engine row; never reinterpret Assimp storage.
    Matrix result(m.a1, m.b1, m.c1, m.d1, m.a2, m.b2, m.c2, m.d2,
                  m.a3, m.b3, m.c3, m.d3, m.a4, m.b4, m.c4, m.d4);
    const float* rows[] = {&result._11, &result._21, &result._31, &result._41};
    for (const auto* row : rows) for (int i = 0; i != 4; ++i)
        Require(std::isfinite(row[i]), "nonfinite skeletal matrix");
    return result;
}

bool Near(const Matrix& a, const Matrix& b, float tolerance = 2e-4f) {
    const float* ar[] = {&a._11, &a._21, &a._31, &a._41};
    const float* br[] = {&b._11, &b._21, &b._31, &b._41};
    for (int r = 0; r != 4; ++r) for (int c = 0; c != 4; ++c)
        if (!std::isfinite(ar[r][c]) || !std::isfinite(br[r][c]) ||
            std::abs(ar[r][c] - br[r][c]) > tolerance * (std::max)(1.f, std::abs(br[r][c]))) return false;
    return true;
}

ozz::math::Transform Rest(const aiNode& node) {
    aiVector3D scale, translation;
    aiQuaternion rotation;
    node.mTransformation.Decompose(scale, rotation, translation);
    ozz::math::Transform result{Float3(translation), Quaternion(rotation), Float3(scale)};
    Require(std::abs(scale.x) > 1e-8f && std::abs(scale.y) > 1e-8f && std::abs(scale.z) > 1e-8f,
            "singular rest scale at node: " + std::string(node.mName.C_Str()));
    const aiQuaternion normalized(result.rotation.w, result.rotation.x, result.rotation.y, result.rotation.z);
    Require(Near(ToRow(aiMatrix4x4(scale, normalized, translation)), ToRow(node.mTransformation)),
            "unsupported shear/non-TRS transform at node: " + std::string(node.mName.C_Str()));
    return result;
}

ozz::math::Transform AuthoredRest(const aiNode& node, const Json& source) {
    if (source.contains("matrix")) {
        Require(!source.contains("translation") && !source.contains("rotation") && !source.contains("scale"),
                "glTF matrix nodes cannot also contain TRS");
        return Rest(node);
    }
    const auto vector = [&source](const char* field, aiVector3D fallback) {
        if (!source.contains(field)) return fallback;
        const auto& v = source.at(field);
        Require(v.is_array() && v.size() == 3, std::string("invalid glTF ") + field);
        return aiVector3D(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
    };
    const auto translation = vector("translation", {0, 0, 0});
    const auto scale = vector("scale", {1, 1, 1});
    aiQuaternion rotation;
    if (source.contains("rotation")) {
        const auto& q = source.at("rotation");
        Require(q.is_array() && q.size() == 4, "invalid glTF rotation");
        rotation = aiQuaternion(q[3].get<float>(), q[0].get<float>(), q[1].get<float>(), q[2].get<float>());
    }
    ozz::math::Transform result{Float3(translation), Quaternion(rotation), Float3(scale)};
    Require(std::abs(scale.x) > 1e-8f && std::abs(scale.y) > 1e-8f && std::abs(scale.z) > 1e-8f,
            "singular authored glTF rest scale");
    const aiQuaternion normalized(result.rotation.w, result.rotation.x, result.rotation.y, result.rotation.z);
    Require(Near(ToRow(aiMatrix4x4(scale, normalized, translation)), ToRow(node.mTransformation)),
            "authored glTF TRS does not match imported node transform");
    return result;
}

std::string PathSegment(std::string_view name) {
    std::string result;
    for (char c : name) {
        if (c == '~') result += "~0";
        else if (c == '/') result += "~1";
        else result += c;
    }
    return result;
}

struct SceneSkeleton {
    std::vector<const aiNode*> nodes;
    std::vector<int> parents;
    std::vector<int> jointToSource;
    std::vector<ozz::math::Transform> rest;
    std::vector<Matrix> models;
    std::unordered_map<std::string, std::uint16_t> names;
    std::unordered_map<std::size_t, std::uint16_t> sourceToJoint;
    std::shared_ptr<detail::SkeletonData> data = std::make_shared<detail::SkeletonData>();
    std::uint64_t signature = 0;

    void Visit(const aiNode* node, int parent, const std::string& parentPath,
               RawSkeleton::Joint& joint, std::size_t depth, const Json& gltf, int sourceIndex) {
        Require(node && depth <= 256, "invalid or excessively deep skeletal hierarchy");
        Require(nodes.size() < ozz::animation::Skeleton::kMaxJoints, "skeleton exceeds ozz joint limit");
        const auto index = static_cast<std::uint16_t>(nodes.size());
        const std::string name(node->mName.C_Str(), node->mName.length);
        Require(name.find('\0') == std::string::npos && names.emplace(name, index).second,
                "ambiguous duplicate node name: " + name);
        nodes.push_back(node);
        parents.push_back(parent);
        jointToSource.push_back(sourceIndex);
        const Json* source = nullptr;
        if (sourceIndex >= 0) {
            source = &gltf.at("nodes").at(static_cast<std::size_t>(sourceIndex));
            Require(sourceToJoint.emplace(static_cast<std::size_t>(sourceIndex), index).second,
                    "glTF node is instanced or cyclic in the skeletal hierarchy");
            if (source->contains("name") && !source->at("name").get<std::string>().empty())
                Require(source->at("name") == name, "glTF/Assimp node name mapping mismatch");
        }
        rest.push_back(source ? AuthoredRest(*node, *source) : Rest(*node));
        data->nodePaths.push_back(parentPath + "/" + PathSegment(name));
        joint.name = name.c_str();
        joint.transform = rest.back();
        Require(node->mNumChildren <= ozz::animation::Skeleton::kMaxJoints,
                "node child count exceeds ozz joint limit");
        joint.children.resize(node->mNumChildren);
        const auto path = data->nodePaths.back();
        // Assimp retains glTF child order. Map by source topology, not generated
        // names: unnamed nodes and synthetic scene roots must remain unambiguous.
        const auto& children = source ? source->value("children", Json::array()) :
            gltf.at("scenes").at(gltf.value("scene", 0)).at("nodes");
        Require(children.is_array() && children.size() == node->mNumChildren,
                "glTF/Assimp node hierarchy mapping mismatch");
        for (unsigned i = 0; i < node->mNumChildren; ++i) {
            const int child = children[i].get<int>();
            Require(child >= 0, "negative glTF child node index");
            Visit(node->mChildren[i], index, path, joint.children[i], depth + 1, gltf, child);
        }
    }

    void Build(const aiScene& scene, const std::vector<std::uint8_t>& source, const Json& gltf) {
        RawSkeleton raw;
        raw.roots.resize(1);
        const auto& roots = gltf.at("scenes").at(gltf.value("scene", 0)).at("nodes");
        Require(roots.is_array() && !roots.empty(), "glTF scene must contain skeletal nodes");
        for (const auto& root : roots) Require(root.get<int>() >= 0, "negative glTF root node index");
        Visit(scene.mRootNode, -1, "", raw.roots[0], 0, gltf, roots.size() == 1 ? roots[0].get<int>() : -1);
        auto built = ozz::animation::offline::SkeletonBuilder{}(raw);
        Require(built != nullptr, "ozz skeleton build failed");
        data->skeleton = std::move(*built);
        Require(data->skeleton.num_joints() == static_cast<int>(nodes.size()), "ozz joint count mismatch");
        for (std::size_t i = 0; i < nodes.size(); ++i)
            Require(data->skeleton.joint_parents()[i] == parents[i] &&
                    std::string(data->skeleton.joint_names()[i]) == nodes[i]->mName.C_Str(), "ozz DFS order mismatch");
        std::vector<ozz::math::Float4x4> ozzModels(nodes.size());
        ozz::animation::LocalToModelJob job;
        job.skeleton = &data->skeleton;
        job.input = data->skeleton.joint_rest_poses();
        job.output = ozz::make_span(ozzModels);
        Require(job.Run(), "ozz rest-pose local-to-model failed");
        for (const auto& model : ozzModels) {
            const auto row = detail::ToEngineMatrix(model);
            Require(Near(row, row), "nonfinite skeleton rest model matrix");
            models.push_back(row);
        }
        Signature hash;
        hash.Text("skeletal-convention-v1:glb:root:row:uv-flip:winding-flip:linear:authored-trs");
        hash.U32(static_cast<std::uint32_t>(source.size()));
        for (auto byte : source) hash.Byte(byte);
        hash.U32(static_cast<std::uint32_t>(nodes.size()));
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            hash.Text(data->nodePaths[i]);
            hash.U32(static_cast<std::uint32_t>(parents[i]));
            const auto& t = rest[i];
            for (float f : {t.translation.x, t.translation.y, t.translation.z,
                           t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                           t.scale.x, t.scale.y, t.scale.z}) hash.Float(f);
        }
        signature = hash.value;
    }

    std::uint16_t Joint(const aiString& name) const {
        const auto found = names.find(name.C_Str());
        Require(found != names.end(), "bone/channel node not found: " + std::string(name.C_Str()));
        return found->second;
    }
};

const Json& ValidateClipJson(const Json& gltf, const std::string& name) {
    Require(gltf.contains("animations") && gltf.at("animations").is_array(), "GLB contains no animations");
    const Json* selected = nullptr;
    for (const auto& animation : gltf.at("animations")) {
        if (animation.value("name", std::string{}) != name) continue;
        Require(selected == nullptr, "duplicate source animation name: " + name);
        selected = &animation;
    }
    Require(selected != nullptr, "exact named animation not found in GLB: " + name);
    for (const auto& sampler : selected->at("samplers"))
        Require(sampler.value("interpolation", std::string("LINEAR")) == "LINEAR",
                "only glTF LINEAR animation interpolation is supported; STEP/CUBICSPLINE require explicit baking");
    for (const auto& channel : selected->at("channels")) {
        const auto& target = channel.at("target");
        const auto path = target.at("path").get<std::string>();
        Require(target.contains("node") && (path == "translation" || path == "rotation" || path == "scale"),
                "only node TRS animation channels are supported (no morph weights or animation pointers)");
    }
    return *selected;
}

template <typename Keys, typename SourceKey, typename Convert>
void CopyKeys(Keys& result, const SourceKey* source, unsigned count, double ticks,
              float duration, const typename Keys::value_type::Value& rest, Convert convert) {
    Require(count <= MaxKeys && (count == 0 || source != nullptr), "animation key limit exceeded or missing key array");
    for (unsigned i = 0; i < count; ++i) {
        // The installed Assimp API exposes interpolation per key. Older Assimp
        // releases lack this member; GLB JSON validation remains authoritative.
        if constexpr (requires { source[i].mInterpolation; }) {
            const int interpolation = static_cast<int>(source[i].mInterpolation);
            Require(interpolation == 1 || (interpolation == 2 && std::is_same_v<SourceKey, aiQuatKey>),
                    "Assimp reported unsupported non-linear animation interpolation");
        }
        const double seconds = source[i].mTime / ticks;
        Require(std::isfinite(seconds) && seconds >= 0 && seconds <= double(duration) + 1e-5,
                "animation key time is nonfinite or outside clip duration");
        result.push_back({(std::min)(static_cast<float>(seconds), duration), convert(source[i].mValue)});
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    // Last authored key wins, including times that collapse when converted to float.
    std::size_t output = 0;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (output && result[output - 1].time == result[i].time) result[output - 1] = result[i];
        else result[output++] = result[i];
    }
    result.resize(output);
    if (result.empty()) result.push_back({0.f, rest});
    // glTF clamps outside each sampler range; explicit boundary keys retain that.
    if (result.front().time > 0) result.insert(result.begin(), {0.f, result.front().value});
    if (result.back().time < duration) result.push_back({duration, result.back().value});
}

AnimationClipAsset BuildClip(const aiScene& scene, const Json& gltf, const std::string& name,
                             const SceneSkeleton& skeleton) {
    Require(!name.empty() && name.size() <= 1024, "clip must be a nonempty exact animation name (at most 1024 bytes)");
    const auto& sourceClip = ValidateClipJson(gltf, name);
    std::vector<unsigned> authoredComponents(skeleton.nodes.size(), 0);
    for (const auto& channel : sourceClip.at("channels")) {
        const auto& target = channel.at("target");
        const auto sourceIndex = target.at("node").get<std::size_t>();
        const auto found = skeleton.sourceToJoint.find(sourceIndex);
        Require(found != skeleton.sourceToJoint.end(), "animation targets a node outside the imported scene");
        Require(!gltf.at("nodes").at(sourceIndex).contains("matrix"), "glTF animation cannot target a matrix node");
        const std::string path = target.at("path");
        const unsigned component = path == "translation" ? 1u : path == "rotation" ? 2u : 4u;
        auto& mask = authoredComponents[found->second];
        Require((mask & component) == 0, "duplicate glTF animation target component");
        mask |= component;
    }
    const aiAnimation* selected = nullptr;
    for (unsigned i = 0; i < scene.mNumAnimations; ++i) {
        const auto* animation = scene.mAnimations[i];
        if (!animation || name != animation->mName.C_Str()) continue;
        Require(selected == nullptr, "duplicate Assimp animation name: " + name);
        selected = animation;
    }
    Require(selected != nullptr, "exact named animation not found by Assimp: " + name);
    Require(selected->mNumMeshChannels == 0 && selected->mNumMorphMeshChannels == 0, "mesh/morph animation is unsupported");
    const double ticks = selected->mTicksPerSecond;
    Require(std::isfinite(ticks) && ticks > 0, "glTF animation has missing/invalid ticks-per-second; no implicit fallback");
    const double seconds = selected->mDuration / ticks;
    Require(std::isfinite(seconds) && seconds > 0 && seconds <= 86400,
            "animation duration must be positive, finite and at most 24 hours");
    RawAnimation raw;
    raw.name = name.c_str();
    raw.duration = static_cast<float>(seconds);
    raw.tracks.resize(skeleton.nodes.size());
    std::vector<const aiNodeAnim*> channels(skeleton.nodes.size(), nullptr);
    std::size_t totalKeys = 0;
    for (unsigned i = 0; i < selected->mNumChannels; ++i) {
        const auto* channel = selected->mChannels[i];
        Require(channel != nullptr, "null animation channel");
        const auto joint = skeleton.Joint(channel->mNodeName);
        Require(channels[joint] == nullptr, "duplicate animation channel for node: " + std::string(channel->mNodeName.C_Str()));
        channels[joint] = channel;
        totalKeys += std::size_t(channel->mNumPositionKeys) + channel->mNumRotationKeys + channel->mNumScalingKeys;
        Require(totalKeys <= MaxKeys, "animation total key limit exceeded");
    }
    for (std::size_t i = 0; i < raw.tracks.size(); ++i) {
        auto& track = raw.tracks[i];
        const auto& rest = skeleton.rest[i];
        const auto* channel = channels[i];
        const unsigned mask = authoredComponents[i];
        Require(mask == 0 || channel != nullptr, "authored glTF animation channel missing from Assimp scene");
        // Ignore Assimp's synthesized component keys. Their matrix decomposition
        // can choose different negative-scale signs than the authored glTF TRS.
        CopyKeys(track.translations, channel && (mask & 1) ? channel->mPositionKeys : nullptr,
                 channel && (mask & 1) ? channel->mNumPositionKeys : 0, ticks, raw.duration, rest.translation, Float3);
        CopyKeys(track.rotations, channel && (mask & 2) ? channel->mRotationKeys : nullptr,
                 channel && (mask & 2) ? channel->mNumRotationKeys : 0, ticks, raw.duration, rest.rotation, Quaternion);
        CopyKeys(track.scales, channel && (mask & 4) ? channel->mScalingKeys : nullptr,
                 channel && (mask & 4) ? channel->mNumScalingKeys : 0, ticks, raw.duration, rest.scale, Float3);
    }
    Require(raw.Validate(), "invalid ozz raw animation");
    RawAnimation optimized;
    ozz::animation::offline::AnimationOptimizer optimizer;
    optimizer.setting.tolerance = 1e-5f;
    Require(optimizer(raw, skeleton.data->skeleton, &optimized), "ozz animation optimization failed");
    auto built = ozz::animation::offline::AnimationBuilder{}(optimized);
    Require(built != nullptr, "ozz animation build failed");
    auto data = std::make_shared<detail::AnimationData>();
    data->animation = std::move(*built);
    return {std::move(data), skeleton.signature, raw.duration};
}

void Enclose(Aabb& bounds, const Vector3& point, bool& initialized) {
    Require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z), "nonfinite skinned bounds");
    if (!initialized) { bounds = {point, point}; initialized = true; }
    else { bounds.min = Vector3::Min(bounds.min, point); bounds.max = Vector3::Max(bounds.max, point); }
}

SkinnedMeshCpu BuildMesh(const aiScene& scene, const SceneSkeleton& skeleton, const AssetReference& reference,
                         const Json& gltf) {
    SkinnedMeshCpu result;
    result.skeletonReference = reference;
    result.skeletonSignature = skeleton.signature;
    bool boundsInitialized = false;
    std::size_t totalVertices = 0;
    std::size_t totalFaces = 0;
    std::size_t totalInfluences = 0;
    for (std::size_t nodeIndex = 0; nodeIndex < skeleton.nodes.size(); ++nodeIndex) {
        const auto* node = skeleton.nodes[nodeIndex];
        const Json* sourceSkin = nullptr;
        const int sourceIndex = skeleton.jointToSource[nodeIndex];
        if (sourceIndex >= 0) {
            const auto& sourceNode = gltf.at("nodes").at(static_cast<std::size_t>(sourceIndex));
            if (sourceNode.contains("skin"))
                sourceSkin = &gltf.at("skins").at(sourceNode.at("skin").get<std::size_t>());
        }
        for (unsigned instance = 0; instance < node->mNumMeshes; ++instance) {
            Require(result.sections.size() < 65536, "mesh section limit exceeded");
            Require(node->mMeshes[instance] < scene.mNumMeshes, "invalid node mesh index");
            const auto* mesh = scene.mMeshes[node->mMeshes[instance]];
            Require(mesh && mesh->HasPositions() && mesh->HasNormals() && mesh->HasFaces() &&
                    mesh->mPrimitiveTypes == aiPrimitiveType_TRIANGLE, "skinned mesh section must contain only triangles with positions/normals");
            Require(mesh->mNumAnimMeshes == 0, "morph-target meshes are unsupported");
            totalVertices += mesh->mNumVertices;
            totalFaces += mesh->mNumFaces;
            Require(totalVertices <= MaxVertices && totalFaces <= MaxVertices * 2, "skinned geometry size limit exceeded");
            SkinnedMeshSectionCpu section;
            section.materialSlot = mesh->mMaterialIndex;
            auto binding = std::make_shared<SkinBinding>();
            struct Influence { std::uint32_t palette; float weight; };
            std::vector<std::vector<Influence>> influences(mesh->mNumVertices);
            std::set<std::uint16_t> boneJoints;
            const unsigned boneCount = sourceSkin ? mesh->mNumBones : 0;
            Require(boneCount <= skeleton.nodes.size(), "mesh bone count exceeds skeleton joint count");
            if (sourceSkin) Require(sourceSkin->at("joints").size() == boneCount, "glTF/Assimp skin palette size mismatch");
            for (unsigned b = 0; b < boneCount; ++b) {
                const auto* bone = mesh->mBones[b];
                Require(bone != nullptr, "null skin bone");
                // Assimp preserves skin palette order but some versions name
                // unnamed bones "bone_N" while the corresponding node is named
                // "nodes[N]". Resolve from source skin indices in that case.
                const auto sourceJoint = sourceSkin->at("joints").at(b).get<std::size_t>();
                const auto found = skeleton.sourceToJoint.find(sourceJoint);
                Require(found != skeleton.sourceToJoint.end(), "skin joint is outside the imported scene");
                const auto joint = found->second;
                const auto& jointNode = gltf.at("nodes").at(sourceJoint);
                if (jointNode.contains("name") && !jointNode.at("name").get<std::string>().empty())
                    Require(jointNode.at("name") == bone->mName.C_Str(), "glTF/Assimp skin palette name mismatch");
                Require(boneJoints.insert(joint).second, "duplicate mesh bone: " + std::string(bone->mName.C_Str()));
                const auto inverseBind = ToRow(bone->mOffsetMatrix);
                const auto palette = static_cast<std::uint32_t>(binding->joints.size());
                binding->joints.push_back(joint);
                binding->inverseBind.push_back(inverseBind);
                totalInfluences += bone->mNumWeights;
                Require(totalInfluences <= MaxVertices * 8, "skin influence size limit exceeded");
                std::set<unsigned> weightedVertices;
                for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                    const auto& weight = bone->mWeights[w];
                    Require(weight.mVertexId < mesh->mNumVertices && std::isfinite(weight.mWeight) && weight.mWeight >= 0,
                            "invalid skin influence index or nonfinite/negative weight");
                    Require(weightedVertices.insert(weight.mVertexId).second, "duplicate bone influence for a vertex");
                    if (weight.mWeight > 0) influences[weight.mVertexId].push_back({palette, weight.mWeight});
                }
            }
            const auto rigid = static_cast<std::uint32_t>(binding->joints.size());
            binding->joints.push_back(static_cast<std::uint16_t>(nodeIndex));
            binding->inverseBind.push_back(Matrix::Identity);
            std::vector<bool> used(binding->joints.size(), false);
            const aiMaterial* material = mesh->mMaterialIndex < scene.mNumMaterials ? scene.mMaterials[mesh->mMaterialIndex] : nullptr;
            const int uvChannel = detail::UvChannelFor(*mesh, material ? detail::BaseColorUvChannel(*material) : 0);
            section.vertices.resize(mesh->mNumVertices);
            for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
                auto& weights = influences[v];
                std::stable_sort(weights.begin(), weights.end(), [](const auto& a, const auto& b) {
                    return a.weight != b.weight ? a.weight > b.weight : a.palette < b.palette;
                });
                if (weights.size() > 4) weights.resize(4);
                if (weights.empty()) weights.push_back({rigid, 1.f});
                double sum = 0;
                for (const auto& weight : weights) sum += weight.weight;
                auto& vertex = section.vertices[v];
                const auto& p = mesh->mVertices[v];
                const auto& n = mesh->mNormals[v];
                const auto uv = uvChannel >= 0 ? mesh->mTextureCoords[uvChannel][v] : aiVector3D{};
                Require(Finite(p) && Finite(n) && Finite(uv), "nonfinite skinned vertex");
                vertex.position = Vector4(p.x, p.y, p.z, 1.f);
                vertex.normal = Vector3(n.x, n.y, n.z);
                vertex.uv = Vector2(uv.x, uv.y);
                for (std::size_t i = 0; i < weights.size(); ++i) {
                    weights[i].weight = static_cast<float>(weights[i].weight / sum);
                    vertex.weights[i] = weights[i].weight;
                    if (weights[i].weight > 0) used[weights[i].palette] = true;
                }
            }
            // Prune bones unused after strongest-four reduction, retaining source
            // palette order and appending the rigid mesh-node entry only if needed.
            std::vector<std::uint16_t> remap(used.size(), 0);
            std::size_t count = 0;
            for (std::size_t i = 0; i < used.size(); ++i) {
                if (!used[i]) continue;
                Require(count < 256, "section uses more than 256 skin palette entries");
                remap[i] = static_cast<std::uint16_t>(count);
                binding->joints[count] = binding->joints[i];
                binding->inverseBind[count] = binding->inverseBind[i];
                ++count;
            }
            binding->joints.resize(count);
            binding->inverseBind.resize(count);
            binding->jointBounds.resize(count);
            std::vector<bool> jointBoundsInitialized(count, false);
            for (std::size_t b = 0; b < count; ++b) {
                const Matrix palette = binding->inverseBind[b] * skeleton.models[binding->joints[b]];
                // glTF skinning ignores the mesh node's global transform. Default
                // joint poses may differ from bind; only rigid entries use the
                // mesh node (through their identity inverse bind).
                Require(Near(palette, palette), "nonfinite default skin palette");
                binding->restPalette.push_back(palette);
            }
            for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
                auto& vertex = section.vertices[v];
                const Vector3 position(vertex.position.x, vertex.position.y, vertex.position.z);
                Vector3 restPosition{};
                for (std::size_t i = 0; i < influences[v].size(); ++i) {
                    const auto b = remap[influences[v][i].palette];
                    vertex.joints[i] = b;
                    if (vertex.weights[i] <= 0) continue;
                    restPosition += Vector3::Transform(position, binding->restPalette[b]) * vertex.weights[i];
                    bool initialized = jointBoundsInitialized[b];
                    Enclose(binding->jointBounds[b], Vector3::Transform(position, binding->inverseBind[b]), initialized);
                    jointBoundsInitialized[b] = initialized;
                }
                Enclose(result.bounds, restPosition, boundsInitialized);
            }
            for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
                const auto& face = mesh->mFaces[f];
                Require(face.mNumIndices == 3, "non-triangle survived Assimp triangulation");
                for (unsigned i = 0; i != 3; ++i) {
                    Require(face.mIndices[i] < mesh->mNumVertices, "invalid skinned triangle index");
                    section.indices.push_back(face.mIndices[i]);
                }
            }
            section.binding = std::move(binding);
            result.sections.push_back(std::move(section));
        }
    }
    Require(boundsInitialized && !result.sections.empty(), "source contains no skinned/rigid triangle instances");
    return result;
}
} // namespace

bool SkeletalAssetImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload,
                                   std::string& error) const {
    try {
        Require(kind_ == AssetKind::Skeleton || kind_ == AssetKind::AnimationClip || kind_ == AssetKind::SkinnedMesh,
                "invalid skeletal importer kind");
        Require(!desc.centerOnLoad, "skeletal conversion requires centerOnLoad=false");
        const auto document = Descriptor(AssetPathResolver::ToAbsolute(desc.path), kind_);
        Json skeletonDocument = document;
        AssetReference skeletonReference;
        if (kind_ != AssetKind::Skeleton) {
            skeletonReference = Reference(document.at("skeleton"));
            const auto& skeletonEntry = Resolve(desc, skeletonReference, CatalogAssetKind::Skeleton);
            skeletonDocument = Descriptor(skeletonEntry.source, AssetKind::Skeleton);
        }
        const auto sourceReference = Reference(skeletonDocument.at("source"));
        const auto& source = Resolve(desc, sourceReference, CatalogAssetKind::Mesh);
        Require(!source.metadata.centerOnLoad, "skeletal Mesh source metadata requires centerOnLoad=false");
        Require(LowerAscii(ToUtf8(source.source.extension())) == ".glb", "skeletal version 1 supports only self-contained .glb sources; export .gltf/other formats to GLB");
        std::vector<std::uint8_t> bytes;
        std::string readError;
        Require(ReadBinaryFile(source.source, bytes, readError, MaxSourceBytes), readError);
        const auto gltf = GlbJson(bytes);
        Assimp::Importer importer; // One independent scene/snapshot per worker; no registry waits.
        const auto* scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), ImportFlags, "glb");
        Require(scene && scene->mRootNode, "Assimp skeletal read failed: " + std::string(importer.GetErrorString()));
        Require((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) == 0, "Assimp returned an incomplete skeletal scene");
        SceneSkeleton skeleton;
        skeleton.Build(*scene, bytes, gltf);
        CpuPayload parsed;
        if (kind_ == AssetKind::Skeleton) {
            parsed.kind = CpuPayload::Kind::Skeleton;
            parsed.skeleton = {skeleton.data, skeleton.signature, static_cast<std::uint32_t>(skeleton.nodes.size())};
        } else if (kind_ == AssetKind::AnimationClip) {
            parsed.kind = CpuPayload::Kind::AnimationClip;
            Require(document.at("clip").is_string(), "clip must be an exact animation name string");
            parsed.animationClip = BuildClip(*scene, gltf, document.at("clip").get<std::string>(), skeleton);
        } else {
            parsed.kind = CpuPayload::Kind::SkinnedMesh;
            parsed.skinnedMesh = BuildMesh(*scene, skeleton, skeletonReference, gltf);
        }
        payload = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = "SkeletalAssetImporter: " + std::string(exception.what());
    } catch (...) {
        error = "SkeletalAssetImporter: unknown conversion failure";
    }
    return false;
}

bool SkeletalAssetImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                     GpuPayload& result, std::string& error) const {
    std::vector<SkinnedMeshSection> sections;
    BufferHandle vertexBuffer, indexBuffer;
    MeshHandle pendingMesh;
    const auto rollback = [&] {
        if (!transfer.resources) return;
        if (pendingMesh) transfer.resources->DestroyMesh(pendingMesh);
        if (vertexBuffer) transfer.resources->DestroyBuffer(vertexBuffer);
        if (indexBuffer) transfer.resources->DestroyBuffer(indexBuffer);
        for (const auto& section : sections) transfer.resources->DestroyMesh(section.mesh);
    };
    try {
        if ((kind_ == AssetKind::Skeleton && payload.kind == CpuPayload::Kind::Skeleton) ||
            (kind_ == AssetKind::AnimationClip && payload.kind == CpuPayload::Kind::AnimationClip)) {
            error.clear();
            return true;
        }
        Require(kind_ == AssetKind::SkinnedMesh && payload.kind == CpuPayload::Kind::SkinnedMesh,
                "skeletal upload payload kind mismatch");
        Require(transfer.resources != nullptr && !payload.skinnedMesh.sections.empty(), "missing skin upload resources/sections");
        sections.reserve(payload.skinnedMesh.sections.size());
        for (const auto& section : payload.skinnedMesh.sections) {
            Require(!section.vertices.empty() && !section.indices.empty() && section.binding,
                    "empty skinned upload section or missing binding");
            Require(section.vertices.size() <= MaxVertices && section.indices.size() <= MaxVertices * 6,
                    "skinned GPU buffer size limit exceeded");
            vertexBuffer = transfer.resources->CreateVertexBuffer<SkinnedVertex>(section.vertices);
            Require(bool(vertexBuffer), "skinned vertex buffer creation failed");
            indexBuffer = transfer.resources->CreateIndexBuffer<std::uint32_t>(section.indices);
            Require(bool(indexBuffer), "skinned index buffer creation failed");
            VertexLayout layout;
            layout.Add("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
            layout.Add("NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 16);
            layout.Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 28);
            layout.Add("BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 36);
            layout.Add("BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 44);
            pendingMesh = transfer.resources->CreateMesh(vertexBuffer, indexBuffer, std::move(layout),
                                                        static_cast<std::uint32_t>(section.indices.size()));
            Require(bool(pendingMesh), "skinned mesh registration failed");
            vertexBuffer = {};
            indexBuffer = {};
            sections.push_back({pendingMesh, section.materialSlot, section.binding});
            pendingMesh = {};
        }
        result.skinnedSections = std::move(sections);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        rollback();
        error = "SkeletalAssetImporter upload: " + std::string(exception.what());
    } catch (...) {
        rollback();
        error = "SkeletalAssetImporter upload: unknown failure";
    }
    return false;
}
} // namespace engine
