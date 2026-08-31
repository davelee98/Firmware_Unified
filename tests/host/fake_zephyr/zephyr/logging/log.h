#ifndef OD_TEST_FAKE_ZEPHYR_LOG_H
#define OD_TEST_FAKE_ZEPHYR_LOG_H

/* A mutable parameter is intentional: production Zephyr copies transient strings into its
 * deferred package. Casting to const would fail this fixture at compile time. */
void fake_log_raw_submit(char *record);
void fake_log_printk_submit(char *record);

/* Two entry points, not one, so the adapter's choice of macro is observable: LOG_RAW is
 * documented to append nothing, LOG_PRINTK carries printk newline semantics. The fixture
 * models the resulting byte streams separately. */
#define LOG_RAW(format_, record_) fake_log_raw_submit(record_)
#define LOG_PRINTK(format_, record_) fake_log_printk_submit(record_)

#endif
