#include <engine/assets/AssetCooking.hpp>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/rendering/EngineShaders.hpp>
#include "../animation/AnimationData.hpp"
#include "MeshFinish.hpp"

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace engine {
namespace {

using DirectX::SimpleMath::Matrix;

static_assert(std::endian::native == std::endian::little, "cooked data is stored little-endian");
static_assert(std::is_trivially_copyable_v<ugly_utils::VertData>);
static_assert(std::is_trivially_copyable_v<SkinnedVertex>);
static_assert(std::is_trivially_copyable_v<MeshSectionCpu>);
static_assert(std::is_trivially_copyable_v<Aabb>);
static_assert(std::is_trivially_copyable_v<Matrix>);

// magic, format version, kind, payload size, payload hash
constexpr std::array<char, 8> Magic{'S', 'R', 'I', 'C', 'O', 'O', 'K', '\0'};
constexpr std::size_t HeaderSize = 32;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::uint8_t byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

    template <class T>
    void Value(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        out_.insert(out_.end(), bytes, bytes + sizeof(T));
    }

    void Count(std::size_t count) {
        Require(count <= std::numeric_limits<std::uint32_t>::max(), "too many elements to cook");
        Value(static_cast<std::uint32_t>(count));
    }

    template <class T>
    void Array(std::span<const T> values) {
        static_assert(std::is_trivially_copyable_v<T>);
        Count(values.size());
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
        out_.insert(out_.end(), bytes, bytes + values.size_bytes());
    }

    void Text(std::string_view text) {
        Array(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

private:
    std::vector<std::uint8_t>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    template <class T>
    T Value() {
        static_assert(std::is_trivially_copyable_v<T>);
        Need(sizeof(T));
        T value;
        std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    // Checked against the bytes left, so a damaged count never allocates.
    std::size_t Count(std::size_t elementSize) {
        const auto count = Value<std::uint32_t>();
        Require(elementSize == 0 || count <= (bytes_.size() - offset_) / elementSize, "cooked data is truncated");
        return count;
    }

    template <class T>
    std::vector<T> Array() {
        const auto count = Count(sizeof(T));
        std::vector<T> values(count);
        if (count != 0) std::memcpy(values.data(), bytes_.data() + offset_, count * sizeof(T));
        offset_ += count * sizeof(T);
        return values;
    }

    std::string Text() {
        const auto bytes = Array<char>();
        return {bytes.begin(), bytes.end()};
    }

    void End() const { Require(offset_ == bytes_.size(), "cooked data has trailing bytes"); }

private:
    void Need(std::size_t size) const {
        Require(size <= bytes_.size() - offset_, "cooked data is truncated");
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

template <class T>
std::vector<std::uint8_t> SaveOzz(const T& object) {
    ozz::io::MemoryStream stream;
    {
        ozz::io::OArchive archive(&stream);
        archive << object;
    }
    std::vector<std::uint8_t> bytes(stream.Size());
    stream.Seek(0, ozz::io::Stream::kSet);
    Require(stream.Read(bytes.data(), bytes.size()) == bytes.size(), "cannot read back the ozz archive");
    return bytes;
}

template <class T>
void LoadOzz(std::span<const std::uint8_t> bytes, T& object) {
    ozz::io::MemoryStream stream;
    Require(stream.Write(bytes.data(), bytes.size()) == bytes.size(), "cannot buffer the ozz archive");
    stream.Seek(0, ozz::io::Stream::kSet);
    ozz::io::IArchive archive(&stream);
    Require(archive.TestTag<T>(), "cooked ozz archive has the wrong type");
    archive >> object;
}

void WriteMesh(Writer& out, const MeshCpu& mesh) {
    out.Array(std::span<const ugly_utils::VertData>(mesh.vertices));
    out.Array(std::span<const std::uint32_t>(mesh.indices));
    out.Array(std::span<const MeshSectionCpu>(mesh.sections));
    out.Count(mesh.materialSlots.size());
    for (const auto& slot : mesh.materialSlots) out.Text(slot);
    out.Value(mesh.bounds);
}

MeshCpu ReadMesh(Reader& in) {
    MeshCpu mesh;
    mesh.vertices = in.Array<ugly_utils::VertData>();
    mesh.indices = in.Array<std::uint32_t>();
    mesh.sections = in.Array<MeshSectionCpu>();
    mesh.materialSlots.resize(in.Count(sizeof(std::uint32_t)));
    for (auto& slot : mesh.materialSlots) slot = in.Text();
    mesh.bounds = in.Value<Aabb>();
    Require(!mesh.vertices.empty() && !mesh.indices.empty() && mesh.indices.size() % 3 == 0,
            "cooked mesh has no triangles");
    for (const auto& section : mesh.sections) {
        Require(section.firstVertex <= mesh.vertices.size() &&
                section.vertexCount <= mesh.vertices.size() - section.firstVertex &&
                section.firstIndex <= mesh.indices.size() &&
                section.indexCount <= mesh.indices.size() - section.firstIndex &&
                section.materialSlot < mesh.materialSlots.size(), "cooked mesh section is out of range");
        for (std::uint32_t i = 0; i < section.indexCount; ++i) {
            const auto index = mesh.indices[section.firstIndex + i];
            Require(index >= section.firstVertex && index - section.firstVertex < section.vertexCount,
                    "cooked mesh index is outside its section");
        }
    }
    return mesh;
}

void WriteSkeleton(Writer& out, const SkeletonAsset& skeleton) {
    Require(skeleton.data != nullptr, "skeleton has no data");
    out.Value(skeleton.signature);
    out.Value(skeleton.jointCount);
    out.Count(skeleton.data->nodePaths.size());
    for (const auto& path : skeleton.data->nodePaths) out.Text(path);
    const auto archive = SaveOzz(skeleton.data->skeleton);
    out.Array(std::span<const std::uint8_t>(archive));
}

SkeletonAsset ReadSkeleton(Reader& in) {
    SkeletonAsset skeleton;
    skeleton.signature = in.Value<std::uint64_t>();
    skeleton.jointCount = in.Value<std::uint32_t>();
    auto data = std::make_shared<detail::SkeletonData>();
    data->nodePaths.resize(in.Count(sizeof(std::uint32_t)));
    for (auto& path : data->nodePaths) path = in.Text();
    LoadOzz(in.Array<std::uint8_t>(), data->skeleton);
    Require(data->skeleton.num_joints() > 0, "cooked skeleton has no joints");
    skeleton.data = std::move(data);
    return skeleton;
}

void WriteClip(Writer& out, const AnimationClipAsset& clip) {
    Require(clip.data != nullptr, "animation clip has no data");
    out.Value(clip.skeletonSignature);
    out.Value(clip.duration);
    const auto archive = SaveOzz(clip.data->animation);
    out.Array(std::span<const std::uint8_t>(archive));
}

AnimationClipAsset ReadClip(Reader& in) {
    AnimationClipAsset clip;
    clip.skeletonSignature = in.Value<std::uint64_t>();
    clip.duration = in.Value<float>();
    auto data = std::make_shared<detail::AnimationData>();
    LoadOzz(in.Array<std::uint8_t>(), data->animation);
    clip.data = std::move(data);
    return clip;
}

void WriteSkinnedMesh(Writer& out, const SkinnedMeshCpu& mesh) {
    out.Value(mesh.bounds);
    out.Value(mesh.skeletonSignature);
    out.Text(mesh.skeletonReference.id.IsValid() ? mesh.skeletonReference.id.ToString() : std::string{});
    out.Text(mesh.skeletonReference.path);
    out.Count(mesh.sections.size());
    for (const auto& section : mesh.sections) {
        Require(section.binding != nullptr, "skinned section has no binding");
        out.Array(std::span<const SkinnedVertex>(section.vertices));
        out.Array(std::span<const std::uint32_t>(section.indices));
        out.Value(section.materialSlot);
        out.Array(std::span<const std::uint16_t>(section.binding->joints));
        out.Array(std::span<const Matrix>(section.binding->inverseBind));
        out.Array(std::span<const Matrix>(section.binding->restPalette));
        out.Array(std::span<const Aabb>(section.binding->jointBounds));
    }
}

SkinnedMeshCpu ReadSkinnedMesh(Reader& in) {
    SkinnedMeshCpu mesh;
    mesh.bounds = in.Value<Aabb>();
    mesh.skeletonSignature = in.Value<std::uint64_t>();
    if (const auto id = in.Text(); !id.empty()) {
        mesh.skeletonReference.id = AssetUuid(id);
        Require(mesh.skeletonReference.id.IsValid(), "cooked skinned mesh has an invalid skeleton UUID");
    }
    mesh.skeletonReference.path = in.Text();
    mesh.sections.resize(in.Count(sizeof(std::uint32_t)));
    for (auto& section : mesh.sections) {
        section.vertices = in.Array<SkinnedVertex>();
        section.indices = in.Array<std::uint32_t>();
        section.materialSlot = in.Value<std::uint32_t>();
        auto binding = std::make_shared<SkinBinding>();
        binding->joints = in.Array<std::uint16_t>();
        binding->inverseBind = in.Array<Matrix>();
        binding->restPalette = in.Array<Matrix>();
        binding->jointBounds = in.Array<Aabb>();
        const auto palette = binding->joints.size();
        Require(binding->inverseBind.size() == palette && binding->restPalette.size() == palette &&
                binding->jointBounds.size() == palette, "cooked skin binding arrays differ in size");
        Require(!section.vertices.empty() && !section.indices.empty() && section.indices.size() % 3 == 0,
                "cooked skinned section has no triangles");
        for (const auto index : section.indices)
            Require(index < section.vertices.size(), "cooked skinned index is out of range");
        for (const auto& vertex : section.vertices)
            for (const auto joint : vertex.joints)
                Require(joint < palette, "cooked skinned vertex names a joint outside its palette");
        section.binding = std::move(binding);
    }
    Require(!mesh.sections.empty(), "cooked skinned mesh has no sections");
    return mesh;
}

void WriteHeader(std::vector<std::uint8_t>& result, AssetKind kind, const std::vector<std::uint8_t>& body) {
    result.clear();
    result.reserve(HeaderSize + body.size());
    Writer header(result);
    header.Value(Magic);
    header.Value(CookedFormatVersion);
    header.Value(static_cast<std::uint32_t>(kind));
    header.Value(static_cast<std::uint64_t>(body.size()));
    header.Value(Hash(body));
    result.insert(result.end(), body.begin(), body.end());
}

std::span<const std::uint8_t> ReadHeader(std::span<const std::uint8_t> bytes, AssetKind kind) {
    Require(bytes.size() >= HeaderSize, "not a cooked asset (too short)");
    Reader header(bytes.first(HeaderSize));
    Require(header.Value<std::array<char, 8>>() == Magic, "not a cooked asset");
    Require(header.Value<std::uint32_t>() == CookedFormatVersion,
            "cooked by a different engine version; build the game again");
    Require(header.Value<std::uint32_t>() == static_cast<std::uint32_t>(kind), "cooked asset has the wrong kind");
    const auto size = header.Value<std::uint64_t>();
    const auto hash = header.Value<std::uint64_t>();
    const auto body = bytes.subspan(HeaderSize);
    Require(size == body.size(), "cooked asset is truncated");
    Require(hash == Hash(body), "cooked asset is damaged (hash mismatch)");
    return body;
}

void KeyPart(std::string& key, std::string_view part) {
    key += std::to_string(part.size());
    key += ':';
    key += part;
}

CpuPayload::Kind PayloadKind(AssetKind kind) {
    switch (kind) {
    case AssetKind::Mesh: return CpuPayload::Kind::Mesh;
    case AssetKind::Skeleton: return CpuPayload::Kind::Skeleton;
    case AssetKind::AnimationClip: return CpuPayload::Kind::AnimationClip;
    case AssetKind::SkinnedMesh: return CpuPayload::Kind::SkinnedMesh;
    default: break;
    }
    throw std::runtime_error("this asset kind is not cooked");
}

} // namespace

bool IsCookedKind(AssetKind kind) {
    return kind == AssetKind::Mesh || kind == AssetKind::Skeleton || kind == AssetKind::AnimationClip ||
        kind == AssetKind::SkinnedMesh;
}

std::filesystem::path CookedPath(const std::filesystem::path& source) {
    auto path = source;
    path += L".cooked";
    return path;
}

bool SerializeCooked(AssetKind kind, const CpuPayload& payload, std::vector<std::uint8_t>& bytes,
                     std::string& error) {
    try {
        Require(payload.kind == PayloadKind(kind), "payload does not match the asset kind");
        std::vector<std::uint8_t> body;
        Writer out(body);
        switch (kind) {
        case AssetKind::Mesh: WriteMesh(out, payload.mesh); break;
        case AssetKind::Skeleton: WriteSkeleton(out, payload.skeleton); break;
        case AssetKind::AnimationClip: WriteClip(out, payload.animationClip); break;
        default: WriteSkinnedMesh(out, payload.skinnedMesh); break;
        }
        std::vector<std::uint8_t> result;
        WriteHeader(result, kind, body);
        bytes = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool DeserializeCooked(AssetKind kind, std::span<const std::uint8_t> bytes, CpuPayload& payload,
                       std::string& error) {
    try {
        Reader in(ReadHeader(bytes, kind));
        CpuPayload decoded;
        decoded.kind = PayloadKind(kind);
        switch (kind) {
        case AssetKind::Mesh: decoded.mesh = ReadMesh(in); break;
        case AssetKind::Skeleton: decoded.skeleton = ReadSkeleton(in); break;
        case AssetKind::AnimationClip: decoded.animationClip = ReadClip(in); break;
        default: decoded.skinnedMesh = ReadSkinnedMesh(in); break;
        }
        in.End();
        payload = std::move(decoded);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string ShaderLibrary::Key(const std::filesystem::path& file, ShaderType stage,
                               const ShaderCompileOptions& options) {
    const auto absolute = AssetPathResolver::ToAbsolute(file);
    const auto name = AssetPathResolver::ToName(absolute);
    auto defines = options.defines;
    std::ranges::sort(defines, {}, &ShaderDefine::name);
    std::string key;
    KeyPart(key, name.empty() ? ToGenericUtf8(absolute) : name);
    KeyPart(key, GetDX11Target(stage));
    KeyPart(key, options.entryPoint.empty() ? GetEntryPoint(stage) : options.entryPoint);
    for (const auto& define : defines) {
        KeyPart(key, define.name);
        KeyPart(key, define.value);
    }
    return key;
}

void ShaderLibrary::Add(std::string key, std::vector<std::uint8_t> bytecode) {
    shaders_.insert_or_assign(std::move(key), std::move(bytecode));
}

const std::vector<std::uint8_t>* ShaderLibrary::Find(const std::string& key) const {
    const auto it = shaders_.find(key);
    return it == shaders_.end() ? nullptr : &it->second;
}

bool ShaderLibrary::Serialize(std::vector<std::uint8_t>& bytes, std::string& error) const {
    try {
        std::vector<std::uint8_t> body;
        Writer out(body);
        out.Count(shaders_.size());
        for (const auto& [key, bytecode] : shaders_) {
            out.Text(key);
            out.Array(std::span<const std::uint8_t>(bytecode));
        }
        WriteHeader(bytes, AssetKind::Shader, body);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool ShaderLibrary::Deserialize(std::span<const std::uint8_t> bytes, std::string& error) {
    try {
        Reader in(ReadHeader(bytes, AssetKind::Shader));
        std::map<std::string, std::vector<std::uint8_t>> shaders;
        for (auto count = in.Count(2 * sizeof(std::uint32_t)); count != 0; --count) {
            auto key = in.Text();
            auto bytecode = in.Array<std::uint8_t>();
            Require(!bytecode.empty(), "shader library entry has no bytecode");
            Require(shaders.emplace(std::move(key), std::move(bytecode)).second, "shader library has a duplicate key");
        }
        in.End();
        shaders_ = std::move(shaders);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::shared_ptr<const ShaderLibrary> CookedShaderLibrary(std::string& error) {
    static std::mutex mutex;
    static std::filesystem::path loadedFor;
    static std::shared_ptr<const ShaderLibrary> library;
    static std::string loadError;
    const auto manifest = AssetPathResolver::GetManifest();
    if (manifest.empty()) return nullptr;
    const std::scoped_lock lock(mutex);
    if (manifest != loadedFor) {
        loadedFor = manifest;
        library.reset();
        std::vector<std::uint8_t> bytes;
        auto loaded = std::make_shared<ShaderLibrary>();
        const auto file = manifest.parent_path() / ShaderLibraryName;
        if (!ReadBinaryFile(file, bytes, loadError) || !loaded->Deserialize(bytes, loadError)) {
            loadError = ToUtf8(file) + ": " + loadError;
        } else {
            library = std::move(loaded);
        }
    }
    error = library ? std::string{} : loadError;
    return library;
}

bool CookShaderLibrary(const AssetCatalog& catalog, ShaderLibrary& library, std::vector<std::string>& errors) {
    const auto add = [&](const ShaderVariant& variant, const std::string& context) {
        std::string error;
        auto bytecode = ShaderContext::CompileForShipping(variant.file, variant.stage, variant.options, error);
        if (!bytecode) {
            errors.push_back(context + ": " + error);
            return;
        }
        library.Add(ShaderLibrary::Key(variant.file, variant.stage, variant.options), std::move(*bytecode));
    };
    for (std::size_t i = 0; i < static_cast<std::size_t>(EngineShader::Count); ++i) {
        auto variant = GetEngineShader(static_cast<EngineShader>(i));
        variant.file = AssetPathResolver::ResolveShader({.path = AssetPathResolver::DefaultPath(
            ToUtf8(variant.file), AssetKind::Shader)});
        add(variant, "engine render pass");
    }
    for (const auto* entry : catalog.List(CatalogAssetKind::Material)) {
        std::string text, error;
        MaterialDefinition definition;
        std::filesystem::path file;
        if (!ReadTextFile(entry->source, text, error) || !ParseMaterialDefinition(text, definition, error) ||
            (definition.hooks != MaterialHook::None && !MaterialShaderFile(definition, &catalog, file, error))) {
            errors.push_back(entry->path + ": " + error);
            continue;
        }
        for (const auto& variant : MaterialShaderVariants(file, definition.hooks)) {
            if (variant) add(*variant, entry->path);
        }
    }
    return errors.empty();
}

CookedAssetImporter::CookedAssetImporter(AssetKind kind, std::unique_ptr<IAssetImporter> uploader)
    : kind_(kind), uploader_(std::move(uploader)) {}

CookedAssetImporter::~CookedAssetImporter() = default;

bool CookedAssetImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload,
                                  std::string& error) const {
    if (kind_ != AssetKind::Mesh && desc.centerOnLoad) {
        error = "skeletal assets cannot be loaded with centerOnLoad";
        return false;
    }
    const auto source = kind_ == AssetKind::Mesh ? AssetPathResolver::ResolveMesh(desc)
                                                 : AssetPathResolver::ToAbsolute(desc.path);
    const auto file = CookedPath(source);
    std::vector<std::uint8_t> bytes;
    if (!ReadBinaryFile(file, bytes, error)) return false;
    CpuPayload decoded;
    if (!DeserializeCooked(kind_, bytes, decoded, error)) {
        error = ToUtf8(file) + ": " + error;
        return false;
    }
    if (kind_ == AssetKind::Mesh) detail::FinishMesh(decoded.mesh, desc.centerOnLoad);
    payload = std::move(decoded);
    return true;
}

bool CookedAssetImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                    GpuPayload& result, std::string& error) const {
    return uploader_->UploadGpu(transfer, payload, result, error);
}

} // namespace engine
