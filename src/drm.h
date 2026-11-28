#ifndef DRM_H
#define DRM_H

#include <cstdint>
#include <optional>
#include <string>

namespace edge_tts_drm {

std::string generateSecMsGecV2At(std::int64_t unixSecs, const std::string &trustedClientToken);

std::string generateSecMsGecV2(const std::string &trustedClientToken);

std::optional<std::int64_t> parseRfc2616Date(const std::string &date);

bool adjustClockSkewFromDate(const std::string &serverDate);

} // namespace edge_tts_drm

#endif // DRM_H
