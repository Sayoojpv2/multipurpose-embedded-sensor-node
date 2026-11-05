/*
 * test_firmware.c - unit and integration tests for the firmware core,
 * run natively against the virtual sensor backend. No external framework.
 */
#include "node.h"
#include "hal.h"
#include "sensors/virtual_sensors.h"
#include "radio/radio_sim.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void target_console_write(const uint8_t *d, size_t n) { fwrite(d, 1, n, stdout); }

static int g_pass, g_fail;
static const char *g_group = "";
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("  FAIL [%s] %s:%d  %s\n", g_group, __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))
#define GROUP(name) do { g_group = name; printf("- %s\n", name); } while (0)

/* ---------- synthetic environment trace ---------- */
#define ROWS 4320           /* 12 h at 10 s */
#define DT   10
static float   tv[MAX_CHANNELS][ROWS];
static uint8_t tf[MAX_CHANNELS][ROWS];
static uint8_t tl[ROWS];
static env_trace_t trace;

static void trace_const(const float *v)
{
    for (int c = 0; c < MAX_CHANNELS; c++)
        for (int r = 0; r < ROWS; r++) { tv[c][r] = v[c]; tf[c][r] = 0; }
    for (int r = 0; r < ROWS; r++) tl[r] = 1;
    trace.n_rows = ROWS; trace.dt_s = DT;
    for (int c = 0; c < MAX_CHANNELS; c++) { trace.v[c] = tv[c]; trace.fault[c] = tf[c]; }
    trace.link = tl;
    env_set_trace(&trace);
}

static const float NOM_WATER[4] = {7.4f, 15.0f, 450.0f, 26.0f};
static const float NOM_AIR[4]   = {25.0f, 600.0f, 65.0f, 28.0f};
static const float NOM_SOIL[4]  = {30.0f, 24.0f, 1200.0f, 20000.0f};

static const float *nominal(uint8_t pid) { return pid == PROFILE_WATER ? NOM_WATER : pid == PROFILE_AIR ? NOM_AIR : NOM_SOIL; }

/* ---------- gateway capture ---------- */
static packet_t g_last;
static uint32_t g_rx, g_alerts, g_stored;
static void gw_cb(void *ctx, uint32_t t, const packet_t *p, uint8_t len)
{
    (void)ctx; (void)t; (void)len;
    g_last = *p; g_rx++;
    if (p->type == PKT_ALERT) g_alerts++;
    if (p->flags & PKT_FLAG_STORED) g_stored++;
}

/* read one channel through the real driver path */
static sensor_reading_t read_channel(const app_profile_t *p, int ch)
{
    sensor_manager_t sm;
    sensor_reading_t rd[MAX_CHANNELS];
    sensor_manager_init(&sm, p);
    sensor_manager_sample(&sm, hal_timer_now_s(), rd);
    sensor_manager_release(&sm, 0);
    return rd[ch];
}

typedef struct { uint32_t samples, packets, delivered, attempts; double energy; float airtime; uint32_t gw_rx; } run_stats_t;

static log_record_t g_mem[2048];
static run_stats_t run_node(uint8_t pid, uint8_t strat, uint32_t seed, float loss, node_t *keep)
{
    static node_t n;
    static storage_t st; storage_backend_t be;
    storage_ram_backend(&be, g_mem);
    storage_init(&st, &be, 2048);
    vdev_load_board(pid);
    radio_sim_cfg_t rc = {loss, loss, 0.0f, seed};
    radio_sim_init(&rc);
    g_rx = g_alerts = g_stored = 0;
    gateway_init(gw_cb, NULL);
    node_config_t c; node_default_config(&c);
    c.profile_id = pid; c.strategy = strat; c.seed = seed;
    node_init(&n, &c, &st);
    uint32_t t = 0, dur = ROWS * DT;
    while (t < dur) { hal_sim_set_time(t); t = node_step(&n, t); }
    node_finish(&n, dur);
    run_stats_t s = {n.n_samples, n.n_packets, n.link.st.delivered, n.link.st.attempts, energy_total_mas(&n.e), n.link.st.airtime_ms, g_rx};
    if (keep) *keep = n;
    return s;
}

/* =================================================================== */
static void test_profiles(void)
{
    GROUP("profile selection");
    CHECK(profile_count() == 3);
    CHECK(profile_find("water") && profile_find("water")->id == PROFILE_WATER);
    CHECK(profile_get(PROFILE_AIR) && strcmp(profile_get(PROFILE_AIR)->name, "air") == 0);
    CHECK(profile_find("ocean") == NULL && profile_get(99) == NULL);
    for (int i = 0; i < profile_count(); i++) {
        const app_profile_t *p = profile_at(i);
        CHECK(p->n_channels == 4);
        for (int c = 0; c < p->n_channels; c++) CHECK(p->ch[c].driver != NULL && p->ch[c].pkt_scale > 0);
        CHECK(p->sampling.alert_s < p->sampling.active_s && p->sampling.active_s < p->sampling.stable_s);
    }
    /* driver selection differs per profile; the EC driver is shared by water and soil */
    CHECK(PROFILE_WATER_CFG.ch[0].driver == &DRV_PH_ANALOG);
    CHECK(PROFILE_AIR_CFG.ch[0].driver == &DRV_PM_UART);
    CHECK(PROFILE_WATER_CFG.ch[2].driver == PROFILE_SOIL_CFG.ch[2].driver);
    CHECK(PROFILE_WATER_CFG.ch[2].cal.p[0] != PROFILE_SOIL_CFG.ch[2].cal.p[0]);
}

static void test_registration_and_drivers(void)
{
    GROUP("sensor registration + driver interface");
    CHECK(sensor_driver_find("rtd_spi") == &DRV_RTD_SPI);
    CHECK(sensor_driver_find("nope") == NULL);
    for (int i = 0; i < profile_count(); i++) {
        const app_profile_t *p = profile_at(i);
        sensor_manager_t sm;
        trace_const(nominal(p->id));
        vdev_load_board(p->id);
        CHECK(sensor_manager_init(&sm, p) == 4);
        vdev_load_board(0);                          /* no board: digital sensors must not answer */
        int n = sensor_manager_init(&sm, p);
        CHECK(n < 4);
    }
    /* every channel decodes the virtual device back to the physical value */
    const float tol[3][4] = {{0.02f, 1.5f, 3.0f, 0.1f}, {1.0f, 1.0f, 0.01f, 0.01f}, {0.1f, 0.1f, 5.0f, 2.0f}};
    for (int i = 0; i < profile_count(); i++) {
        const app_profile_t *p = profile_at(i);
        trace_const(nominal(p->id));
        vdev_load_board(p->id);
        hal_sim_set_time(100);
        for (int c = 0; c < 4; c++) {
            sensor_reading_t r = read_channel(p, c);
            CHECK(r.status == RD_OK);
            NEAR(r.value, nominal(p->id)[c], tol[i][c]);
            CHECK(r.sensor_id == p->ch[c].sensor_id && r.unit == p->ch[c].unit && r.timestamp == 100);
        }
    }
}

static void test_adc_and_calibration(void)
{
    GROUP("ADC conversion + calibration");
    float v[4] = {7.0f, 15.0f, 450.0f, 35.0f};
    trace_const(v);
    vdev_load_board(PROFILE_WATER);
    vdev_set_rail(0, true);
    uint16_t raw;
    CHECK(hal_adc_read(0, &raw) == HAL_OK);
    NEAR(raw, lrintf(1.680f / 3.3f * 4095.0f), 1);   /* pH 7 -> Vmid */
    vdev_set_rail(0, false);
    hal_adc_read(0, &raw);
    CHECK(raw == 0);                                  /* unpowered sensor reads 0 V */

    /* two-point calibration matters: nominal Nernst parameters give an error */
    float vv[4] = {5.0f, 15.0f, 450.0f, 35.0f};
    trace_const(vv);
    sensor_reading_t ok = read_channel(&PROFILE_WATER_CFG, 0);
    NEAR(ok.value, 5.0f, 0.02f);
    app_profile_t bad = PROFILE_WATER_CFG;
    bad.ch[0].cal.p[0] = 1.65f; bad.ch[0].cal.p[1] = 3.0f * 0.05916f;
    sensor_reading_t un = read_channel(&bad, 0);
    CHECK(fabsf(un.value - 5.0f) > 0.1f);

    /* EC temperature compensation (water at 35 C) */
    sensor_reading_t ec = read_channel(&PROFILE_WATER_CFG, 2);
    NEAR(ec.value, 450.0f, 4.5f);
    float raw_ec;
    DRV_EC_ADS_I2C.convert(&PROFILE_WATER_CFG.ch[2], ec.raw, NAN, &raw_ec);   /* no compensation */
    CHECK(raw_ec > 450.0f * 1.15f);

    /* NTC (Beta) and RTD (Callendar-Van Dusen) non-linear conversions */
    float s[4] = {30.0f, 5.0f, 1200.0f, 1000.0f};
    trace_const(s);
    vdev_load_board(PROFILE_SOIL);
    NEAR(read_channel(&PROFILE_SOIL_CFG, 1).value, 5.0f, 0.1f);
    float w[4] = {7.0f, 15.0f, 450.0f, 70.0f};
    trace_const(w);
    vdev_load_board(PROFILE_WATER);
    NEAR(read_channel(&PROFILE_WATER_CFG, 3).value, 70.0f, 0.1f);
}

static void test_invalid_readings(void)
{
    GROUP("invalid readings / fault status");
    sensor_channel_cfg_t c = PROFILE_WATER_CFG.ch[0];
    CHECK(sensor_validate(&c, 7.0f) == RD_OK);
    CHECK(sensor_validate(&c, 15.0f) == RD_OUT_OF_RANGE);
    CHECK(sensor_validate(&c, NAN) == RD_OUT_OF_RANGE);

    trace_const(NOM_WATER);
    vdev_load_board(PROFILE_WATER);
    for (int r = 0; r < ROWS; r++) { tf[0][r] = 2; tf[1][r] = 1; tf[2][r] = 1; tf[3][r] = 2; }
    hal_sim_set_time(50);
    CHECK(read_channel(&PROFILE_WATER_CFG, 0).status == RD_RAIL);         /* stuck ADC */
    CHECK(read_channel(&PROFILE_WATER_CFG, 1).status == RD_RAIL);         /* open input -> 0 V */
    CHECK(read_channel(&PROFILE_WATER_CFG, 2).status == RD_NOT_AVAILABLE); /* I2C NACK at probe */
    CHECK(read_channel(&PROFILE_WATER_CFG, 3).status == RD_RAIL);         /* RTD fault bit */

    trace_const(NOM_AIR);
    vdev_load_board(PROFILE_AIR);
    for (int r = 0; r < ROWS; r++) { tf[0][r] = 2; tf[2][r] = 2; }
    CHECK(read_channel(&PROFILE_AIR_CFG, 0).status == RD_CHECKSUM);       /* UART frame checksum */
    CHECK(read_channel(&PROFILE_AIR_CFG, 2).status == RD_CHECKSUM);       /* SHT CRC-8 on RH word */
    CHECK(read_channel(&PROFILE_AIR_CFG, 3).status == RD_OK);             /* T word still fine */
    for (int r = 0; r < ROWS; r++) tf[0][r] = 1;
    CHECK(read_channel(&PROFILE_AIR_CFG, 0).status == RD_NO_RESPONSE);    /* UART timeout */
}

static log_record_t mem4[4];
static void test_buffers(void)
{
    GROUP("circular storage + TX queue");
    storage_t s; storage_backend_t be;
    storage_ram_backend(&be, mem4);
    storage_init(&s, &be, 4);
    for (int i = 0; i < 6; i++) { log_record_t r; memset(&r, 0, sizeof r); r.ts = (uint32_t)(100 * i); storage_append(&s, &r); }
    CHECK(s.count == 4 && s.overwritten == 2);
    CHECK(storage_oldest_seq(&s) == 3);
    log_record_t o;
    CHECK(storage_get_by_seq(&s, 2, &o) != 0);
    CHECK(storage_get_by_seq(&s, 6, &o) == 0 && o.ts == 500 && o.seq == 6);
    log_record_t out[8];
    CHECK(storage_read_range(&s, 1, out, 8) == 4 && out[0].seq == 3 && out[3].seq == 6);

    /* queue overflow keeps the most important packets */
    link_t l; link_cfg_t lc; lora_cfg_t lo;
    link_default_cfg(&lc); lora_default_cfg(&lo);
    link_init(&l, &lc, &lo, 1);
    uint8_t buf[20] = {0};
    for (int i = 0; i < TXQ_CAP + 5; i++) link_submit(&l, buf, 20, PRIO_LOW, (uint16_t)i, PKT_SUMMARY, (uint32_t)i);
    CHECK(l.count == TXQ_CAP && l.st.dropped_queue == 5);
    CHECK(link_submit(&l, buf, 20, PRIO_CRITICAL, 999, PKT_ALERT, 1000) == 0);
    int found = 0;
    for (int i = 0; i < l.count; i++) if (l.q[i].seq == 999) found = 1;
    CHECK(found && l.count == TXQ_CAP);
}

static void test_filters(void)
{
    GROUP("filters + outlier detection");
    ma_filter_t ma; ma_init(&ma, 4);
    NEAR(ma_update(&ma, 2.0f), 2.0f, 1e-6);            /* primed with first value */
    ma_update(&ma, 6.0f); ma_update(&ma, 6.0f);
    NEAR(ma_update(&ma, 6.0f), 5.0f, 1e-5);            /* (2+6+6+6)/4 */
    NEAR(ma_update(&ma, 6.0f), 6.0f, 1e-5);

    fir_filter_t f; fir_init(&f, FIR_BINOMIAL7, 7);
    fir_update(&f, 0.0f);
    for (int k = 0; k < 7; k++) NEAR(fir_update(&f, k == 0 ? 1.0f : 0.0f), FIR_BINOMIAL7[k], 1e-7); /* impulse response */
    fir_init(&f, FIR_BINOMIAL7, 7);
    float y = 0; for (int k = 0; k < 20; k++) y = fir_update(&f, 3.0f);
    NEAR(y, 3.0f, 1e-5);                                /* unity DC gain */

    float v5[5] = {5, 1, 4, 2, 3};
    NEAR(dsp_median(v5, 5), 3.0f, 0);
    hampel_t h; hampel_init(&h, 3.0f, 0.5f);
    bool out;
    for (int k = 0; k < 5; k++) hampel_update(&h, 10.0f + 0.1f * (k % 2), &out);
    float r = hampel_update(&h, 50.0f, &out);
    CHECK(out && r < 11.0f);                            /* isolated spike replaced by median */
    hampel_update(&h, 10.0f, &out);
    CHECK(!out);
    hampel_init(&h, 3.0f, 0.5f);
    for (int k = 0; k < 5; k++) hampel_update(&h, 10.0f, &out);
    int accepted_at = -1;
    for (int k = 0; k < 5; k++) { float z = hampel_update(&h, 20.0f, &out); if (!out && z > 19.0f && accepted_at < 0) accepted_at = k; }
    CHECK(accepted_at == 2);                            /* genuine step accepted after 3 samples */
}

static void test_events(void)
{
    GROUP("event detection + hysteresis");
    thresholds_t t = {THR_NONE_LO, 50.0f, THR_NONE_LO, 100.0f, 5.0f};
    event_cfg_t c = {2, 3, 3};
    chan_event_t e; event_init(&e);
    CHECK(event_update(&e, &t, &c, true, 20) == EV_NORMAL);
    CHECK(event_update(&e, &t, &c, true, 120) == EV_NORMAL);   /* one sample: not confirmed */
    CHECK(event_pending_level(&e, 2));
    CHECK(event_update(&e, &t, &c, true, 20) == EV_NORMAL);    /* spike, counter reset */
    event_update(&e, &t, &c, true, 120);
    CHECK(event_update(&e, &t, &c, true, 120) == EV_CRITICAL); /* confirmed */
    CHECK(event_update(&e, &t, &c, true, 97) == EV_CRITICAL);  /* inside hysteresis band */
    CHECK(event_update(&e, &t, &c, true, 97) == EV_CRITICAL);
    event_update(&e, &t, &c, true, 70);
    CHECK(event_update(&e, &t, &c, true, 70) == EV_WARNING);   /* de-escalation confirmed */
    event_update(&e, &t, &c, true, 30);
    CHECK(event_update(&e, &t, &c, true, 30) == EV_RECOVERY);
    event_update(&e, &t, &c, true, 30); event_update(&e, &t, &c, true, 30);
    CHECK(event_update(&e, &t, &c, true, 30) == EV_NORMAL);    /* recover_n normal samples */
    /* fault and recovery from fault */
    event_update(&e, &t, &c, false, NAN); event_update(&e, &t, &c, false, NAN);
    CHECK(e.state == EV_NORMAL);
    CHECK(event_update(&e, &t, &c, false, NAN) == EV_FAULT);
    CHECK(event_update(&e, &t, &c, true, 30) == EV_RECOVERY);
    /* node state = most severe channel */
    chan_event_t ev[3]; event_init(&ev[0]); event_init(&ev[1]); event_init(&ev[2]);
    ev[1].state = EV_FAULT; ev[2].state = EV_WARNING;
    CHECK(event_node_state(ev, 3) == EV_FAULT);
    ev[0].state = EV_CRITICAL;
    CHECK(event_node_state(ev, 3) == EV_CRITICAL);
    /* without hysteresis the same oscillation chatters */
    thresholds_t t0 = t; t0.hyst = 0;
    chan_event_t a, b; event_init(&a); event_init(&b);
    float seq[] = {120, 120, 98, 101, 98, 101, 98, 101, 98, 98, 101, 98};
    for (unsigned k = 0; k < sizeof seq / sizeof seq[0]; k++) { event_update(&a, &t, &c, true, seq[k]); event_update(&b, &t0, &c, true, seq[k]); }
    CHECK(a.transitions == 1 && b.transitions > a.transitions);
}

static void test_sampling(void)
{
    GROUP("adaptive sampling");
    const sampling_cfg_t *c = &PROFILE_WATER_CFG.sampling;
    sampler_t s; sampler_init(&s, STRAT_ADAPTIVE);
    sample_cues_t q; memset(&q, 0, sizeof q);
    uint32_t iv = 0;
    for (int k = 0; k < c->stable_confirm; k++) iv = sampler_next(&s, c, STRAT_ADAPTIVE, &q);
    CHECK(iv == c->stable_s && s.mode == SM_STABLE);
    q.suspect = true;
    CHECK(sampler_next(&s, c, STRAT_ADAPTIVE, &q) == c->active_s);
    q.suspect = false; q.pending_crit = true;
    CHECK(sampler_next(&s, c, STRAT_ADAPTIVE, &q) == c->alert_s);
    q.pending_crit = false; q.node_state = EV_RECOVERY;
    CHECK(sampler_next(&s, c, STRAT_ADAPTIVE, &q) == c->recovery_s);
    q.node_state = EV_NORMAL;
    CHECK(sampler_next(&s, c, STRAT_ADAPTIVE, &q) != c->stable_s);   /* needs confirmation */
    sampler_t f; sampler_init(&f, STRAT_FIXED);
    q.node_state = EV_CRITICAL;
    CHECK(sampler_next(&f, c, STRAT_FIXED, &q) == c->fixed_s);
    CHECK(PROFILE_SOIL_CFG.sampling.stable_s != PROFILE_WATER_CFG.sampling.stable_s);   /* profile specific */
}

static void test_packets_crc(void)
{
    GROUP("packet encode/decode + CRC");
    CHECK(crc16_ccitt_false((const uint8_t *)"123456789", 9) == 0x29B1);
    uint8_t be[2] = {0xBE, 0xEF};
    CHECK(crc8_sensirion(be, 2) == 0x92);

    packet_t p; memset(&p, 0, sizeof p);
    p.type = PKT_DATA; p.node_id = 0x1234; p.profile_id = PROFILE_WATER; p.seq = 4321; p.timestamp = 86400;
    p.priority = PRIO_CRITICAL; p.node_state = EV_CRITICAL; p.mask = 0x0F;
    float vals[4] = {6.12f, 104.5f, 1234.0f, 25.37f};
    for (int i = 0; i < 4; i++) { p.ch[i].sensor_id = (uint8_t)(i + 1); p.ch[i].status = pkt_status_byte(EV_CRITICAL, true, 0); p.ch[i].value = vals[i]; }
    p.ch[2].value = NAN;
    uint8_t buf[PKT_MAX_LEN];
    int n = packet_encode(&p, buf, sizeof buf);
    CHECK(n == PKT_HDR_LEN + 4 * 4 + PKT_CRC_LEN);
    packet_t d;
    CHECK(packet_decode(buf, (size_t)n, &d) == 0);
    CHECK(d.node_id == 0x1234 && d.seq == 4321 && d.timestamp == 86400 && d.priority == PRIO_CRITICAL && d.node_state == EV_CRITICAL);
    NEAR(d.ch[0].value, 6.12f, 0.006); NEAR(d.ch[1].value, 104.5f, 0.06); NEAR(d.ch[3].value, 25.37f, 0.006);
    CHECK(isnan(d.ch[2].value));
    int detected = 0;
    for (int bit = 0; bit < n * 8; bit++) {          /* every single-bit error is caught */
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if (packet_decode(buf, (size_t)n, &d) != 0) detected++;
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    CHECK(detected == n * 8);
    p.type = PKT_SUMMARY; p.mask = 0x05; p.ch[0].vmin = 6.0f; p.ch[0].vmax = 6.3f;
    p.ch[2].value = 400; p.ch[2].vmin = 390; p.ch[2].vmax = 410;
    n = packet_encode(&p, buf, sizeof buf);
    CHECK(n == PKT_HDR_LEN + 2 * 8 + PKT_CRC_LEN);
    CHECK(packet_decode(buf, (size_t)n, &d) == 0 && d.mask == 0x05 && isnan(d.ch[1].value));
    NEAR(d.ch[0].vmax, 6.3f, 0.006); NEAR(d.ch[2].vmin, 390, 0.5);
    packet_set_flags(buf, (size_t)n, PKT_FLAG_STORED);
    CHECK(packet_decode(buf, (size_t)n, &d) == 0 && (d.flags & PKT_FLAG_STORED));

    lora_cfg_t lc; lora_default_cfg(&lc);
    lc.sf = 7;  NEAR(lora_airtime_ms(&lc, 13, true), 46.336, 0.01);
    lc.sf = 12; NEAR(lora_airtime_ms(&lc, 13, true), 1155.072, 0.01);
}

static void test_outage(void)
{
    GROUP("communication outage + store-and-forward");
    trace_const(NOM_WATER);
    for (int r = 0; r < ROWS; r++) tl[r] = (r * DT >= 3600 && r * DT < 7200) ? 0 : 1;   /* 1 h outage */
    node_t n;
    run_stats_t s = run_node(PROFILE_WATER, STRAT_FIXED, 3, 0.0f, &n);
    CHECK(n.link.st.outages >= 1);
    CHECK(n.link.st.recovered_after_outage >= 55);         /* ~60 packets queued during the hour */
    CHECK(s.delivered == s.packets && n.link.count == 0);  /* nothing lost after recovery */
    CHECK(g_stored >= 55);                                 /* gateway sees them flagged as stored */
    CHECK(n.link.st.max_queue >= 55);
    /* duplicates: lost ACKs cause retransmissions that the gateway de-duplicates */
    trace_const(NOM_WATER);
    run_node(PROFILE_WATER, STRAT_FIXED, 5, 0.3f, &n);
    CHECK(gateway_stats()->duplicates > 0 && g_rx == n.link.st.delivered);
}

static void test_fixed_vs_adaptive(void)
{
    GROUP("fixed vs adaptive telemetry");
    for (int i = 0; i < profile_count(); i++) {
        const app_profile_t *p = profile_at(i);
        trace_const(nominal(p->id));
        run_stats_t f = run_node(p->id, STRAT_FIXED, 1, 0.0f, NULL);
        run_stats_t a = run_node(p->id, STRAT_ADAPTIVE, 1, 0.0f, NULL);
        CHECK(f.packets == f.samples);
        CHECK(a.packets * 4 < f.packets);                  /* stable input: far fewer packets */
        CHECK(a.samples < f.samples && a.energy < f.energy && a.airtime < f.airtime);
    }
    /* critical step: adaptive sends an ALERT (priority 3) and records it locally */
    trace_const(NOM_WATER);
    for (int r = ROWS / 2; r < ROWS; r++) tv[0][r] = 5.5f;
    node_t n;
    run_node(PROFILE_WATER, STRAT_ADAPTIVE, 1, 0.0f, &n);
    CHECK(g_alerts >= 1);
    CHECK(n.storage->next_seq - 1 == n.n_samples);         /* every sample retained locally */
}

static void test_reproducibility(void)
{
    GROUP("reproducibility");
    trace_const(NOM_AIR);
    run_stats_t a = run_node(PROFILE_AIR, STRAT_ADAPTIVE, 7, 0.1f, NULL);
    run_stats_t b = run_node(PROFILE_AIR, STRAT_ADAPTIVE, 7, 0.1f, NULL);
    CHECK(a.samples == b.samples && a.packets == b.packets && a.attempts == b.attempts && a.energy == b.energy);
    run_stats_t c = run_node(PROFILE_AIR, STRAT_FIXED, 8, 0.1f, NULL);
    run_stats_t d = run_node(PROFILE_AIR, STRAT_FIXED, 9, 0.1f, NULL);
    CHECK(c.attempts != d.attempts);                       /* the seed really drives the loss model */
}

int main(void)
{
    printf("firmware unit tests (version %s)\n", FW_VERSION_STR);
    test_profiles();
    test_registration_and_drivers();
    test_adc_and_calibration();
    test_invalid_readings();
    test_buffers();
    test_filters();
    test_events();
    test_sampling();
    test_packets_crc();
    test_outage();
    test_fixed_vs_adaptive();
    test_reproducibility();
    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
