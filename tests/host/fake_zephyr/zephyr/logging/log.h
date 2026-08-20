#ifndef OD_TEST_FAKE_ZEPHYR_LOG_H
#define OD_TEST_FAKE_ZEPHYR_LOG_H

/* A mutable parameter is intentional: production Zephyr copies transient strings into its
 * deferred package. Casting to const would fail this fixture at compile time. */
void fake_log_raw_submit(char *record);

#define LOG_RAW(format_, record_) fake_log_raw_submit(record_)

#endif
