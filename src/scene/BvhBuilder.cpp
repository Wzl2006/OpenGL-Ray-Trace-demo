#include "scene/BvhBuilder.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <numeric>

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace trace {

namespace {

struct BBox {
    glm::vec3 bmin = glm::vec3(std::numeric_limits<float>::max());
    glm::vec3 bmax = glm::vec3(std::numeric_limits<float>::lowest());

    void expand(const glm::vec3& p) {
        bmin = glm::min(bmin, p);
        bmax = glm::max(bmax, p);
    }

    void expand(const BBox& other) {
        bmin = glm::min(bmin, other.bmin);
        bmax = glm::max(bmax, other.bmax);
    }

    glm::vec3 extent() const {
        return bmax - bmin;
    }
};

struct Primitive {
    int index = -1;
    BBox bounds;
    glm::vec3 centroid = glm::vec3(0.0f);
};

struct NodeTemp {
    BBox bounds;
    int left = -1;
    int right = -1;
    int first = -1;
    int count = 0;
};

GpuTriangle toGpuTriangle(const TriangleDefinition& tri) {
    const glm::vec3 normal = glm::normalize(glm::cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
    GpuTriangle gpu{};
    gpu.v0 = glm::vec4(tri.v0, 1.0f);
    gpu.v1 = glm::vec4(tri.v1, 1.0f);
    gpu.v2 = glm::vec4(tri.v2, 1.0f);
    gpu.normalMaterial = glm::vec4(normal, static_cast<float>(tri.materialIndex));
    return gpu;
}

} // namespace

BvhBuildOutput BvhBuilder::build(const std::vector<TriangleDefinition>& triangles) const {
    BvhBuildOutput output;
    if (triangles.empty()) {
        return output;
    }

    std::vector<Primitive> primitives;
    primitives.reserve(triangles.size());
    for (int i = 0; i < static_cast<int>(triangles.size()); ++i) {
        const TriangleDefinition& tri = triangles[static_cast<size_t>(i)];
        Primitive p{};
        p.index = i;
        p.bounds.expand(tri.v0);
        p.bounds.expand(tri.v1);
        p.bounds.expand(tri.v2);
        p.centroid = (tri.v0 + tri.v1 + tri.v2) / 3.0f;
        primitives.push_back(p);
    }

    std::vector<int> indices(primitives.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::vector<NodeTemp> nodes;
    std::vector<GpuTriangle> orderedTriangles;
    orderedTriangles.reserve(triangles.size());

    const int leafMax = 2;

    std::function<int(int, int)> buildRecursive = [&](int begin, int end) -> int {
        NodeTemp node{};
        BBox centroidBounds;
        for (int i = begin; i < end; ++i) {
            const Primitive& p = primitives[static_cast<size_t>(indices[static_cast<size_t>(i)])];
            node.bounds.expand(p.bounds);
            centroidBounds.expand(p.centroid);
        }

        const int nodeIndex = static_cast<int>(nodes.size());
        nodes.push_back(node);

        const int count = end - begin;
        if (count <= leafMax) {
            nodes[static_cast<size_t>(nodeIndex)].first = static_cast<int>(orderedTriangles.size());
            nodes[static_cast<size_t>(nodeIndex)].count = count;
            for (int i = begin; i < end; ++i) {
                const int primitiveIndex = indices[static_cast<size_t>(i)];
                orderedTriangles.push_back(toGpuTriangle(triangles[static_cast<size_t>(primitives[static_cast<size_t>(primitiveIndex)].index)]));
            }
            return nodeIndex;
        }

        const glm::vec3 extents = centroidBounds.extent();
        int axis = 0;
        if (extents.y > extents.x) {
            axis = 1;
        }
        if (extents.z > extents[axis]) {
            axis = 2;
        }

        const int mid = (begin + end) / 2;
        std::nth_element(indices.begin() + begin, indices.begin() + mid, indices.begin() + end, [&](int lhs, int rhs) {
            return primitives[static_cast<size_t>(lhs)].centroid[axis] < primitives[static_cast<size_t>(rhs)].centroid[axis];
        });

        const int left = buildRecursive(begin, mid);
        const int right = buildRecursive(mid, end);
        nodes[static_cast<size_t>(nodeIndex)].left = left;
        nodes[static_cast<size_t>(nodeIndex)].right = right;
        nodes[static_cast<size_t>(nodeIndex)].first = -1;
        nodes[static_cast<size_t>(nodeIndex)].count = 0;
        return nodeIndex;
    };

    buildRecursive(0, static_cast<int>(indices.size()));

    output.nodes.reserve(nodes.size());
    for (const NodeTemp& n : nodes) {
        GpuBvhNode gpu{};
        gpu.bboxMin = glm::vec4(n.bounds.bmin, 0.0f);
        gpu.bboxMax = glm::vec4(n.bounds.bmax, 0.0f);
        gpu.meta = glm::ivec4(n.left, n.right, n.first, n.count);
        output.nodes.push_back(gpu);
    }
    output.triangles = std::move(orderedTriangles);
    return output;
}

} // namespace trace
