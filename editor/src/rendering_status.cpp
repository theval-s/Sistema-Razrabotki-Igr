#include <editor/rendering_status.hpp>

#include <algorithm>
#include <type_traits>
#include <QStringList>
#include <QObject>
#include <QTimer>
#include <limits>
#include <editor/paths.hpp>
#include <engine/engine.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/scene/Scene.hpp>

namespace editor {
namespace {
QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString failureDetail(const engine::RenderingFailure& failure) {
    QStringList lines{text(failure.summary)};
    if (!failure.operation.empty()) lines << QStringLiteral("Operation: %1").arg(text(failure.operation));
    if (!failure.pass.empty()) lines << QStringLiteral("Pass: %1").arg(text(failure.pass));
    if (!failure.path.empty()) lines << QStringLiteral("Path: %1").arg(toQString(failure.path));
    const auto code = [](std::int64_t value) {
        return QStringLiteral("%1 (0x%2)").arg(value).arg(static_cast<quint32>(value), 8, 16, QLatin1Char('0'));
    };
    if (failure.nativeError) lines << QStringLiteral("Native error: %1").arg(code(*failure.nativeError));
    if (failure.deviceRemovalReason)
        lines << QStringLiteral("Device removal reason: %1").arg(code(*failure.deviceRemovalReason));
    if (failure.shader) {
        const auto& shader = *failure.shader;
        lines << QStringLiteral("Shader: %1 | entry %2 | target %3")
            .arg(text(shader.stage), text(shader.entryPoint), text(shader.target));
        for (const auto& [name, value] : shader.defines)
            lines << QStringLiteral("Define: %1=%2").arg(text(name), text(value));
    }
    if (!failure.detail.empty()) lines << text(failure.detail);
    return lines.join(u'\n');
}
} // namespace

RenderingStatus::RenderingStatus(engine::Engine& engine) : engine_(engine), prune_context_(std::make_unique<QObject>()) {
    failed_connection_ = engine.GetEvents().sink<engine::AssetFailedEvent>().connect<&RenderingStatus::failed>(*this);
    loaded_connection_ = engine.GetEvents().sink<engine::AssetLoadedEvent>().connect<&RenderingStatus::loaded>(*this);
    // Clear removal/replacement synchronously without walking the scene for every
    // transform edit. The entity UUID scopes even identical references to this scene.
    scene_observer_ = engine.GetScene().AddLifecycleObserver([this](auto event, auto entity, auto type) {
        const auto entityPrefix = engine_.GetScene().GetId(entity).ToString() + ":";
        const bool destroying = event == engine::Scene::LifecycleEvent::EntityDestroying;
        std::string prefix;
        if (destroying) prefix = entityPrefix;
        else if (type == entt::type_hash<engine::MeshRenderer>::value()) prefix = entityPrefix + "static:";
        else if (type == entt::type_hash<engine::SkinnedMeshRenderer>::value()) prefix = entityPrefix + "skinned:";
        else return;
        if (failures_.empty()) return;
        const auto current = requests();
        // The notification is before removal. Retain a shared request's detail
        // if another component still uses it, but never resurrect a removed one.
        for (const auto& request : current) {
            if (!request.identity.starts_with(prefix)) continue;
            const bool shared = std::ranges::any_of(current, [&](const auto& other) {
                return other.key == request.key && !other.identity.starts_with(prefix);
            });
            if (!shared) failures_.erase(request.key);
        }
    });
    scene_change_observer_ = engine.GetScene().AddChangeObserver([this](auto kind, auto) {
        if (kind == engine::Scene::ChangeKind::Destroyed && engine_.GetScene().GetRootEntities().empty()) clear();
    });
}

RenderingStatus::~RenderingStatus() {
    engine_.GetScene().RemoveLifecycleObserver(scene_observer_);
    engine_.GetScene().RemoveChangeObserver(scene_change_observer_);
}
void RenderingStatus::clear() { failures_.clear(); }

std::vector<RenderingStatus::Request> RenderingStatus::requests() {
    auto& assets = engine_.GetSceneAssets();
    std::vector<Request> result;
    const auto add = [&]<class T>(const std::string& owner, engine::AssetHandle<T> handle, engine::AssetReference reference,
                                 engine::AssetKind kind, engine::CatalogAssetKind catalogKind) {
        const RequestKey key{kind, handle.slot, handle.generation};
        if (!reference.id.IsValid() && reference.path.empty()) {
            if (auto recovered = assets.ReferenceFor(handle)) reference = *recovered;
        }
        const auto* entry = assets.GetCatalog().Find(reference.id, catalogKind);
        auto name = engine::NormalizeName(entry ? entry->path : reference.path);
        const auto identity = owner + std::to_string(static_cast<int>(kind)) + ":" + reference.id.ToString() + ":" +
            name + ":" + std::to_string(handle.slot) + ":" + std::to_string(handle.generation);
        QString label = text(entry ? entry->path : reference.path);
        if (label.isEmpty()) {
            if (const auto captured = failures_.find(key); captured != failures_.end()) {
                label = captured->second.name;
                name = label.toUtf8().toStdString();
            }
        }
        if (label.isEmpty()) label = handle
            ? QStringLiteral("Renderer asset request [slot %1, generation %2]").arg(handle.slot).arg(handle.generation)
            : QStringLiteral("Unassigned renderer asset");
        if (reference.id.IsValid()) label += QStringLiteral(" [UUID %1]").arg(text(reference.id.ToString()));
        if (reference.id.IsValid() && !entry) label += QStringLiteral(" — UUID is not resolved in the catalog");
        if (std::ranges::none_of(result, [&](const auto& request) { return request.identity == identity; }))
            result.push_back({key, assets.GetState(handle), name, identity, label});
    };
    const auto renderer = [&](auto entity, const auto& component) {
        using Mesh = std::decay_t<decltype(component.mesh)>;
        constexpr bool skinned = std::is_same_v<Mesh, engine::AssetHandle<engine::SkinnedMeshAsset>>;
        const auto owner = engine_.GetScene().GetId(entity).ToString() + (skinned ? ":skinned:" : ":static:");
        add(owner, component.mesh, component.meshReference,
            skinned ? engine::AssetKind::SkinnedMesh : engine::AssetKind::Mesh,
            skinned ? engine::CatalogAssetKind::SkinnedMesh : engine::CatalogAssetKind::Mesh);
        const auto material = [&](auto handle, const auto& reference) {
            add(owner, handle, reference, engine::AssetKind::MaterialInstance, engine::CatalogAssetKind::MaterialInstance);
        };
        if constexpr (skinned) {
            material(component.materialInstance, component.materialReference);
        } else if (const auto mesh = assets.Peek(component.mesh)) {
            auto meshReference = component.meshReference;
            if (!meshReference.id.IsValid()) {
                if (const auto recovered = assets.ReferenceFor(component.mesh)) meshReference = *recovered;
            }
            const auto* entry = assets.GetCatalog().Find(meshReference.id, engine::CatalogAssetKind::Mesh);
            for (const auto& section : mesh->sections) {
                const auto slot = section.materialSlot;
                // Keep this selection identical to SectionMaterial, including named
                // but failed overrides: they must not fall through to a default.
                if (slot < component.slotMaterials.size() && engine::HasOwnMaterial(component.slotMaterials[slot])) {
                    const auto& override = component.slotMaterials[slot];
                    material(override.materialInstance, override.materialReference);
                } else if (engine::HasOwnMaterial(component)) {
                    material(component.materialInstance, component.materialReference);
                } else {
                    const auto handle = slot < mesh->materials.size() ? mesh->materials[slot]
                        : engine::AssetHandle<engine::MaterialInstanceAsset>{};
                    // A failed request can retire before we inspect it. Recover its
                    // authored label from the mesh metadata, without requesting it again.
                    auto reference = assets.ReferenceFor(handle).value_or(engine::AssetReference{});
                    if (!reference.id.IsValid() && entry && slot < entry->metadata.materials.size() &&
                        entry->metadata.materials[slot]) reference = *entry->metadata.materials[slot];
                    material(handle, reference);
                }
            }
        } else if (engine::HasOwnMaterial(component)) {
            // Until the mesh is ready its used slots/defaults are unknown. The
            // renderer-wide request is still useful, but an empty override is normal.
            material(component.materialInstance, component.materialReference);
        }
    };
    engine_.GetScene().ForEach<engine::MeshRenderer>(renderer);
    engine_.GetScene().ForEach<engine::SkinnedMeshRenderer>(renderer);
    return result;
}

void RenderingStatus::prune(const std::vector<Request>& current, bool dropUnclaimed) {
    for (auto it = failures_.begin(); it != failures_.end();) {
        const bool referenced = std::ranges::any_of(current, [&](const auto& request) {
            return request.key == it->first && request.state == engine::AssetState::Failed;
        });
        if (referenced) {
            it->second.claimed = true;
            ++it;
        } else if (it->second.claimed || dropUnclaimed) it = failures_.erase(it);
        else ++it;
    }
}

void RenderingStatus::schedulePrune() {
    if (prune_scheduled_) return;
    prune_scheduled_ = true;
    QTimer::singleShot(0, prune_context_.get(), [this] {
        prune_scheduled_ = false;
        prune(requests(), true);
    });
}

void RenderingStatus::failed(const engine::AssetFailedEvent& event) {
    if (event.source != &engine_.GetSceneAssets() || event.slot == std::numeric_limits<std::uint32_t>::max()) return;
    if (event.kind != engine::AssetKind::Mesh && event.kind != engine::AssetKind::SkinnedMesh &&
        event.kind != engine::AssetKind::MaterialInstance) return;
    const RequestKey key{event.kind, event.slot, event.generation};
    failures_.try_emplace(key, FailureText{text(event.name), text(event.error), ++event_sequence_, false});
    prune(requests());
    constexpr std::size_t MaxUnclaimed = 256;
    std::size_t unclaimed = 0;
    for (const auto& entry : failures_) if (!entry.second.claimed) ++unclaimed;
    while (unclaimed > MaxUnclaimed) {
        auto oldest = failures_.end();
        for (auto it = failures_.begin(); it != failures_.end(); ++it) {
            if (!it->second.claimed && (oldest == failures_.end() || it->second.sequence < oldest->second.sequence)) oldest = it;
        }
        failures_.erase(oldest);
        --unclaimed;
    }
    schedulePrune();
}

void RenderingStatus::loaded(const engine::AssetLoadedEvent& event) {
    if (event.source != &engine_.GetSceneAssets() || event.slot == std::numeric_limits<std::uint32_t>::max()) return;
    if (event.kind != engine::AssetKind::Mesh && event.kind != engine::AssetKind::SkinnedMesh &&
        event.kind != engine::AssetKind::MaterialInstance) return;
    failures_.erase(RequestKey{event.kind, event.slot, event.generation});
    prune(requests());
}

RenderingStatusText RenderingStatus::describe(const engine::RenderingDiagnostics& diagnostics,
                                            bool startupFailed, bool playing, std::size_t submittedItems) {
    using engine::RenderingLifecycle;
    QStringList catalogErrors;
    if (!engine_.GetSceneAssets().GetCatalogError().empty())
        catalogErrors << text(engine_.GetSceneAssets().GetCatalogError());
    if (!engine_.GetAssets().GetCatalogError().empty() &&
        engine_.GetAssets().GetCatalogError() != engine_.GetSceneAssets().GetCatalogError())
        catalogErrors << text(engine_.GetAssets().GetCatalogError());
    const QString catalogDetail = catalogErrors.isEmpty() ? QString()
        : QStringLiteral("Asset catalog errors (independent of renderer failure):\n%1").arg(catalogErrors.join(u'\n'));
    if (diagnostics.lifecycle == RenderingLifecycle::Failed) {
        if (!diagnostics.failure) return {QStringLiteral("Renderer failed — Show Log for details"), catalogDetail, true};
        const auto& failure = *diagnostics.failure;
        QString summary = QStringLiteral("Renderer failed: %1").arg(text(failure.summary));
        if (!failure.pass.empty()) summary += QStringLiteral(" | %1").arg(text(failure.pass));
        if (!failure.operation.empty()) summary += QStringLiteral(" | %1").arg(text(failure.operation));
        if (!failure.path.empty()) summary += QStringLiteral(" | %1").arg(toQString(failure.path));
        QString detail = failureDetail(failure);
        if (!catalogDetail.isEmpty()) detail += QStringLiteral("\n\n") + catalogDetail;
        return {summary, detail, true,
                failure.path.empty() ? text(failure.summary) : toQString(failure.path)};
    }
    if (!catalogErrors.isEmpty())
        return {QStringLiteral("Asset catalog error — see Details / Log"), catalogErrors.join(u'\n'), true,
                catalogErrors.front().section(u'\n', 0, 0)};
    if (diagnostics.lifecycle == RenderingLifecycle::SurfaceUnavailable)
        return {QStringLiteral("Rendering suspended — viewport surface unavailable"),
                QStringLiteral("A minimized or zero-sized viewport cannot render. This is not a renderer failure."), false};
    if (startupFailed)
        return {QStringLiteral("Engine startup failed — Show Log for the failing subsystem"),
                QStringLiteral("Engine initialization did not complete. No engine tick is running."), true};
    if (diagnostics.lifecycle != RenderingLifecycle::Ready)
        return {QStringLiteral("Renderer not initialized"), {}, false};

    const auto current = requests();
    prune(current);
    int pending = 0, failedCount = 0;
    QString logSearch;
    QStringList details;
    for (const auto& request : current) {
        if (request.state == engine::AssetState::Loading || request.state == engine::AssetState::Unloaded) {
            ++pending;
            details << QStringLiteral("Preparing: %1").arg(request.label);
        } else if (request.state == engine::AssetState::Failed) {
            ++failedCount;
            if (logSearch.isEmpty()) logSearch = text(request.name);
            const auto failure = failures_.find(request.key);
            details << QStringLiteral("Failed / unavailable: %1\n%2").arg(request.label,
                failure == failures_.end() ? QStringLiteral("No retained event detail; see the existing Log for the original request error.")
                    : QStringLiteral("Request: %1 [slot %2, generation %3]\n%4")
                        .arg(failure->second.name).arg(request.key.slot).arg(request.key.generation).arg(failure->second.error));
        }
    }
    const bool noCamera = engine_.GetScene().GetActiveCamera() == entt::null;
    const QString camera = playing
        ? QStringLiteral("No active scene camera in Play: rendering keeps the current RenderWorld camera (the last camera, or its default).")
        : QStringLiteral("No active scene camera: Edit uses the editor camera.");
    details << QStringLiteral("%1 submitted drawable items (skinned sections can submit multiple items). This count does not measure on-screen visibility.")
        .arg(submittedItems);
    if (noCamera) details << camera;
    QString summary;
    if (failedCount) summary = QStringLiteral("%1 scene renderer asset references failed / unavailable").arg(failedCount);
    if (pending) {
        if (!summary.isEmpty()) summary += QStringLiteral(" | ");
        summary += QStringLiteral("Preparing %1 scene renderer asset references").arg(pending);
    }
    if (summary.isEmpty() && submittedItems == 0) summary = QStringLiteral("No drawable scene items submitted");
    if (summary.isEmpty() && noCamera && playing) summary = QStringLiteral("Play: using retained/default camera");
    return {summary, details.join(u'\n'), failedCount != 0, logSearch};
}

} // namespace editor
