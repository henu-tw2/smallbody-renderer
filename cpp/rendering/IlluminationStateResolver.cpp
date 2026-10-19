// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "IlluminationStateResolver.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Isis::SmallBodyRender {
namespace {

constexpr double kEtToleranceS = 1.0e-6;
constexpr const char *kAbsoluteHeader =
    "frame_id,et_tdb_s,position_j2000_x_m,position_j2000_y_m,"
    "position_j2000_z_m";
constexpr const char *kIlluminationHeader =
    "frame_id,et_tdb_s,target_to_sun_j2000_x_m,"
    "target_to_sun_j2000_y_m,target_to_sun_j2000_z_m";
const std::regex kSafeName("^[A-Za-z0-9][A-Za-z0-9._-]*$");

struct AbsoluteState {
  std::string frameId;
  double etTdbS;
  Vector3 positionJ2000M;
};

struct IlluminationState {
  std::string frameId;
  double etTdbS;
  Vector3 targetToSunJ2000M;
};

std::vector<std::string> splitExactCsvRow(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = line.find(',', begin);
    fields.push_back(line.substr(begin, comma - begin));
    if (comma == std::string::npos) {
      return fields;
    }
    begin = comma + 1;
  }
}

std::string lineWithoutCarriageReturn(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

double finiteNumber(const std::string &text, const std::string &field) {
  double value = 0.0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value,
                      std::chars_format::general);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
    throw std::invalid_argument(field + ": must be a finite number");
  }
  return value;
}

bool hasAsciiEdgeWhitespace(const std::string &value) {
  constexpr const char *whitespace = " \t\n\r\f\v";
  return !value.empty() &&
         (std::strchr(whitespace, value.front()) != nullptr ||
          std::strchr(whitespace, value.back()) != nullptr);
}

std::string safeFrameId(const std::string &value,
                        const std::string &field) {
  if (!std::regex_match(value, kSafeName) || value == "." || value == "..") {
    throw std::invalid_argument(field + ": unsafe frame_id");
  }
  return value;
}

void requireRegularInput(const std::filesystem::path &path,
                         const std::string &role) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    throw std::invalid_argument(role + ": missing regular file: " +
                                path.string());
  }
}

std::vector<AbsoluteState> loadAbsoluteStates(
    const std::filesystem::path &path, const std::string &role) {
  requireRegularInput(path, role);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error(role + ": failed to open: " + path.string());
  }
  std::string line;
  if (!std::getline(stream, line) ||
      lineWithoutCarriageReturn(line) != kAbsoluteHeader) {
    throw std::invalid_argument(
        role + ": columns must be exactly " + kAbsoluteHeader);
  }
  std::vector<AbsoluteState> states;
  std::map<std::string, std::size_t> frameLines;
  std::size_t lineNumber = 1;
  while (std::getline(stream, line)) {
    ++lineNumber;
    line = lineWithoutCarriageReturn(std::move(line));
    const std::string prefix =
        role + ":" + std::to_string(lineNumber);
    if (line.empty()) {
      throw std::invalid_argument(prefix +
                                  ": empty physical row is not allowed");
    }
    const std::vector<std::string> fields = splitExactCsvRow(line);
    if (fields.size() != 5) {
      throw std::invalid_argument(prefix +
                                  ": expected exactly 5 CSV fields");
    }
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (fields[index].empty()) {
        throw std::invalid_argument(
            prefix + ": CSV field " + std::to_string(index + 1) +
            " must not be empty");
      }
      if (hasAsciiEdgeWhitespace(fields[index])) {
        throw std::invalid_argument(
            prefix + ": CSV field " + std::to_string(index + 1) +
            " must not have leading or trailing whitespace");
      }
    }
    const std::string frameId =
        safeFrameId(fields[0], prefix + ".frame_id");
    if (!frameLines.emplace(frameId, lineNumber).second) {
      throw std::invalid_argument(prefix + ": duplicate frame_id " +
                                  frameId);
    }
    states.push_back(
        {frameId,
         finiteNumber(fields[1], prefix + ".et_tdb_s"),
         Vector3(finiteNumber(fields[2], prefix + ".position_j2000_x_m"),
                 finiteNumber(fields[3], prefix + ".position_j2000_y_m"),
                 finiteNumber(fields[4], prefix + ".position_j2000_z_m"))});
  }
  if (!stream.eof()) {
    throw std::runtime_error(role + ": failed while reading: " +
                             path.string());
  }
  if (states.empty()) {
    throw std::invalid_argument(role + ": state CSV must not be empty");
  }
  return states;
}

std::vector<IlluminationState> resolveStates(
    const std::vector<AbsoluteState> &targets,
    const std::vector<AbsoluteState> &suns) {
  std::map<std::string, const AbsoluteState *> sunByFrame;
  for (const AbsoluteState &sun : suns) {
    sunByFrame.emplace(sun.frameId, &sun);
  }
  if (targets.size() != suns.size()) {
    throw std::invalid_argument(
        "target/sun frame_id sets must match exactly");
  }
  std::vector<IlluminationState> result;
  result.reserve(targets.size());
  for (const AbsoluteState &target : targets) {
    const auto found = sunByFrame.find(target.frameId);
    if (found == sunByFrame.end()) {
      throw std::invalid_argument(
          "target/sun frame_id sets must match exactly");
    }
    const AbsoluteState &sun = *found->second;
    if (std::abs(target.etTdbS - sun.etTdbS) > kEtToleranceS) {
      throw std::invalid_argument(
          target.frameId +
          ": target/sun ET/TDB differs by more than 1e-6 s");
    }
    const Vector3 targetToSunJ2000M =
        targetToSecondary(target.positionJ2000M, sun.positionJ2000M);
    if (!(targetToSunJ2000M.cwiseAbs().maxCoeff() > 0.0)) {
      throw std::invalid_argument(
          target.frameId +
          ": target_to_sun_j2000_m must be finite and nonzero");
    }
    result.push_back(
        {target.frameId, target.etTdbS, targetToSunJ2000M});
  }
  return result;
}

std::string illuminationCsv(
    const std::vector<IlluminationState> &states) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << kIlluminationHeader << '\n'
         << std::setprecision(std::numeric_limits<double>::max_digits10);
  for (const IlluminationState &state : states) {
    stream << state.frameId << ',' << state.etTdbS << ','
           << state.targetToSunJ2000M.x() << ','
           << state.targetToSunJ2000M.y() << ','
           << state.targetToSunJ2000M.z() << '\n';
  }
  if (!stream) {
    throw std::runtime_error("failed to serialize illumination CSV");
  }
  return stream.str();
}

void verifyIlluminationCsv(
    const std::filesystem::path &path,
    const std::vector<IlluminationState> &expected) {
  std::ifstream stream(path, std::ios::binary);
  std::string line;
  if (!stream || !std::getline(stream, line) ||
      lineWithoutCarriageReturn(line) != kIlluminationHeader) {
    throw std::runtime_error(
        "temporary illumination CSV failed exact-column verification");
  }
  std::size_t index = 0;
  while (std::getline(stream, line)) {
    line = lineWithoutCarriageReturn(std::move(line));
    const std::vector<std::string> fields = splitExactCsvRow(line);
    if (fields.size() != 5 || index >= expected.size()) {
      throw std::runtime_error(
          "temporary illumination CSV failed row-count verification");
    }
    const IlluminationState &row = expected[index++];
    if (fields[0] != row.frameId ||
        finiteNumber(fields[1], "temporary.et_tdb_s") != row.etTdbS ||
        finiteNumber(fields[2], "temporary.target_to_sun_j2000_x_m") !=
            row.targetToSunJ2000M.x() ||
        finiteNumber(fields[3], "temporary.target_to_sun_j2000_y_m") !=
            row.targetToSunJ2000M.y() ||
        finiteNumber(fields[4], "temporary.target_to_sun_j2000_z_m") !=
            row.targetToSunJ2000M.z()) {
      throw std::runtime_error(
          "temporary illumination CSV failed content verification");
    }
  }
  if (!stream.eof() || index != expected.size()) {
    throw std::runtime_error(
        "temporary illumination CSV failed row-count verification");
  }
}

class TemporaryOutput {
 public:
  TemporaryOutput(std::filesystem::path path, int descriptor)
      : path_(std::move(path)), descriptor_(descriptor), owned_(true) {}
  ~TemporaryOutput() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
    if (owned_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }
  const std::filesystem::path &path() const { return path_; }
  int descriptor() const { return descriptor_; }
  void markClosed() { descriptor_ = -1; }
  void markPublished() { owned_ = false; }

 private:
  std::filesystem::path path_;
  int descriptor_;
  bool owned_;
};

std::unique_ptr<TemporaryOutput> createTemporaryOutput(
    const std::filesystem::path &parent,
    const std::string &outputFilename) {
  constexpr unsigned int kMaximumAttempts = 128;
  static unsigned long long sequence = 0;
  for (unsigned int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    const std::filesystem::path candidate =
        parent /
        (outputFilename + ".tmp." +
         std::to_string(static_cast<unsigned long long>(::getpid())) + "." +
         std::to_string(sequence++));
    const int descriptor =
        ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor >= 0) {
      return std::make_unique<TemporaryOutput>(candidate, descriptor);
    }
    if (errno != EEXIST) {
      throw std::runtime_error("failed to create temporary output: " +
                               std::string(std::strerror(errno)));
    }
  }
  throw std::runtime_error(
      "failed to create a unique temporary output after 128 attempts");
}

void writeDurably(TemporaryOutput &temporary, const std::string &bytes) {
  const int descriptor = temporary.descriptor();
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count =
        ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved = errno;
      throw std::runtime_error("failed to write temporary output: " +
                               std::string(std::strerror(saved)));
    }
    written += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0) {
    const int saved = errno;
    throw std::runtime_error("failed to sync temporary output: " +
                             std::string(std::strerror(saved)));
  }
  if (::close(descriptor) != 0) {
    temporary.markClosed();
    throw std::runtime_error("failed to close temporary output: " +
                             std::string(std::strerror(errno)));
  }
  temporary.markClosed();
}

std::string readFileBytes(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to read temporary output bytes");
  }
  std::string bytes{std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()};
  if (stream.bad()) {
    throw std::runtime_error("failed while reading temporary output bytes");
  }
  return bytes;
}

std::string sha256Bytes(const std::string &bytes) {
  return QCryptographicHash::hash(QByteArray::fromStdString(bytes),
                                  QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}

void publishNoReplace(const std::filesystem::path &temporary,
                      const std::filesystem::path &output,
                      const std::filesystem::path &parent) {
  const long status =
      ::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
                output.c_str(), RENAME_NOREPLACE);
  if (status != 0) {
    if (errno == EEXIST) {
      throw std::runtime_error("output already exists: " + output.string());
    }
    throw std::runtime_error("atomic output rename failed: " +
                             std::string(std::strerror(errno)));
  }
  const int directory =
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
  if (directory < 0 || ::fsync(directory) != 0) {
    const int saved = errno;
    if (directory >= 0) {
      ::close(directory);
    }
    const int removeStatus = ::unlink(output.c_str());
    throw std::runtime_error(
        "output published but parent-directory durability is unconfirmed: " +
        std::string(std::strerror(saved)) +
        (removeStatus == 0 ? "; formal output rolled back"
                           : "; formal output rollback failed: " +
                                 std::string(std::strerror(errno))));
  }
  if (::close(directory) != 0) {
    // The file and directory were already fsync'd. A close error cannot undo
    // that durable atomic publication and is therefore not reported as a
    // false product failure.
  }
}

}  // namespace

Vector3 targetToSecondary(const Vector3 &targetAbsoluteJ2000M,
                          const Vector3 &secondaryAbsoluteJ2000M) {
  if (!targetAbsoluteJ2000M.allFinite() ||
      !secondaryAbsoluteJ2000M.allFinite()) {
    throw std::invalid_argument(
        "absolute J2000 positions in metres must be finite");
  }
  const Vector3 result = secondaryAbsoluteJ2000M - targetAbsoluteJ2000M;
  if (!result.allFinite()) {
    throw std::invalid_argument(
        "target-to-secondary J2000 vector in metres must be finite");
  }
  return result;
}

namespace {

IlluminationResolutionSummary resolveAndPublishInternal(
    const std::filesystem::path &targetStatesPath,
    const std::filesystem::path &sunStatesPath,
    const std::string &referenceOrigin,
    const std::filesystem::path &outputPath,
    Testing::PreVerifyHook preVerifyHook) {
  if (!std::regex_match(referenceOrigin, kSafeName) ||
      referenceOrigin == "." || referenceOrigin == "..") {
    throw std::invalid_argument(
        "reference-origin must be a nonempty safe common-origin declaration");
  }
  const std::filesystem::path outputFilename = outputPath.filename();
  if (outputFilename.empty() || outputFilename == "." ||
      outputFilename == "..") {
    throw std::invalid_argument("output must name a new CSV file");
  }
  std::error_code error;
  const std::filesystem::path absoluteOutput =
      std::filesystem::absolute(outputPath, error);
  if (error) {
    throw std::invalid_argument("failed to resolve output path: " +
                                error.message());
  }
  const std::filesystem::path originalParent =
      absoluteOutput.parent_path();
  const std::filesystem::path parent =
      std::filesystem::canonical(originalParent, error);
  if (error) {
    throw std::invalid_argument(
        "output parent must be an existing resolvable directory: " +
        originalParent.string() + ": " + error.message());
  }
  if (!std::filesystem::is_directory(parent, error) || error) {
    throw std::invalid_argument(
        "output parent must be an existing directory: " + parent.string());
  }
  const std::filesystem::path actualOutput = parent / outputFilename;
  if (std::filesystem::exists(actualOutput, error) ||
      std::filesystem::is_symlink(actualOutput, error)) {
    throw std::runtime_error("output already exists: " +
                             actualOutput.string());
  }
  const std::vector<AbsoluteState> targets =
      loadAbsoluteStates(targetStatesPath, "target-states");
  const std::vector<AbsoluteState> suns =
      loadAbsoluteStates(sunStatesPath, "sun-states");
  const std::vector<IlluminationState> states =
      resolveStates(targets, suns);

  const std::string serialized = illuminationCsv(states);
  const std::string expectedSha256 = sha256Bytes(serialized);
  std::unique_ptr<TemporaryOutput> temporary =
      createTemporaryOutput(parent, actualOutput.filename().string());
  writeDurably(*temporary, serialized);
  if (preVerifyHook != nullptr) {
    preVerifyHook(temporary->path());
  }
  const std::string diskBytes = readFileBytes(temporary->path());
  const std::string diskSha256 = sha256Bytes(diskBytes);
  if (diskSha256 != expectedSha256) {
    throw std::runtime_error(
        "temporary illumination CSV SHA-256 does not match serialized bytes");
  }
  verifyIlluminationCsv(temporary->path(), states);
  publishNoReplace(temporary->path(), actualOutput, parent);
  temporary->markPublished();
  return {states.size(), referenceOrigin, expectedSha256};
}

}  // namespace

IlluminationResolutionSummary resolveAndPublishIllumination(
    const std::filesystem::path &targetStatesPath,
    const std::filesystem::path &sunStatesPath,
    const std::string &referenceOrigin,
    const std::filesystem::path &outputPath) {
  return resolveAndPublishInternal(targetStatesPath, sunStatesPath,
                                   referenceOrigin, outputPath, nullptr);
}

namespace Testing {

IlluminationResolutionSummary
resolveAndPublishIlluminationWithPreVerifyHook(
    const std::filesystem::path &targetStatesPath,
    const std::filesystem::path &sunStatesPath,
    const std::string &referenceOrigin,
    const std::filesystem::path &outputPath,
    PreVerifyHook hook) {
  if (hook == nullptr) {
    throw std::invalid_argument("pre-verification test hook must not be null");
  }
  return resolveAndPublishInternal(targetStatesPath, sunStatesPath,
                                   referenceOrigin, outputPath, hook);
}

}  // namespace Testing

}  // namespace Isis::SmallBodyRender
