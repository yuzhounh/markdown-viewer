#pragma once

#include <algorithm>

namespace TabLayout {
constexpr float content_padding = 8;
constexpr float inactive_close_width = 84;
constexpr float close_hit_size = 24;

struct Geometry {
  bool close_visible = false;
  bool icon_visible = false;
  float close_center = 0;
  float close_size = 0;
  float icon_left = 0;
  float icon_size = 0;
  float title_left = 0;
  float title_width = 0;
};

inline Geometry Calculate(float width, bool active) {
  width = std::max(0.0f, width);
  Geometry result;
  result.close_visible = width > 0 && (active || width >= inactive_close_width);
  result.close_center = std::max(width - 16, width / 2);
  result.close_size = std::min(close_hit_size, width);
  const float content_width = width - 2 * content_padding;
  result.icon_visible =
      width > 0 && (!result.close_visible || content_width >= 16 + 16 + 4);
  result.icon_size = result.icon_visible ? std::min(16.0f, width) : 0;
  result.icon_left = !result.close_visible && content_width < 16
                         ? (width - result.icon_size) / 2
                         : content_padding;
  result.title_left = result.icon_visible
                          ? result.icon_left + result.icon_size + 8
                          : content_padding;
  const float title_right = result.close_visible
                                ? result.close_center - result.close_size / 2
                                : width - content_padding;
  result.title_width = std::max(0.0f, title_right - result.title_left);
  return result;
}
} // namespace TabLayout
