// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "PlateAlbedoTable.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QFile>
#include <QString>

namespace Isis::SmallBodyRender {
namespace {

constexpr std::string_view kErrorCategory = "ALBEDO_TABLE_ERROR";
constexpr std::string_view kPlateId = "plate_id";
constexpr std::string_view kRelativeAlbedo = "relative_albedo_linear";
constexpr std::string_view kValid = "valid";
constexpr std::string_view kSourceCount = "source_count";
constexpr std::string_view kSourceRms = "source_rms";

[[noreturn]] void reject(const std::string &message) {
  throw std::invalid_argument(std::string(kErrorCategory) + ": " + message);
}

void requirePositiveFinite(double value, const std::string &field) {
  if (!isRenderableRelativeAlbedo(value)) {
    reject(field +
           " must remain finite and positive in IEEE float32");
  }
}

std::vector<std::string> splitCsvLine(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t fieldStart = 0;
  while (true) {
    const std::size_t delimiter = line.find(',', fieldStart);
    if (delimiter == std::string::npos) {
      fields.emplace_back(line.substr(fieldStart));
      return fields;
    }
    fields.emplace_back(line.substr(fieldStart, delimiter - fieldStart));
    fieldStart = delimiter + 1;
  }
}

std::uint64_t parseUnsigned(const std::string &text,
                            const std::string &field,
                            std::size_t lineNumber) {
  if (text.empty()) {
    reject("line " + std::to_string(lineNumber) + " " + field +
           " is empty");
  }
  std::uint64_t value = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end) {
    reject("line " + std::to_string(lineNumber) + " " + field +
           " must be an unsigned integer");
  }
  return value;
}

double parseFiniteDouble(const std::string &text, const std::string &field,
                         std::size_t lineNumber) {
  if (text.empty()) {
    reject("line " + std::to_string(lineNumber) + " " + field +
           " is empty");
  }
  std::size_t parsed = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &parsed);
  }
  catch (const std::exception &) {
    reject("line " + std::to_string(lineNumber) + " " + field +
           " must be a finite number");
  }
  if (parsed != text.size() || !std::isfinite(value)) {
    reject("line " + std::to_string(lineNumber) + " " + field +
           " must be a finite number");
  }
  return value;
}

std::string sha256Snapshot(const std::string &snapshot) {
  return QCryptographicHash::hash(QByteArray::fromStdString(snapshot),
                                  QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}

std::unordered_map<std::string, std::size_t> parseHeader(
    const std::string &header) {
  const std::vector<std::string> names = splitCsvLine(header);
  if (names.size() != 5) {
    reject("header must contain exactly five fields");
  }
  std::unordered_map<std::string, std::size_t> indices;
  for (std::size_t index = 0; index < names.size(); ++index) {
    const std::string &name = names[index];
    if (name != kPlateId && name != kRelativeAlbedo && name != kValid &&
        name != kSourceCount && name != kSourceRms) {
      reject("header contains unknown field " + name);
    }
    if (!indices.emplace(name, index).second) {
      reject("header contains duplicate field " + name);
    }
  }
  if (indices.size() != 5) {
    reject("header is missing a required field");
  }
  return indices;
}

PlateAlbedoStatistics statisticsFor(const std::vector<double> &values,
                                    std::uint32_t plateCount,
                                    std::uint32_t invalidPlateCount,
                                    double defaultAlbedo) {
  PlateAlbedoStatistics statistics;
  if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
    reject("valid plate count exceeds uint32 range");
  }
  statistics.validPlateCount = static_cast<std::uint32_t>(values.size());
  statistics.missingPlateCount = plateCount - statistics.validPlateCount;
  statistics.invalidPlateCount = invalidPlateCount;
  if (values.empty()) {
    statistics.minimumRelativeAlbedo = defaultAlbedo;
    statistics.maximumRelativeAlbedo = defaultAlbedo;
    statistics.medianRelativeAlbedo = defaultAlbedo;
    return statistics;
  }

  std::vector<double> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  statistics.minimumRelativeAlbedo = sorted.front();
  statistics.maximumRelativeAlbedo = sorted.back();
  const std::size_t midpoint = sorted.size() / 2;
  if (sorted.size() % 2 == 0) {
    statistics.medianRelativeAlbedo =
        0.5 * sorted[midpoint - 1] + 0.5 * sorted[midpoint];
  }
  else {
    statistics.medianRelativeAlbedo = sorted[midpoint];
  }
  return statistics;
}

}  // namespace

PlateAlbedoTable::PlateAlbedoTable(std::uint32_t plateCount,
                                   double defaultAlbedo,
                                   std::vector<double> relativeAlbedos,
                                   std::vector<bool> validPlates,
                                   PlateAlbedoStatistics statistics)
    : plateCount_(plateCount),
      defaultAlbedo_(defaultAlbedo),
      relativeAlbedos_(std::move(relativeAlbedos)),
      validPlates_(std::move(validPlates)),
      statistics_(statistics) {}

PlateAlbedoTable PlateAlbedoTable::read(const std::filesystem::path &path,
                                        std::uint32_t plateCount,
                                        double defaultAlbedo,
                                        const std::string &expectedSha256) {
  if (plateCount == 0) {
    reject("plateCount must be positive");
  }
  requirePositiveFinite(defaultAlbedo, "defaultAlbedo");
  std::error_code statusError;
  if (!std::filesystem::is_regular_file(path, statusError)) {
    if (statusError) {
      reject("cannot inspect CSV " + path.string() + ": " +
             statusError.message());
    }
    reject("CSV is not a regular file: " + path.string());
  }
  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    reject("cannot open CSV " + path.string() + " for snapshot");
  }
  const std::string snapshot = file.readAll().toStdString();
  if (file.error() != QFileDevice::NoError) {
    reject("failed while reading CSV snapshot " + path.string());
  }
  return readSnapshot(snapshot, plateCount, defaultAlbedo, expectedSha256);
}

PlateAlbedoTable PlateAlbedoTable::readSnapshot(
    std::string csvSnapshot, std::uint32_t plateCount, double defaultAlbedo,
    const std::string &expectedSha256) {
  if (plateCount == 0) {
    reject("plateCount must be positive");
  }
  requirePositiveFinite(defaultAlbedo, "defaultAlbedo");
  if (!expectedSha256.empty() &&
      sha256Snapshot(csvSnapshot) != expectedSha256) {
    reject("CSV snapshot SHA-256 does not match expectedSha256");
  }

  std::istringstream stream(std::move(csvSnapshot));
  std::string header;
  if (!std::getline(stream, header)) {
    reject("CSV snapshot is empty");
  }
  if (!header.empty() && header.back() == '\r') {
    header.pop_back();
  }
  const std::unordered_map<std::string, std::size_t> fields =
      parseHeader(header);

  std::vector<double> relativeAlbedos(static_cast<std::size_t>(plateCount) +
                                      1,
                                      defaultAlbedo);
  std::vector<bool> validPlates(static_cast<std::size_t>(plateCount) + 1,
                                false);
  std::vector<bool> seenPlates(static_cast<std::size_t>(plateCount) + 1,
                               false);
  std::vector<double> validValues;
  std::uint32_t invalidPlateCount = 0;
  std::string line;
  std::size_t lineNumber = 1;
  while (std::getline(stream, line)) {
    ++lineNumber;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::vector<std::string> columns = splitCsvLine(line);
    if (columns.size() != fields.size()) {
      reject("line " + std::to_string(lineNumber) +
             " has unconsumed or missing fields");
    }

    const std::uint64_t plateId = parseUnsigned(
        columns.at(fields.at(std::string(kPlateId))), "plate_id", lineNumber);
    if (plateId == 0 || plateId > plateCount) {
      reject("line " + std::to_string(lineNumber) +
             " plate_id is outside 1..plateCount");
    }
    const std::size_t plateIndex = static_cast<std::size_t>(plateId);
    if (seenPlates[plateIndex]) {
      reject("line " + std::to_string(lineNumber) +
             " duplicates a plate_id");
    }

    const double albedo = parseFiniteDouble(
        columns.at(fields.at(std::string(kRelativeAlbedo))),
        "relative_albedo_linear", lineNumber);
    requirePositiveFinite(albedo, "line " + std::to_string(lineNumber) +
                                      " relative_albedo_linear");
    const std::string &valid = columns.at(fields.at(std::string(kValid)));
    if (valid != "0" && valid != "1") {
      reject("line " + std::to_string(lineNumber) +
             " valid must be exactly 0 or 1");
    }
    static_cast<void>(parseUnsigned(
        columns.at(fields.at(std::string(kSourceCount))), "source_count",
        lineNumber));
    const double sourceRms = parseFiniteDouble(
        columns.at(fields.at(std::string(kSourceRms))), "source_rms",
        lineNumber);
    if (sourceRms < 0.0) {
      reject("line " + std::to_string(lineNumber) +
             " source_rms must be non-negative");
    }

    seenPlates[plateIndex] = true;
    relativeAlbedos[plateIndex] = albedo;
    if (valid == "1") {
      validPlates[plateIndex] = true;
      validValues.push_back(albedo);
    }
    else {
      ++invalidPlateCount;
    }
  }
  if (!stream.eof()) {
    reject("failed while reading CSV snapshot");
  }

  const PlateAlbedoStatistics statistics = statisticsFor(
      validValues, plateCount, invalidPlateCount, defaultAlbedo);
  return PlateAlbedoTable(plateCount, defaultAlbedo,
                          std::move(relativeAlbedos),
                          std::move(validPlates), statistics);
}

double PlateAlbedoTable::relativeAlbedo(std::uint32_t plateId) const {
  if (plateId == 0 || plateId > plateCount_) {
    reject("plateId is outside 1..plateCount");
  }
  if (!validPlates_.at(plateId)) {
    return defaultAlbedo_;
  }
  return relativeAlbedos_.at(plateId);
}

const PlateAlbedoStatistics &PlateAlbedoTable::statistics() const {
  return statistics_;
}

}  // namespace Isis::SmallBodyRender
