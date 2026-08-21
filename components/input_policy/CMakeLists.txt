# ZenClock input policy — pure decision layer for button presses.
# No ESP-IDF headers: this also builds for the host-side [env:native] tests.
idf_component_register(
        SRCS "input_policy.c"
        INCLUDE_DIRS "."
        REQUIRES ui
)
