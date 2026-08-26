#pragma once

#include <cstdint>

namespace cvr::camera {

// Publishes the actual pose from the render graph's authoritative MAIN view (view key 0).
// The callback receives REDengine fixed-point position followed by a world quaternion.
bool PublishActiveMainCamera(const float* renderCamera);

// Builds the other eye's absolute world pose from the same coherent MAIN snapshot. Position is
// returned in REDengine fixed point so PatchCamera can write it directly into component+0xE0.
bool ReadWorldVrcamPose(int32_t position[3], float orientation[4], uint64_t* sequence = nullptr);

} // namespace cvr::camera
