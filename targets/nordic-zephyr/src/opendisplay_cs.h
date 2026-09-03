#ifndef OPENDISPLAY_CS_H
#define OPENDISPLAY_CS_H

#include <stdbool.h>
#include <zephyr/bluetooth/bluetooth.h>

struct bt_conn;
struct od_config;

bool opendisplay_cs_config_enabled(const struct od_config *cfg);
void opendisplay_cs_fill_scan_response(const struct od_config *cfg,
				       struct bt_data *out, unsigned max_entries,
				       unsigned *count_out);
void opendisplay_cs_on_connected(struct bt_conn *conn);
void opendisplay_cs_on_disconnected(struct bt_conn *conn);

#endif
