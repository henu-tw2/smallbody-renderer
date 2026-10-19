// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "IlluminationStateResolver.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void usage(std::ostream &stream) {
  stream
      << "Usage: smallbody_illumination_resolve"
         " --target-states PATH --sun-states PATH"
         " --reference-origin NAME --output PATH\n"
      << "The two exact-column inputs declare absolute positions in metres,"
         " on J2000 axes, at ET/TDB seconds, relative to the same required"
         " caller-declared reference origin. Output direction is"
         " target_to_sun = Sun absolute - target absolute. No light-time or"
         " stellar-aberration correction is applied.\n";
}

std::map<std::string, std::string> arguments(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    usage(std::cout);
    std::exit(EXIT_SUCCESS);
  }
  if (argc != 9) {
    throw std::invalid_argument("exactly four named arguments are required");
  }
  std::map<std::string, std::string> result;
  for (int index = 1; index < argc; index += 2) {
    const std::string name(argv[index]);
    if (name != "--target-states" && name != "--sun-states" &&
        name != "--reference-origin" && name != "--output") {
      throw std::invalid_argument("unknown argument: " + name);
    }
    if (!result.emplace(name, argv[index + 1]).second) {
      throw std::invalid_argument("duplicate argument: " + name);
    }
  }
  for (const std::string name :
       {"--target-states", "--sun-states", "--reference-origin",
        "--output"}) {
    if (result.find(name) == result.end()) {
      throw std::invalid_argument("missing required argument: " + name);
    }
  }
  return result;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const std::map<std::string, std::string> options =
        arguments(argc, argv);
    const auto summary =
        Isis::SmallBodyRender::resolveAndPublishIllumination(
            std::filesystem::path(options.at("--target-states")),
            std::filesystem::path(options.at("--sun-states")),
            options.at("--reference-origin"),
            std::filesystem::path(options.at("--output")));
    std::cout << "status=passed"
              << " frame_count=" << summary.frameCount
              << " reference_origin=" << summary.referenceOrigin
              << " axes=J2000 time_scale=ET/TDB units=m"
              << " direction=target_to_sun"
              << " output_sha256=" << summary.outputSha256
              << " correction=NONE\n";
    return EXIT_SUCCESS;
  }
  catch (const std::exception &error) {
    std::cerr << "ILLUMINATION_RESOLVE_ERROR: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
