#include <engine/navigation/NavigationMesh.hpp>

#include <engine/rendering/DebugDraw.hpp>
#include <Recast.h>
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourCrowd.h>
#include <engine/scene/Scene.hpp>
#include <unordered_map>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine {
namespace {
using DirectX::SimpleMath::Vector3;

bool IsFinite(Vector3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

constexpr int QueryCapacity = 2048;
}

struct NavigationMesh::Impl {
    NavigationSettings settings;
    std::unordered_map<EntityUuid, int, EntityUuidHash> agents;
    std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)> mesh{dtAllocNavMesh(), dtFreeNavMesh};
    std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)> query{dtAllocNavMeshQuery(), dtFreeNavMeshQuery};
    std::unique_ptr<dtCrowd, decltype(&dtFreeCrowd)> crowd{nullptr, dtFreeCrowd};
};

NavigationMesh::NavigationMesh() = default;
NavigationMesh::~NavigationMesh() = default;

bool NavigationMesh::Build(std::span<const Vector3> vertices, std::span<const int> indices, const NavigationSettings& settings) {
    if (vertices.empty() || !std::ranges::all_of(vertices, IsFinite)) return false;
    Vector3 minimum = vertices.front(), maximum = vertices.front();
    for (const auto vertex : vertices) {
        minimum = Vector3::Min(minimum, vertex);
        maximum = Vector3::Max(maximum, vertex);
    }
    return BuildRegion(vertices, indices, settings, minimum, maximum);
}

bool NavigationMesh::BuildRegion(std::span<const Vector3> vertices, std::span<const int> indices,
                                 const NavigationSettings& settings, Vector3 minimum, Vector3 maximum) {
    if (!IsFinite(minimum) || !IsFinite(maximum) ||
        maximum.x <= minimum.x || maximum.z <= minimum.z || maximum.y < minimum.y) return false;
    constexpr auto maxCount = static_cast<std::size_t>((std::numeric_limits<int>::max)() / 3);
    if (vertices.size() < 3 || vertices.size() > maxCount || indices.empty() ||
        indices.size() > maxCount || indices.size() % 3 != 0 ||
        !std::ranges::all_of(vertices, IsFinite) ||
        !std::ranges::all_of(indices, [vertices](int index) {
            return index >= 0 && static_cast<std::size_t>(index) < vertices.size();
        })) {
        return false;
    }

    std::vector<float> positions;
    positions.reserve(vertices.size() * 3);
    for (const Vector3 vertex : vertices) {
        positions.insert(positions.end(), {vertex.x, vertex.y, vertex.z});
    }
    const int vertexCount = static_cast<int>(vertices.size());
    const int triangleCount = static_cast<int>(indices.size() / 3);

    const auto [cellSize, cellHeight, agentHeight, agentRadius, agentClimb, agentSlope] = settings;
    if (!(cellSize >= 0.01f && cellSize <= 10.0f && cellHeight >= 0.01f && cellHeight <= 10.0f &&
          agentHeight >= 3 * cellHeight && agentHeight / cellHeight <= 255 &&
          agentRadius >= 0 && agentRadius / cellSize <= 127 &&
          agentClimb >= 0 && agentClimb / cellHeight <= 255 && agentSlope >= 0 && agentSlope < 90)) {
        return false;
    }
    rcConfig config{};
    config.cs = cellSize;
    config.ch = cellHeight;
    config.walkableSlopeAngle = agentSlope;
    config.walkableHeight = static_cast<int>(std::ceil(agentHeight / config.ch));
    config.walkableClimb = static_cast<int>(std::floor(agentClimb / config.ch));
    config.walkableRadius = static_cast<int>(std::ceil(agentRadius / config.cs));
    config.maxEdgeLen = 60;
    config.maxSimplificationError = 1.3f;
    config.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    config.bmin[0] = minimum.x;
    config.bmin[1] = minimum.y;
    config.bmin[2] = minimum.z;
    config.bmax[0] = maximum.x;
    config.bmax[1] = maximum.y;
    config.bmax[2] = maximum.z;
    if (config.bmax[0] - config.bmin[0] > 2000.0f ||
        config.bmax[2] - config.bmin[2] > 2000.0f ||
        config.bmax[1] - config.bmin[1] > RC_SPAN_MAX_HEIGHT * config.ch) {
        return false;
    }
    config.bmax[1] = (std::max)(config.bmax[1], config.bmin[1] + config.ch);
    rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width, &config.height);
    if (config.width <= 0 || config.height <= 0 ||
        static_cast<long long>(config.width) * config.height > 4000000) {
        return false;
    }

    rcContext context(false);
    std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)> heightfield{rcAllocHeightfield(), rcFreeHeightField};
    if (!heightfield || !rcCreateHeightfield(&context, *heightfield, config.width, config.height,
                                           config.bmin, config.bmax, config.cs, config.ch)) {
        return false;
    }
    std::vector<unsigned char> areas(triangleCount, RC_NULL_AREA);
    rcMarkWalkableTriangles(&context, config.walkableSlopeAngle, positions.data(), vertexCount,
                            indices.data(), triangleCount, areas.data());
    if (!rcRasterizeTriangles(&context, positions.data(), vertexCount, indices.data(), areas.data(),
                              triangleCount, *heightfield, config.walkableClimb)) {
        return false;
    }
    rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *heightfield);
    rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, *heightfield);
    rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *heightfield);

    std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)>
        compact{rcAllocCompactHeightfield(), rcFreeCompactHeightfield};
    std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)> contours{rcAllocContourSet(), rcFreeContourSet};
    std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)> polygons{rcAllocPolyMesh(), rcFreePolyMesh};
    std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)>
        detail{rcAllocPolyMeshDetail(), rcFreePolyMeshDetail};
    if (!compact || !contours || !polygons || !detail ||
        !rcBuildCompactHeightfield(&context, config.walkableHeight, config.walkableClimb, *heightfield, *compact) ||
        !rcErodeWalkableArea(&context, config.walkableRadius, *compact) ||
        !rcBuildDistanceField(&context, *compact) ||
        !rcBuildRegions(&context, *compact, 0, 0, 0) ||
        !rcBuildContours(&context, *compact, config.maxSimplificationError, config.maxEdgeLen, *contours) ||
        !rcBuildPolyMesh(&context, *contours, config.maxVertsPerPoly, *polygons) ||
        polygons->npolys == 0 ||
        !rcBuildPolyMeshDetail(&context, *polygons, *compact, 1.2f, 0.1f, *detail)) {
        return false;
    }
    std::fill_n(polygons->flags, polygons->npolys, static_cast<unsigned short>(1));

    dtNavMeshCreateParams params{};
    params.verts = polygons->verts;
    params.vertCount = polygons->nverts;
    params.polys = polygons->polys;
    params.polyAreas = polygons->areas;
    params.polyFlags = polygons->flags;
    params.polyCount = polygons->npolys;
    params.nvp = polygons->nvp;
    params.detailMeshes = detail->meshes;
    params.detailVerts = detail->verts;
    params.detailVertsCount = detail->nverts;
    params.detailTris = detail->tris;
    params.detailTriCount = detail->ntris;
    params.walkableHeight = agentHeight;
    params.walkableRadius = agentRadius;
    params.walkableClimb = agentClimb;
    params.cs = config.cs;
    params.ch = config.ch;
    rcVcopy(params.bmin, polygons->bmin);
    rcVcopy(params.bmax, polygons->bmax);
    params.buildBvTree = true;

    auto next = std::make_shared<Impl>();
    unsigned char* data = nullptr;
    int dataSize = 0;
    if (!next->mesh || !next->query || !dtCreateNavMeshData(&params, &data, &dataSize)) {
        return false;
    }
    if (dtStatusFailed(next->mesh->init(data, dataSize, DT_TILE_FREE_DATA))) {
        dtFree(data);
        return false;
    }
    if (dtStatusFailed(next->query->init(next->mesh.get(), QueryCapacity))) {
        return false;
    }
    next->settings = settings;
    impl_ = std::move(next);
    return true;
}

bool NavigationMesh::RegisterAgent(EntityUuid agent, Vector3 position, float speed) const {
    if (!impl_ || !agent.IsValid() || !IsFinite(position) || !std::isfinite(speed) ||
        speed <= 0 || speed > 100 || impl_->settings.agentRadius <= 0) return false;
    if (impl_->agents.contains(agent)) return true;
    if (!impl_->crowd) {
        std::unique_ptr<dtCrowd, decltype(&dtFreeCrowd)> crowd{dtAllocCrowd(), dtFreeCrowd};
        if (!crowd || !crowd->init(128, impl_->settings.agentRadius, impl_->mesh.get())) return false;
        impl_->crowd = std::move(crowd);
    }
    dtCrowdAgentParams params{};
    params.radius = impl_->settings.agentRadius;
    params.height = impl_->settings.agentHeight;
    params.maxAcceleration = 8.0f;
    params.maxSpeed = speed;
    params.collisionQueryRange = params.radius * 12;
    params.pathOptimizationRange = params.radius * 30;
    params.separationWeight = 2.0f;
    params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OBSTACLE_AVOIDANCE |
        DT_CROWD_SEPARATION | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
    const float point[]{position.x, position.y, position.z};
    const int index = impl_->crowd->addAgent(point, &params);
    if (index < 0) return false;
    if (impl_->crowd->getAgent(index)->state == DT_CROWDAGENT_STATE_INVALID) {
        impl_->crowd->removeAgent(index);
        return false;
    }
    impl_->agents.emplace(agent, index);
    return true;
}

bool NavigationMesh::SetAgentGoal(EntityUuid agent, Vector3 goal) const {
    if (!impl_ || !IsFinite(goal)) return false;
    const auto found = impl_->agents.find(agent);
    if (found == impl_->agents.end()) return false;
    const float point[]{goal.x, goal.y, goal.z};
    float nearest[3];
    dtPolyRef polygon = 0;
    return dtStatusSucceed(impl_->query->findNearestPoly(point, impl_->crowd->getQueryExtents(),
        impl_->crowd->getFilter(0), &polygon, nearest)) && polygon &&
        impl_->crowd->requestMoveTarget(found->second, polygon, nearest);
}

std::optional<Vector3> NavigationMesh::GetAgentPosition(EntityUuid agent) const {
    if (!impl_) return std::nullopt;
    const auto found = impl_->agents.find(agent);
    if (found == impl_->agents.end()) return std::nullopt;
    const auto* position = impl_->crowd->getAgent(found->second)->npos;
    return Vector3{position[0], position[1], position[2]};
}

void NavigationMesh::RemoveAgent(EntityUuid agent) const {
    if (!impl_) return;
    const auto found = impl_->agents.find(agent);
    if (found == impl_->agents.end()) return;
    impl_->crowd->removeAgent(found->second);
    impl_->agents.erase(found);
}

void NavigationMesh::UpdateCrowd(float dt, const Scene& scene) const {
    if (!impl_ || !impl_->crowd) return;
    std::erase_if(impl_->agents, [&](const auto& entry) {
        if (scene.FindByUuid(entry.first) != entt::null) return false;
        impl_->crowd->removeAgent(entry.second);
        return true;
    });
    if (!impl_->agents.empty()) impl_->crowd->update(dt, nullptr);
}

void NavigationMesh::ClearCrowd() const {
    if (!impl_) return;
    impl_->agents.clear();
    impl_->crowd.reset();
}

void NavigationMesh::Clear() {
    impl_.reset();
}

void NavigationMesh::Draw(DebugDraw& draw) const {
    if (!impl_) return;
    const dtNavMesh& mesh = *impl_->mesh;
    const auto* tile = mesh.getTile(0);
    if (!tile || !tile->header) return;
    const DirectX::SimpleMath::Color color{0.15f, 0.85f, 0.8f, 1.0f};
    for (int i = 0; i < tile->header->polyCount; ++i) {
        const auto& poly = tile->polys[i];
        for (int j = 0; j < poly.vertCount; ++j) {
            const auto* a = tile->verts + 3 * poly.verts[j];
            const auto* b = tile->verts + 3 * poly.verts[(j + 1) % poly.vertCount];
            draw.Line({a[0], a[1] + 0.03f, a[2]}, {b[0], b[1] + 0.03f, b[2]}, color);
        }
    }
}

std::vector<Vector3> NavigationMesh::FindPath(Vector3 start, Vector3 goal) const {
    if (!impl_ || !IsFinite(start) || !IsFinite(goal)) {
        return {};
    }

    dtQueryFilter filter;
    constexpr float extents[]{1.0f, 2.0f, 1.0f};
    const float startPosition[]{start.x, start.y, start.z};
    const float goalPosition[]{goal.x, goal.y, goal.z};
    float nearestStart[3], nearestGoal[3];
    dtPolyRef startPoly = 0, goalPoly = 0;
    const auto& query = *impl_->query;
    if (dtStatusFailed(query.findNearestPoly(startPosition, extents, &filter, &startPoly, nearestStart)) ||
        dtStatusFailed(query.findNearestPoly(goalPosition, extents, &filter, &goalPoly, nearestGoal)) ||
        !startPoly || !goalPoly) {
        return {};
    }

    std::array<dtPolyRef, QueryCapacity> corridor;
    int polygonCount = 0;
    const dtStatus status = query.findPath(startPoly, goalPoly, nearestStart, nearestGoal, &filter,
                                          corridor.data(), &polygonCount, QueryCapacity);
    if (dtStatusFailed(status) || polygonCount == 0 || corridor[polygonCount - 1] != goalPoly) {
        return {};
    }

    std::array<float, 3 * QueryCapacity> points;
    int pointCount = 0;
    const dtStatus straightStatus = query.findStraightPath(nearestStart, nearestGoal, corridor.data(),
        polygonCount, points.data(), nullptr, nullptr, &pointCount, QueryCapacity);
    if (dtStatusFailed(straightStatus) || dtStatusDetail(straightStatus, DT_BUFFER_TOO_SMALL)) {
        return {};
    }
    std::vector<Vector3> path;
    path.reserve(pointCount);
    for (int i = 0; i < pointCount; ++i) {
        path.emplace_back(points[3 * i], points[3 * i + 1], points[3 * i + 2]);
    }
    return path;
}

}
