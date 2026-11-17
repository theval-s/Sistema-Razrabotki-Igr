#pragma once

#include <QString>
#include <map>
#include <compare>
#include <memory>
#include <string>
#include <vector>
#include <entt/signal/dispatcher.hpp>
#include <engine/assets/AssetEvents.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>

namespace engine { class Engine; }
class QObject;

namespace editor {

struct RenderingStatusText {
    QString summary;
    QString detail;
    bool warning = false;
    QString logSearch;
};

/// Presentation-only cache of failure text for current scene renderer requests.
/// No asset ownership, loading, catalog scans or GPU queries are performed here.
class RenderingStatus {
public:
    explicit RenderingStatus(engine::Engine& engine);
    ~RenderingStatus();
    RenderingStatusText describe(const engine::RenderingDiagnostics& diagnostics,
                                 bool startupFailed, bool playing, std::size_t submittedItems);
    void clear();

private:
    struct RequestKey {
        engine::AssetKind kind;
        std::uint32_t slot;
        std::uint32_t generation;
        auto operator<=>(const RequestKey&) const = default;
    };
    struct FailureText {
        QString name;
        QString error;
        std::uint64_t sequence = 0;
        bool claimed = false;
    };
    struct Request {
        RequestKey key;
        engine::AssetState state;
        std::string name;
        std::string identity;
        QString label;
    };
    std::vector<Request> requests();
    void prune(const std::vector<Request>& requests, bool dropUnclaimed = false);
    void schedulePrune();
    void failed(const engine::AssetFailedEvent& event);
    void loaded(const engine::AssetLoadedEvent& event);
    engine::Engine& engine_;
    // Source is implicit: only GetSceneAssets() events enter this cache.
    // Referenced entries are bounded by current renderer handles; pre-install
    // entries are capped and expire at the next event-loop turn.
    std::map<RequestKey, FailureText> failures_;
    std::unique_ptr<QObject> prune_context_;
    std::uint64_t event_sequence_ = 0;
    bool prune_scheduled_ = false;
    entt::scoped_connection failed_connection_;
    entt::scoped_connection loaded_connection_;
    std::size_t scene_observer_ = 0;
    std::size_t scene_change_observer_ = 0;
};

} // namespace editor
