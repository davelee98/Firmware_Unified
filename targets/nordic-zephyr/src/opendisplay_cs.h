#ifndef OPENDISPLAY_CS_H
#define OPENDISPLAY_CS_H

#include <stdbool.h>
#include <zephyr/bluetooth/bluetooth.h>

struct bt_conn;
struct GlobalConfig;

bool opendisplay_cs_config_enabled(const struct GlobalConfig *cfg);
unsigned opendisplay_cs_scan_response_count(const struct GlobalConfig *cfg);
void opendisplay_cs_fill_scan_response(const struct GlobalConfig *cfg,
				       struct bt_data *out, unsigned max_entries,
				       unsigned *count_out);
void opendisplay_cs_on_connected(struct bt_conn *conn);
void opendisplay_cs_on_disconnected(struct bt_conn *conn);

#endif
