// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "RenderCameraFactory.h"

#include <memory>
#include <stdexcept>

#include "OpenCvRadtanCamera.h"
#include "TsaiPinholeCamera.h"

namespace Isis::SmallBodyRender {
namespace {

std::unique_ptr<RenderCamera> loadCheckedTsai(
    const FrameState &frame, const DistortionInverseControls &inverseControls,
    CameraModelFormat expectedFormat) {
  auto camera = std::make_unique<TsaiPinholeCamera>(
      TsaiPinholeCamera::readCanonical(frame.cameraPath, inverseControls));
  const CameraModelFormat actualFormat = camera->cameraModelFormat();
  if (actualFormat != expectedFormat) {
    const char *declared = expectedFormat == CameraModelFormat::AspTsai5
                               ? "asp_tsai5"
                               : "asp_tsai_null";
    const char *actual =
        actualFormat == CameraModelFormat::AspTsai5 ? "TSAI" : "NULL";
    throw std::invalid_argument(
        std::string("CAMERA_MODEL_ERROR: camera_model_format: declared ") +
        declared + " but the camera file terminal is " + actual);
  }
  camera->requireMatches(frame);
  return camera;
}

}  // namespace

std::unique_ptr<RenderCamera> loadRenderCamera(
    const FrameState &frame, const DistortionInverseControls &inverseControls) {
  switch (frame.cameraModelFormat) {
    case CameraModelFormat::AspTsaiNull:
      return loadCheckedTsai(frame, inverseControls,
                             CameraModelFormat::AspTsaiNull);
    case CameraModelFormat::AspTsai5:
      return loadCheckedTsai(frame, inverseControls,
                             CameraModelFormat::AspTsai5);
    case CameraModelFormat::OpenCvRadtan5Json:
      return std::make_unique<OpenCvRadtanCamera>(
          OpenCvRadtanCamera::read(frame.cameraPath, frame, inverseControls));
  }
  throw std::invalid_argument("CAMERA_MODEL_ERROR: camera_model_format: "
                              "unsupported value");
}

}  // namespace Isis::SmallBodyRender
