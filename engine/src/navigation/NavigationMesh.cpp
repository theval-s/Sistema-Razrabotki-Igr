#include <engine/navigation/NavigationMesh.hpp>

#include <Recast.h>
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>

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
    std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)> mesh{dtAllocNavMesh(), dtFreeNavMesh};
    std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)> query{dtAllocNavMeshQuery(), dtFreeNavMeshQuery};
};

NavigationMesh::NavigationMesh() = default;
NavigationMesh::~NavigationMesh() = default;

bool NavigationMesh::Build(std::span<const Vector3> vertices, std::span<const int> indices) {
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

    constexpr float agentHeight = 2.0f;
    constexpr float agentRadius = 0.4f;
    constexpr float agentClimb = 0.4f;
    rcConfig config{};
    config.cs = 0.2f;
    config.ch = 0.1f;
    config.walkableSlopeAngle = 45.0f;
    config.walkableHeight = static_cast<int>(std::ceil(agentHeight / config.ch));
    config.walkableClimb = static_cast<int>(std::floor(agentClimb / config.ch));
    config.walkableRadius = static_cast<int>(std::ceil(agentRadius / config.cs));
    config.maxEdgeLen = 60;
    config.maxSimplificationError = 1.3f;
    config.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    rcCalcBounds(positions.data(), vertexCount, config.bmin, config.bmax);
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
    std::fill_n(polygons->flags, polygons->npolys, 1);

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

    auto next = std::make_unique<Impl>();
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
    impl_ = std::move(next);
    return true;
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
