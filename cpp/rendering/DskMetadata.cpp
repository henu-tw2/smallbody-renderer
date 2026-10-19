// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "DskMetadata.h"

#include <SpiceUsr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace Isis::SmallBodyRender {
namespace {

std::string spiceMessage() {
  std::array<SpiceChar, 1841> message{};
  getmsg_c("LONG", static_cast<SpiceInt>(message.size()), message.data());
  const std::string result(message.data());
  reset_c();
  return result;
}

void requireSpiceSuccess(const std::string &field) {
  if (failed_c()) {
    throw std::runtime_error(field + ": CSPICE failure: " + spiceMessage());
  }
}

class KernelScope {
  public:
    explicit KernelScope(
        const std::vector<std::filesystem::path> &kernelPaths) {
      loaded_.reserve(kernelPaths.size());
      for (std::size_t index = 0; index < kernelPaths.size(); ++index) {
        const auto &path = kernelPaths[index];
        const std::string field =
            "dsk.frame_kernel_paths[" + std::to_string(index) + "]";
        if (!std::filesystem::is_regular_file(path)) {
          throw std::invalid_argument(field +
                                      ": kernel file does not exist");
        }
        furnsh_c(path.c_str());
        requireSpiceSuccess(field);
        loaded_.push_back(path);
      }
    }

    ~KernelScope() {
      for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
           ++iterator) {
        unload_c(iterator->c_str());
        if (failed_c()) {
          reset_c();
        }
      }
    }

  private:
    std::vector<std::filesystem::path> loaded_;
};

class DasHandle {
  public:
    explicit DasHandle(const std::filesystem::path &path) {
      dasopr_c(path.c_str(), &handle_);
      requireSpiceSuccess("dsk.path");
    }

    ~DasHandle() {
      if (handle_ >= 0) {
        dascls_c(handle_);
        if (failed_c()) {
          reset_c();
        }
      }
    }

    SpiceInt value() const {
      return handle_;
    }

  private:
    SpiceInt handle_{-1};
};

}  // namespace

DskMetadata inspectSingleType2Dsk(
    const DskSpecification &specification) {
  SpiceChar returnAction[] = "RETURN";
  erract_c("SET", 0, returnAction);
  requireSpiceSuccess("CSPICE error action");

  if (!specification.path.is_absolute() ||
      !std::filesystem::is_regular_file(specification.path)) {
    throw std::invalid_argument(
        "dsk.path: must name an existing absolute regular file");
  }

  KernelScope kernels(specification.frameKernelPaths);
  DasHandle file(specification.path);
  SpiceDLADescr descriptor{};
  SpiceBoolean found = SPICEFALSE;
  dlabfs_c(file.value(), &descriptor, &found);
  requireSpiceSuccess("dsk.segment");

  int segmentCount = 0;
  SpiceDSKDescr onlyDescriptor{};
  SpiceDLADescr onlyDla{};
  while (found) {
    SpiceDSKDescr current{};
    dskgd_c(file.value(), &descriptor, &current);
    requireSpiceSuccess("dsk.segment");
    if (current.dtype != 2) {
      throw std::invalid_argument(
          "dsk.segment: every segment must be NAIF Type-2");
    }
    ++segmentCount;
    if (segmentCount == 1) {
      onlyDescriptor = current;
      onlyDla = descriptor;
    }
    SpiceDLADescr next{};
    SpiceBoolean hasNext = SPICEFALSE;
    dlafns_c(file.value(), &descriptor, &next, &hasNext);
    requireSpiceSuccess("dsk.segment");
    descriptor = next;
    found = hasNext;
  }
  if (segmentCount != 1) {
    throw std::invalid_argument(
        "dsk.segment: expected exactly one Type-2 segment, found " +
        std::to_string(segmentCount));
  }

  if (onlyDescriptor.center != specification.expectedCenterId) {
    throw std::invalid_argument(
        "dsk.expected_center_id: declared " +
        std::to_string(specification.expectedCenterId) +
        " but DSK contains " + std::to_string(onlyDescriptor.center));
  }
  if (onlyDescriptor.frmcde != specification.expectedFrameId) {
    throw std::invalid_argument(
        "dsk.expected_frame_id: declared " +
        std::to_string(specification.expectedFrameId) +
        " but DSK contains " + std::to_string(onlyDescriptor.frmcde));
  }

  std::array<SpiceChar, 128> frameNameBuffer{};
  frmnam_c(onlyDescriptor.frmcde,
           static_cast<SpiceInt>(frameNameBuffer.size()),
           frameNameBuffer.data());
  requireSpiceSuccess("dsk.expected_frame_name");
  const std::string frameName(frameNameBuffer.data());
  if (frameName.empty() ||
      frameName != specification.expectedFrameName) {
    throw std::invalid_argument(
        "dsk.expected_frame_name: declared " +
        specification.expectedFrameName + " but DSK resolves to " +
        (frameName.empty() ? std::string("<unresolved>") : frameName));
  }

  SpiceInt vertexCount = 0;
  SpiceInt plateCount = 0;
  dskz02_c(file.value(), &onlyDla, &vertexCount, &plateCount);
  requireSpiceSuccess("dsk.segment");
  if (vertexCount <= 0 || plateCount <= 0) {
    throw std::invalid_argument(
        "dsk.segment: Type-2 vertex and plate counts must be positive");
  }

  constexpr SpiceInt kVertexBatchSize = 4096;
  SpiceDouble vertices[kVertexBatchSize][3];
  SpiceInt nextVertex = 1;
  double maximumVertexRadiusM = 0.0;
  while (nextVertex <= vertexCount) {
    const SpiceInt remaining = vertexCount - nextVertex + 1;
    const SpiceInt requested =
        std::min(kVertexBatchSize, remaining);
    SpiceInt returned = 0;
    dskv02_c(file.value(), &onlyDla, nextVertex, requested, &returned,
             vertices);
    requireSpiceSuccess("dsk.segment.vertices");
    if (returned != requested) {
      throw std::runtime_error(
          "dsk.segment.vertices: CSPICE returned " +
          std::to_string(returned) + " vertices at 1-based index " +
          std::to_string(nextVertex) + ", expected " +
          std::to_string(requested));
    }
    for (SpiceInt index = 0; index < returned; ++index) {
      const double xM = vertices[index][0] * 1000.0;
      const double yM = vertices[index][1] * 1000.0;
      const double zM = vertices[index][2] * 1000.0;
      if (!std::isfinite(xM) || !std::isfinite(yM) ||
          !std::isfinite(zM)) {
        throw std::runtime_error(
            "dsk.segment.vertices: Type-2 vertex coordinates must "
            "remain finite after kilometres-to-metres conversion");
      }
      const double radiusM =
          std::hypot(std::hypot(xM, yM), zM);
      if (!std::isfinite(radiusM)) {
        throw std::runtime_error(
            "dsk.segment.vertices: Type-2 vertex radius is non-finite");
      }
      maximumVertexRadiusM =
          std::max(maximumVertexRadiusM, radiusM);
    }
    nextVertex += returned;
  }
  if (!std::isfinite(maximumVertexRadiusM) ||
      !(maximumVertexRadiusM > 0.0)) {
    throw std::invalid_argument(
        "maximumVertexRadiusM: Type-2 maximum vertex radius must be "
        "finite and strictly positive metres");
  }

  return {onlyDescriptor.center,
          onlyDescriptor.surfce,
          onlyDescriptor.frmcde,
          frameName,
          vertexCount,
          plateCount,
          maximumVertexRadiusM};
}

}  // namespace Isis::SmallBodyRender
