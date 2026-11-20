#include <vespera/ui_tokens.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

void fits_and_never_overlaps() {
  const double labels[]{40, 20, 33, 46, 55};
  const double fields[]{64, 52, 120, 140, 62};
  vs_ui_labelled_field row[5];
  check(vs_ui_layout_labelled_row(152, 865, 6, 12, labels, fields, 5, row) == 1, "row fits");
  check(row[0].label_x == 152, "row starts at start");
  check(std::abs(row[4].field_x + row[4].field_width - 865) < 1e-9, "row ends at end");
  for (int i = 0; i < 5; ++i) {
    check(row[i].field_x - (row[i].label_x + row[i].label_width) == 6, "label gap");
    check(row[i].field_width == fields[i], "field width preserved");
    if (i > 0) {
      check(row[i].label_x >= row[i - 1].field_x + row[i - 1].field_width + 12,
            "pairs keep minimum gap");
    }
  }
}

void rejects_rows_that_cannot_fit() {
  const double labels[]{40, 40};
  const double fields[]{100, 100};
  vs_ui_labelled_field row[2]{{-1, -1, -1, -1}, {-1, -1, -1, -1}};
  check(vs_ui_layout_labelled_row(0, 300, 6, 8, labels, fields, 2, row) == 1, "exact fit");
  check(row[1].label_x == 154, "exact fit uses minimum gap");
  check(vs_ui_layout_labelled_row(0, 300, 6, 9, labels, fields, 2, row) == 0,
        "minimum pair gap enforced");
  vs_ui_labelled_field untouched[2]{{-1, -1, -1, -1}, {-1, -1, -1, -1}};
  check(vs_ui_layout_labelled_row(0, 200, 6, 0, labels, fields, 2, untouched) == 0,
        "too-wide row rejected");
  check(untouched[0].label_x == -1 && untouched[1].field_x == -1, "rejection writes nothing");
  check(vs_ui_layout_labelled_row(0, 300, 6, 0, labels, fields, 0, untouched) == 0,
        "empty row rejected");
  check(vs_ui_layout_labelled_row(10, 0, 6, 0, labels, fields, 1, untouched) == 0,
        "inverted bounds rejected");
  const double negative[]{-1};
  check(vs_ui_layout_labelled_row(0, 300, 6, 0, negative, fields, 1, untouched) == 0,
        "negative width rejected");
}

void single_pair_is_left_aligned() {
  const double label[]{30};
  const double field[]{50};
  vs_ui_labelled_field row[1];
  check(vs_ui_layout_labelled_row(10, 500, 8, 12, label, field, 1, row) == 1, "single fits");
  check(row[0].label_x == 10 && row[0].field_x == 48, "single pair starts at start");
}

void dropdown_titles_leave_room_for_chevron() {
  for (vs_ui_size size : {VS_UI_SIZE_STANDARD, VS_UI_SIZE_COMPACT}) {
    vs_ui_metrics metrics = vs_ui_metrics_for(size);
    check(vs_ui_dropdown_accessory_width(size) > metrics.horizontal_padding,
          "accessory reserves chevron space beyond padding");
    check(metrics.menu_row_height >= metrics.font_size * 1.5, "menu rows fit their text");
    check(metrics.control_height >= metrics.checkbox_size, "checkbox fits a control row");
  }
}
} // namespace

int main() {
  fits_and_never_overlaps();
  rejects_rows_that_cannot_fit();
  single_pair_is_left_aligned();
  dropdown_titles_leave_room_for_chevron();
  if (failures == 0) {
    std::puts("ui tokens: all checks passed");
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
