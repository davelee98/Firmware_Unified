#ifndef OD_TEST_FAKE_ZEPHYR_UTIL_H
#define OD_TEST_FAKE_ZEPHYR_UTIL_H

#define IS_ENABLED(value) (value)
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)

#endif
