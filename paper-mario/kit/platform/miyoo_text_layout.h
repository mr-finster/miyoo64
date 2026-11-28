#ifndef MIYOO_TEXT_LAYOUT_H
#define MIYOO_TEXT_LAYOUT_H
/* Keep the selected marquee at its established scale. Other labels use the
 * largest scale that fits the 8-pixel-cell font within the row lane. */
static inline int miyoo_menu_label_scale(int width_at_scale3, int selected, int lane_width) {
    int scale = 3;
    if (selected) return scale;
    while (scale > 1 && width_at_scale3 * scale / 3 > lane_width) scale--;
    return scale;
}
static inline int miyoo_menu_label_scrolls(int width_at_scale3, int lane_width) {
    return width_at_scale3 > lane_width;
}
#endif
