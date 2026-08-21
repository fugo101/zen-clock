# ZenClock battery view — pure mapping from a battery reading to icon, tint, blink, label and the
# low-battery policy flag. No LVGL and no ESP-IDF headers: this also builds for the host-side
# [env:native] tests.
idf_component_register(
        SRCS "battery_view.c"
        INCLUDE_DIRS "."
)
