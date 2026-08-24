/* The od_hal_i2c contract, as an executable script.
 *
 * Included by one executable per implementation. Staging step 3 binds the reference engine;
 * steps 4, 5 and 9 bind ESP32's, Nordic's and BG22's production adapters to this same body, so
 * "the adapter satisfies the contract" is a claim something checks rather than one someone made.
 *
 * Every assertion here is about FRAMING or BUS SELECTION -- the two things an engine can get
 * wrong while still returning plausible bytes.
 */

/* The consumer supplies: a way to install a config, and CHECK/CASE from od_check.h. */

static void ct_bus(struct od_config *cfg, uint8_t slot, uint8_t instance,
                   uint8_t scl, uint8_t sda)
{
    cfg->data_buses[slot].instance_number = instance;
    cfg->data_buses[slot].bus_type = 0x01u;   /* I2C */
    cfg->data_buses[slot].pin_1 = scl;
    cfg->data_buses[slot].pin_2 = sda;
    if (slot >= cfg->data_bus_count) {
        cfg->data_bus_count = (uint8_t)(slot + 1u);
    }
}

static void test_write_ends_with_stop(void)
{
    static struct od_config cfg;
    const uint8_t tx[2] = { 0xAA, 0xBB };

    CASE("write completes with STOP");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);

    CHECK(od_hal_i2c_write(0, 0x44, tx, 2) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_len >= 5u);
    CHECK(i2c_wire_len >= 5u);
    if (i2c_wire_len >= 5u) {
        CHECK(i2c_wire_trace[0].ev == I2C_EV_START);
        CHECK(i2c_wire_trace[1].ev == I2C_EV_ADDR_W && i2c_wire_trace[1].arg == 0x44u);
        CHECK(i2c_wire_trace[2].ev == I2C_EV_TX && i2c_wire_trace[2].arg == 0xAAu);
        CHECK(i2c_wire_trace[3].ev == I2C_EV_TX && i2c_wire_trace[3].arg == 0xBBu);
    }
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);
    CHECK(i2c_wire_count(I2C_EV_RSTART) == 0u);
}

static void test_write_read_has_no_stop_between(void)
{
    static struct od_config cfg;
    const uint8_t sel = 0x08;
    uint8_t rx[2] = { 0, 0 };
    const uint8_t payload[2] = { 0x34, 0x12 };
    unsigned addr_r_at = 0;

    CASE("write_read is ONE transaction: repeated START, no STOP between the phases");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x55);
    i2c_wire_set_read_data(0x55, payload, 2);

    CHECK(od_hal_i2c_write_read(0, 0x55, &sel, 1, rx, 2) == OD_HAL_I2C_OK);
    CHECK(rx[0] == 0x34u && rx[1] == 0x12u);
    CHECK(i2c_wire_count(I2C_EV_RSTART) == 1u);
    /* The write phase has to actually happen: without these, an engine that emitted
     * START/RSTART/ADDR_R and never sent the selector satisfies every framing assertion. */
    CHECK(i2c_wire_len >= 3u);
    if (i2c_wire_len >= 3u) {
        CHECK(i2c_wire_trace[0].ev == I2C_EV_START);
        CHECK(i2c_wire_trace[1].ev == I2C_EV_ADDR_W && i2c_wire_trace[1].arg == 0x55u);
        CHECK(i2c_wire_trace[2].ev == I2C_EV_TX && i2c_wire_trace[2].arg == 0x08u);
    }
    CHECK(i2c_wire_count(I2C_EV_ADDR_W) == 1u && i2c_wire_count(I2C_EV_ADDR_R) == 1u);
    CHECK(i2c_wire_count(I2C_EV_RX) == 2u);
    for (unsigned i = 0; i < i2c_wire_len; ++i) {
        if (i2c_wire_trace[i].ev == I2C_EV_ADDR_R) { addr_r_at = i; break; }
    }
    CHECK(addr_r_at > 0u);
    /* The assertion the whole seam exists for. */
    CHECK(!i2c_wire_has_stop_between(0, addr_r_at));
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);
}

static void test_write_then_read_is_distinguishable(void)
{
    static struct od_config cfg;
    const uint8_t sel = 0x08;
    uint8_t rx[2] = { 0, 0 };
    const uint8_t payload[2] = { 0x34, 0x12 };
    unsigned addr_r_at = 0;

    /* Same bytes, different framing. A device needing the repeated START answers STOP-then-START
     * with whatever an unaddressed read yields -- plausible values, not an error -- so these two
     * must never collapse into one another. */
    CASE("write() then read() puts a STOP between, unlike write_read()");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x55);
    i2c_wire_set_read_data(0x55, payload, 2);

    CHECK(od_hal_i2c_write(0, 0x55, &sel, 1) == OD_HAL_I2C_OK);
    CHECK(od_hal_i2c_read(0, 0x55, rx, 2) == OD_HAL_I2C_OK);
    for (unsigned i = 0; i < i2c_wire_len; ++i) {
        if (i2c_wire_trace[i].ev == I2C_EV_ADDR_R) { addr_r_at = i; break; }
    }
    CHECK(addr_r_at > 0u);
    CHECK(i2c_wire_has_stop_between(0, addr_r_at));
    CHECK(i2c_wire_count(I2C_EV_RSTART) == 0u);
    CHECK(i2c_wire_count(I2C_EV_START) == 2u);
    /* Both transactions closed. Without this a read that never issues STOP passes, because the
     * preceding write's STOP already satisfied has_stop_between(). */
    CHECK(i2c_wire_count(I2C_EV_STOP) == 2u);
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);

    CASE("a standalone read is a complete START/address/read/STOP on its own");
    i2c_wire_reset();                       /* also drops the scripted devices */
    i2c_wire_add_device(10, 11, 0x55);
    i2c_wire_set_read_data(0x55, payload, 2);
    CHECK(od_hal_i2c_read(0, 0x55, rx, 2) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_len >= 2u);
    if (i2c_wire_len >= 2u) {
        CHECK(i2c_wire_trace[0].ev == I2C_EV_START);
        CHECK(i2c_wire_trace[1].ev == I2C_EV_ADDR_R && i2c_wire_trace[1].arg == 0x55u);
    }
    CHECK(i2c_wire_count(I2C_EV_RX) == 2u);
    CHECK(i2c_wire_count(I2C_EV_ADDR_W) == 0u);
    CHECK(i2c_wire_count(I2C_EV_STOP) == 1u);
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);
}

static void test_probe_is_address_only(void)
{
    static struct od_config cfg;

    CASE("probe drives an address and no data phase");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x5D);

    CHECK(od_hal_i2c_probe(0, 0x5D) == OD_HAL_I2C_OK);
    /* It has to reach the wire: an implementation answering from a table of known addresses
     * would pass a test that only checked for the absence of a data phase. */
    CHECK(i2c_wire_len >= 3u);
    if (i2c_wire_len >= 3u) {
        CHECK(i2c_wire_trace[0].ev == I2C_EV_START);
        CHECK(i2c_wire_trace[1].ev == I2C_EV_ADDR_W && i2c_wire_trace[1].arg == 0x5Du);
    }
    CHECK(i2c_wire_count(I2C_EV_TX) == 0u);
    CHECK(i2c_wire_count(I2C_EV_RX) == 0u);
    CHECK(i2c_wire_count(I2C_EV_STOP) == 1u);
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);

    CASE("an absent address is ENODEV, and the bus is still released");
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x5D);
    CHECK(od_hal_i2c_probe(0, 0x21) == OD_HAL_I2C_ENODEV);
    CHECK(i2c_wire_count(I2C_EV_ADDR_W) == 1u);
    CHECK(i2c_wire_count(I2C_EV_STOP) == 1u);
}

static void test_failures_stay_distinct(void)
{
    static struct od_config cfg;
    const uint8_t tx[1] = { 0x01 };
    uint8_t rx[1] = { 0 };
    int r_nodev, r_eio, r_inval;

    CASE("the header's numbering is what implementations are compiled against");
    CHECK(OD_HAL_I2C_OK == 0);
    CHECK(OD_HAL_I2C_EIO == -1);
    CHECK(OD_HAL_I2C_ENODEV == -2);
    CHECK(OD_HAL_I2C_EINVAL == -3);

    CASE("address NACK, data failure and an unusable bus are three DIFFERENT answers");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);

    r_nodev = od_hal_i2c_write(0, 0x33, tx, 1);     /* nobody there */
    i2c_wire_fail_next_data();
    r_eio = od_hal_i2c_write(0, 0x44, tx, 1);       /* fault mid-data */
    r_inval = od_hal_i2c_write(7, 0x44, tx, 1);     /* no such bus */

    CHECK(r_nodev == OD_HAL_I2C_ENODEV);
    CHECK(r_eio == OD_HAL_I2C_EIO);
    CHECK(r_inval == OD_HAL_I2C_EINVAL);
    /* Pairwise, against the VALUES returned: comparing each to its own macro still passes if two
     * macros collapse to the same number, which is the thing "distinct" is supposed to forbid. */
    CHECK(r_nodev != r_eio);
    CHECK(r_eio != r_inval);
    CHECK(r_nodev != r_inval);
    CHECK(r_nodev != OD_HAL_I2C_OK && r_eio != OD_HAL_I2C_OK && r_inval != OD_HAL_I2C_OK);
    CHECK(od_hal_i2c_read(7, 0x44, rx, 1) == OD_HAL_I2C_EINVAL);

    CASE("every failure that reached the wire still released it");
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    CHECK(od_hal_i2c_write(0, 0x33, tx, 1) == OD_HAL_I2C_ENODEV);
    CHECK(i2c_wire_count(I2C_EV_STOP) == i2c_wire_count(I2C_EV_START));
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);

    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    i2c_wire_fail_next_data();
    CHECK(od_hal_i2c_write(0, 0x44, tx, 1) == OD_HAL_I2C_EIO);
    CHECK(i2c_wire_count(I2C_EV_STOP) == i2c_wire_count(I2C_EV_START));
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);

    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    i2c_wire_fail_next_data();
    CHECK(od_hal_i2c_write_read(0, 0x44, tx, 1, rx, 1) == OD_HAL_I2C_EIO);
    CHECK(i2c_wire_count(I2C_EV_STOP) == 1u);
    CHECK(i2c_wire_last_ev() == I2C_EV_STOP);

    CASE("bad arguments are refused without touching the bus");
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    CHECK(od_hal_i2c_write(0, 0x80u, tx, 1) == OD_HAL_I2C_EINVAL);   /* 8-bit address */
    CHECK(od_hal_i2c_write(0, 0x44, NULL, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write(0, 0x44, tx, 0) == OD_HAL_I2C_EINVAL);    /* not a presence check */
    CHECK(od_hal_i2c_read(0, 0x44, NULL, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_read(0, 0x44, rx, 0) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write_read(0, 0x44, tx, 0, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write_read(0, 0x44, tx, 1, rx, 0) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_probe(0, 0x80u) == OD_HAL_I2C_EINVAL);
    CHECK(i2c_wire_len == 0u);
}

static void test_bus_is_keyed_by_instance_number(void)
{
    static struct od_config cfg;
    const uint8_t tx[1] = { 0x01 };

    /* Records deliberately out of order, so index and instance disagree. An engine that indexed
     * data_buses[] would drive the other bus's pins and still return OK. */
    CASE("out-of-order records: the operation reaches the bus named by instance_number");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 1, 20, 21);   /* instance 1 arrived first */
    ct_bus(&cfg, 1, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    i2c_wire_add_device(20, 21, 0x44);

    CHECK(od_hal_i2c_write(0, 0x44, tx, 1) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 10u && i2c_wire_last_sda == 11u);
    CHECK(od_hal_i2c_write(1, 0x44, tx, 1) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 20u && i2c_wire_last_sda == 21u);

    CASE("sparse records: an unnamed instance is EINVAL, not the nearest bus");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 2, 30, 31);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(30, 31, 0x44);
    CHECK(od_hal_i2c_write(0, 0x44, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write(2, 0x44, tx, 1) == OD_HAL_I2C_OK);

    CASE("a duplicated instance is ambiguous, so no bus is driven");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 1, 40, 41);
    ct_bus(&cfg, 1, 1, 50, 51);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(40, 41, 0x44);
    CHECK(od_hal_i2c_write(1, 0x44, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(i2c_wire_len == 0u);   /* refused BEFORE touching hardware */
}

static void test_every_operation_selects_by_instance(void)
{
    static struct od_config cfg;
    const uint8_t tx[1] = { 0x01 };
    const uint8_t payload[1] = { 0x5A };
    uint8_t rx[1] = { 0 };

    /* write() alone proves nothing about the other three: each could index data_buses[],
     * special-case 0xFF, or reach hardware before refusing an ambiguous bus, independently. */
    CASE("probe, read and write_read select by instance_number too");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 1, 20, 21);   /* out of order, so index != instance */
    ct_bus(&cfg, 1, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    i2c_wire_add_device(20, 21, 0x44);
    i2c_wire_set_read_data(0x44, payload, 1);

    CHECK(od_hal_i2c_probe(0, 0x44) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 10u);
    CHECK(od_hal_i2c_probe(1, 0x44) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 20u);

    CHECK(od_hal_i2c_read(0, 0x44, rx, 1) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 10u);
    CHECK(od_hal_i2c_read(1, 0x44, rx, 1) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 20u);

    CHECK(od_hal_i2c_write_read(0, 0x44, tx, 1, rx, 1) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 10u);
    CHECK(od_hal_i2c_write_read(1, 0x44, tx, 1, rx, 1) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 20u);

    CASE("and every one of them refuses an absent, ambiguous or unassigned bus");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 1, 40, 41);
    ct_bus(&cfg, 1, 1, 50, 51);   /* instance 1 declared twice */
    ct_bus(&cfg, 2, 3, 60, 61);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(40, 41, 0x44);

    CHECK(od_hal_i2c_probe(1, 0x44) == OD_HAL_I2C_EINVAL);            /* ambiguous */
    CHECK(od_hal_i2c_read(1, 0x44, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write_read(1, 0x44, tx, 1, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_probe(9, 0x44) == OD_HAL_I2C_EINVAL);            /* absent */
    CHECK(od_hal_i2c_read(9, 0x44, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write_read(9, 0x44, tx, 1, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_probe(0xFFu, 0x44) == OD_HAL_I2C_EINVAL);        /* no 0xFF record here */
    CHECK(od_hal_i2c_read(0xFFu, 0x44, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write_read(0xFFu, 0x44, tx, 1, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(i2c_wire_len == 0u);   /* none of them reached hardware first */
}

static void test_sentinel_is_not_the_transports_rule(void)
{
    static struct od_config cfg;
    const uint8_t tx[1] = { 0x01 };

    /* 0xFF means "unconfigured" to a SENSOR, and shared sensor/touch policy refuses it. The
     * transport must not encode that rule: a record genuinely numbered 0xFF resolves, and a
     * 0xFF with no such record is EINVAL for the ordinary reason, not a special case. */
    CASE("0xFF is passed through literally, not special-cased in the transport");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0xFFu, 60, 61);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(60, 61, 0x44);
    CHECK(od_hal_i2c_write(0xFFu, 0x44, tx, 1) == OD_HAL_I2C_OK);
    CHECK(i2c_wire_last_scl == 60u);

    CASE("and it is never silently resolved to bus 0");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0, 10, 11);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    CHECK(od_hal_i2c_write(0xFFu, 0x44, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(i2c_wire_len == 0u);
}

static void test_no_ownership_is_retained(void)
{
    static struct od_config cfg;
    const uint8_t tx[1] = { 0x01 };

    /* Alternating buses back to back. An engine that cached one and forgot to switch would pass
     * every framing assertion above and drive the wrong pins here. */
    CASE("alternating buses: each call selects its own, and none keeps the bus afterwards");
    od_config_reset(&cfg);
    ct_bus(&cfg, 0, 0, 10, 11);
    ct_bus(&cfg, 1, 1, 20, 21);
    i2c_contract_install(&cfg);
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x44);
    i2c_wire_add_device(20, 21, 0x44);

    for (unsigned round = 0; round < 3u; ++round) {
        CHECK(od_hal_i2c_write(0, 0x44, tx, 1) == OD_HAL_I2C_OK);
        CHECK(i2c_wire_last_scl == 10u);
        CHECK(od_hal_i2c_write(1, 0x44, tx, 1) == OD_HAL_I2C_OK);
        CHECK(i2c_wire_last_scl == 20u);
    }
    /* Every transaction ended: as many STOPs as STARTs. */
    CHECK(i2c_wire_count(I2C_EV_STOP) == i2c_wire_count(I2C_EV_START));
}

static void od_i2c_contract_run(void)
{
    test_write_ends_with_stop();
    test_write_read_has_no_stop_between();
    test_write_then_read_is_distinguishable();
    test_probe_is_address_only();
    test_failures_stay_distinct();
    test_bus_is_keyed_by_instance_number();
    test_every_operation_selects_by_instance();
    test_sentinel_is_not_the_transports_rule();
    test_no_ownership_is_retained();
}
