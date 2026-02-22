#pragma once

#include <vector>

#include "scene/SceneData.h"

namespace trace {

class BvhBuilder {
public:
    BvhBuildOutput build(const std::vector<TriangleDefinition>& triangles) const;
};

} // namespace trace
