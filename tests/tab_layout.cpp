#include "../src/tab_layout.h"
#include <cassert>
#include <iostream>

int main() {
  for (float width : {0.0f, 4.0f, 16.0f, 24.0f, 32.0f, 48.0f, 52.0f, 60.0f,
                      83.0f, 84.0f, 200.0f}) {
    const auto active = TabLayout::Calculate(width, true);
    const auto inactive = TabLayout::Calculate(width, false);
    assert(active.close_visible == (width > 0));
    assert(inactive.close_visible == (width >= 84));
    assert(active.close_center - active.close_size / 2 >= 0);
    assert(active.close_center + active.close_size / 2 <= width);
    assert(active.title_width >= 0 && inactive.title_width >= 0);
    if (width > 0 && width < 52)
      assert(!active.icon_visible);
    if (width >= 52) {
      assert(active.icon_visible && active.icon_left == 8);
      assert(active.icon_left + active.icon_size <=
             active.close_center - active.close_size / 2);
    }
    if (width >= 32)
      assert(inactive.icon_left == 8);
  }
  std::cout << "PASS: active close priority, inactive threshold, bounded hit "
               "target and favicon spacing\n";
}
