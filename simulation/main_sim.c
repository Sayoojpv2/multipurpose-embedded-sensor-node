/*
 * main_sim.c - native software-in-the-loop harness.
 *
 * Runs the unchanged firmware core (firmware/) against the virtual sensor
 * backend and simulated radio/gateway, over a scenario trace, and writes
 * CSV/JSON results. This file is host-only (stdio, malloc are allowed here,
 * never inside firmware/).
 *
 *   sensor_node --profile water --strategy adaptive --trace t.csv --out dir [options]
 *   sensor_node --config node.ini
 *   sensor_node --list-profiles | --dump-profile water
 *   sensor_node --filter-test in.csv out.csv
 */
#include "node.h"
#include "hal.h"
#include "sensors/virtual_sensors.h"
#include "radio/radio_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <time.h>

void target_console_write(const uint8_t *d, size_t n) { fwrite(d, 1, n, stdout); }

/* ---------------- options ---------------- */
typedef struct {
    char profile[16], strategy[16], trace[512], out[512], flash[512];
    unsigned seed, node_id, storage_cap, stable_s, sf;
    double loss, ack_loss, corrupt;
} opts_t;

static void set_opt(opts_t *o, const char *k, const char *v)
{
    if (!strcmp(k, "profile")) snprintf(o->profile, sizeof o->profile, "%s", v);
    else if (!strcmp(k, "strategy")) snprintf(o->strategy, sizeof o->strategy, "%s", v);
    else if (!strcmp(k, "trace")) snprintf(o->trace, sizeof o->trace, "%s", v);
    else if (!strcmp(k, "out")) snprintf(o->out, sizeof o->out, "%s", v);
    else if (!strcmp(k, "flash")) snprintf(o->flash, sizeof o->flash, "%s", v);
    else if (!strcmp(k, "seed")) o->seed = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "node-id") || !strcmp(k, "node_id")) o->node_id = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "storage-cap") || !strcmp(k, "storage_cap")) o->storage_cap = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "stable-s") || !strcmp(k, "stable_s")) o->stable_s = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "sf")) o->sf = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "loss")) o->loss = atof(v);
    else if (!strcmp(k, "ack-loss") || !strcmp(k, "ack_loss")) o->ack_loss = atof(v);
    else if (!strcmp(k, "corrupt")) o->corrupt = atof(v);
    else fprintf(stderr, "warning: unknown option '%s'\n", k);
}

static int load_ini(opts_t *o, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open config %s\n", path); return -1; }
    char line[600];
    while (fgets(line, sizeof line, f)) {
        char *h = strpbrk(line, "#;\r\n"); if (h) *h = 0;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        while (*v == ' ') v++;
        for (char *e = k + strlen(k); e > k && e[-1] == ' '; ) *--e = 0;
        for (char *e = v + strlen(v); e > v && e[-1] == ' '; ) *--e = 0;
        set_opt(o, k, v);
    }
    fclose(f);
    return 0;
}

/* ---------------- trace loading ---------------- */
static env_trace_t g_trace;

#define TRACE_MAX_COLS 64
/* Upper limit for any trace time (and the trace duration). Times are held in
 * uint32_t; staying below 2^31 s leaves room for t + interval in node_step(). */
#define TRACE_MAX_TIME_S 2147483647.0

static void free_trace(void)
{
    for (int i = 0; i < MAX_CHANNELS; i++) { free((void *)g_trace.v[i]); free((void *)g_trace.fault[i]); }
    free((void *)g_trace.link);
    memset(&g_trace, 0, sizeof g_trace);
}

/* Split one CSV line into numbers. Empty fields become NaN (missing value).
 * Returns the number of fields, -1 if a field is not a number or there are
 * too many fields, or -2 if a field is an explicit inf/nan. */
static int parse_row(char *line, double *vals, int max)
{
    int n = 0;
    char *p = line;
    for (;;) {
        if (n == max) return -1;
        while (*p == ' ') p++;
        if (*p == ',' || *p == '\0' || *p == '\r' || *p == '\n') {
            vals[n++] = NAN;
        } else {
            char *e;
            vals[n++] = strtod(p, &e);
            if (e == p) return -1;
            if (!isfinite(vals[n - 1])) return -2;
            while (*e == ' ') e++;
            if (*e != ',' && *e != '\0' && *e != '\r' && *e != '\n') return -1;
            p = e;
        }
        p = strchr(p, ',');
        if (!p) return n;
        p++;
    }
}

static bool is_code(double x, int max) { return x == x && x >= 0 && x <= max && x == (int)x; }

/* Loads a trace CSV (t, v0..v3, f0..f3, link, other columns ignored).
 * Requirements: a t column starting at 0 with a constant positive integer
 * step (all times below 2^31 s), at least one v column, every row with the
 * same number of fields as the header, fault codes 0/1/2 and link 0/1.
 * Fields must be finite numbers; an empty sensor value is a missing reading
 * (fault 1). Any violation is reported with its line number. */
static int load_trace(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open trace %s\n", path); return -1; }
    char line[2048];
    long ln = 1;
    int col_t = -1, col_v[MAX_CHANNELS], col_f[MAX_CHANNELS], col_link = -1, ncol = 0, nv = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) col_v[i] = col_f[i] = -1;
    if (!fgets(line, sizeof line, f)) { fprintf(stderr, "%s: empty file\n", path); fclose(f); return -1; }
    for (char *tok = strtok(line, ",\r\n"); tok; tok = strtok(NULL, ",\r\n"), ncol++) {
        if (ncol >= TRACE_MAX_COLS) { fprintf(stderr, "%s: more than %d columns\n", path, TRACE_MAX_COLS); fclose(f); return -1; }
        if (!strcmp(tok, "t")) col_t = ncol;
        else if (!strcmp(tok, "link")) col_link = ncol;
        else if (tok[0] == 'v' && tok[1] >= '0' && tok[1] < '0' + MAX_CHANNELS && !tok[2]) { col_v[tok[1] - '0'] = ncol; nv++; }
        else if (tok[0] == 'f' && tok[1] >= '0' && tok[1] < '0' + MAX_CHANNELS && !tok[2]) col_f[tok[1] - '0'] = ncol;
    }
    if (col_t < 0 || nv == 0) { fprintf(stderr, "%s: header needs a 't' column and at least one 'vN' column\n", path); fclose(f); return -1; }

    size_t cap = 1024, n = 0;
    float *v[MAX_CHANNELS]; uint8_t *fl[MAX_CHANNELS]; uint8_t *lk;
    memset(&g_trace, 0, sizeof g_trace);
    double t0 = 0, dt = 0, vals[TRACE_MAX_COLS];
    const char *err = NULL;

    lk = malloc(cap);
    bool ok = lk != NULL;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        v[i] = malloc(cap * sizeof(float)); fl[i] = malloc(cap);
        ok = ok && v[i] && fl[i];
    }
    if (!ok) err = "out of memory";

    while (!err && fgets(line, sizeof line, f)) {
        ln++;
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n' && !feof(f)) { err = "line too long"; break; }
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;     /* blank line */
        int c = parse_row(line, vals, TRACE_MAX_COLS);
        if (c == -2) { err = "inf/nan is not allowed (leave the field empty for a missing reading)"; break; }
        if (c < 0) { err = "non-numeric field"; break; }
        if (c != ncol) { err = "row has a different number of fields than the header"; break; }
        if (n == cap) {
            cap *= 2;
            void *q = realloc(lk, cap); if (!q) { err = "out of memory"; break; } lk = q;
            for (int i = 0; i < MAX_CHANNELS && !err; i++) {
                q = realloc(v[i], cap * sizeof(float)); if (!q) { err = "out of memory"; break; } v[i] = q;
                q = realloc(fl[i], cap); if (!q) { err = "out of memory"; break; } fl[i] = q;
            }
            if (err) break;
        }
        double t = vals[col_t];
        if (t != t) { err = "missing time value"; break; }
        if (t < 0 || t > TRACE_MAX_TIME_S || floor(t) != t) {
            err = "time must be a whole number of seconds from 0 to 2147483647"; break;
        }
        if (n == 0) {
            t0 = t;
            if (t0 != 0) { err = "first time value must be 0 (the simulation starts at t = 0)"; break; }
        } else if (n == 1) {
            dt = t - t0;                                   /* whole and in range: t is checked above */
            if (dt < 1) { err = "time step must be a positive whole number of seconds"; break; }
        } else if (t != t0 + (double)n * dt) { err = "time column is not evenly spaced"; break; }
        for (int i = 0; i < MAX_CHANNELS; i++) {
            double x = col_v[i] >= 0 ? vals[col_v[i]] : NAN;
            double fc = col_f[i] >= 0 ? vals[col_f[i]] : 0.0;
            if (fc != fc) fc = 0.0;
            if (!is_code(fc, 2)) { err = "fault code must be 0, 1 or 2"; break; }
            if (x == x && fabs(x) > FLT_MAX) { err = "sensor value outside the float range"; break; }
            v[i][n] = (float)x;
            fl[i][n] = (uint8_t)fc;
            if (x != x && fl[i][n] == 0) fl[i][n] = 1;     /* no value -> missing reading */
        }
        if (err) break;
        double lkv = col_link >= 0 ? vals[col_link] : 1.0;
        if (lkv != lkv) lkv = 1.0;
        if (!is_code(lkv, 1)) { err = "link must be 0 or 1"; break; }
        lk[n] = (uint8_t)lkv;
        n++;
    }
    fclose(f);
    for (int i = 0; i < MAX_CHANNELS; i++) { g_trace.v[i] = v[i]; g_trace.fault[i] = fl[i]; }
    g_trace.link = lk;
    if (!err && n < 2) err = "need at least two data rows";
    if (!err && (double)n * dt > TRACE_MAX_TIME_S) err = "trace duration exceeds 2147483647 s";
    if (err) {
        fprintf(stderr, "%s:%ld: %s\n", path, ln, err);
        free_trace();
        return -1;
    }
    g_trace.n_rows = (uint32_t)n;
    g_trace.dt_s = (uint32_t)dt;
    return 0;
}

/* ---------------- file-backed flash emulation ---------------- */
static int file_write(void *ctx, uint32_t slot, const log_record_t *r)
{
    FILE *f = ctx;
    fseek(f, (long)(slot * sizeof *r), SEEK_SET);
    return fwrite(r, sizeof *r, 1, f) == 1 ? 0 : -1;
}
static int file_read(void *ctx, uint32_t slot, log_record_t *r)
{
    FILE *f = ctx;
    fseek(f, (long)(slot * sizeof *r), SEEK_SET);
    return fread(r, sizeof *r, 1, f) == 1 ? 0 : -1;
}

/* ---------------- logging ---------------- */
static FILE *g_samples, *g_tx, *g_gw;

static void fmt_val(FILE *f, float v) { if (isnan(v)) fputs(",", f); else fprintf(f, ",%.4f", v); }

static void on_sample(void *ctx, const node_sample_log_t *s)
{
    const node_t *n = ctx;
    fprintf(g_samples, "%u,%u,%u,%u,%u", s->t, s->mode, s->interval, s->node_state, s->n_tx);
    for (int i = 0; i < n->prof->n_channels; i++) {
        fmt_val(g_samples, s->rd[i].value); fmt_val(g_samples, s->filt[i]);
        fprintf(g_samples, ",%u,%u,%u", s->ch_state[i], s->rd[i].status, s->flags[i]);
    }
    fputc('\n', g_samples);
}

static void on_attempt(void *ctx, uint32_t t, const txq_item_t *it, uint8_t attempt, float air, bool acked)
{
    (void)ctx;
    fprintf(g_tx, "%u,%u,%s,%u,%u,%.2f,%u,%u,%u,%u\n", t, it->seq, pkt_type_name(it->type), it->prio, it->len,
            air, attempt, acked ? 1 : 0, (it->buf[10] & PKT_FLAG_STORED) ? 1 : 0, it->created_t);
}

static void on_gateway(void *ctx, uint32_t t, const packet_t *p, uint8_t len)
{
    (void)ctx;
    fprintf(g_gw, "%u,%u,%u,%s,%u,%u,%u,%u,%u", t, p->timestamp, p->seq, pkt_type_name(p->type), p->priority,
            p->node_state, p->flags & PKT_FLAG_STORED ? 1 : 0, p->mask, len);
    for (int i = 0; i < MAX_CHANNELS; i++) fmt_val(g_gw, p->ch[i].value);
    for (int i = 0; i < MAX_CHANNELS; i++) fmt_val(g_gw, p->ch[i].vmax);
    fputc('\n', g_gw);
}

static FILE *open_out(const char *dir, const char *name)
{
    char path[700];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) fprintf(stderr, "cannot write %s (does the directory exist?)\n", path);
    return f;
}

/* ---------------- profile dump / filter test ---------------- */
static void jf(FILE *f, const char *k, float v, int last)
{
    if (v >= 1e8f || v <= -1e8f) fprintf(f, "\"%s\": null%s", k, last ? "" : ", ");
    else fprintf(f, "\"%s\": %g%s", k, v, last ? "" : ", ");
}

static void dump_profile(const app_profile_t *p)
{
    printf("{\"id\": %u, \"name\": \"%s\", \"description\": \"%s\",\n", p->id, p->name, p->description);
    printf(" \"sampling\": {\"fixed_s\": %u, \"stable_s\": %u, \"active_s\": %u, \"alert_s\": %u, \"recovery_s\": %u},\n",
           p->sampling.fixed_s, p->sampling.stable_s, p->sampling.active_s, p->sampling.alert_s, p->sampling.recovery_s);
    printf(" \"telemetry\": {\"summary_s\": %u, \"min_update_s\": %u, \"crit_repeat_s\": %u},\n",
           p->telemetry.summary_s, p->telemetry.min_update_s, p->telemetry.crit_repeat_s);
    printf(" \"channels\": [\n");
    for (int i = 0; i < p->n_channels; i++) {
        const sensor_channel_cfg_t *c = &p->ch[i];
        printf("  {\"index\": %d, \"sensor_id\": %u, \"name\": \"%s\", \"unit\": \"%s\", \"driver\": \"%s\", ",
               i, c->sensor_id, c->name, c->unit_str, c->driver->name);
        jf(stdout, "range_min", c->range_min, 0); jf(stdout, "range_max", c->range_max, 0);
        jf(stdout, "warn_lo", c->thr.warn_lo, 0); jf(stdout, "warn_hi", c->thr.warn_hi, 0);
        jf(stdout, "crit_lo", c->thr.crit_lo, 0); jf(stdout, "crit_hi", c->thr.crit_hi, 0);
        jf(stdout, "hyst", c->thr.hyst, 0); jf(stdout, "delta_report", c->delta_report, 1);
        printf("}%s\n", i + 1 < p->n_channels ? "," : "");
    }
    printf(" ]}\n");
}

static int filter_test(const char *in, const char *out)
{
    FILE *fi = fopen(in, "r"), *fo = fopen(out, "w");
    if (!fi || !fo) { fprintf(stderr, "filter-test: cannot open files\n"); return 1; }
    ma_filter_t ma; fir_filter_t fir; hampel_t hp;
    ma_init(&ma, 5); fir_init(&fir, FIR_BINOMIAL7, 7); hampel_init(&hp, 3.0f, 0.0f);
    char line[128];
    fprintf(fo, "x,ma5,fir7,hampel\n");
    while (fgets(line, sizeof line, fi)) {
        char *e; double x = strtod(line, &e);
        if (e == line) continue;                    /* header */
        bool o;
        float a = ma_update(&ma, (float)x), b = fir_update(&fir, (float)x), h = hampel_update(&hp, (float)x, &o);
        fprintf(fo, "%.9g,%.9g,%.9g,%.9g\n", x, a, b, h);
    }
    fclose(fi); fclose(fo);
    return 0;
}

static const char *strat_name(int s) { return s == STRAT_FIXED ? "fixed" : s == STRAT_ADAPTIVE_TX ? "adaptive_tx" : "adaptive"; }

int main(int argc, char **argv)
{
    opts_t o;
    memset(&o, 0, sizeof o);
    strcpy(o.profile, "water"); strcpy(o.strategy, "adaptive"); strcpy(o.out, ".");
    o.seed = 1; o.node_id = 1; o.storage_cap = 8192; o.sf = 9; o.loss = 0.02; o.ack_loss = -1; o.corrupt = 0.005;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list-profiles")) {
            for (int k = 0; k < profile_count(); k++) printf("%s: %s\n", profile_at(k)->name, profile_at(k)->description);
            return 0;
        }
        if (!strcmp(argv[i], "--dump-profile") && i + 1 < argc) {
            const app_profile_t *p = profile_find(argv[i + 1]);
            if (!p) { fprintf(stderr, "unknown profile\n"); return 2; }
            dump_profile(p); return 0;
        }
        if (!strcmp(argv[i], "--filter-test") && i + 2 < argc) return filter_test(argv[i + 1], argv[i + 2]);
        if (!strcmp(argv[i], "--config") && i + 1 < argc) { if (load_ini(&o, argv[++i])) return 2; continue; }
        if (!strncmp(argv[i], "--", 2) && i + 1 < argc) { set_opt(&o, argv[i] + 2, argv[i + 1]); i++; continue; }
        fprintf(stderr, "bad argument %s\n", argv[i]); return 2;
    }
    if (o.ack_loss < 0) o.ack_loss = o.loss;
    if (!(o.loss >= 0 && o.loss <= 1) || !(o.ack_loss >= 0 && o.ack_loss <= 1) || !(o.corrupt >= 0 && o.corrupt <= 1)) {
        fprintf(stderr, "loss, ack-loss and corrupt must be probabilities in [0, 1]\n"); return 2;
    }
    if (o.sf < 7 || o.sf > 12) { fprintf(stderr, "sf must be 7..12\n"); return 2; }
    if (o.node_id > 0xFFFFu) { fprintf(stderr, "node-id must fit in 16 bits\n"); return 2; }
    if (o.storage_cap < 1 || o.storage_cap > 1000000u) { fprintf(stderr, "storage-cap must be 1..1000000 records\n"); return 2; }

    const app_profile_t *prof = profile_find(o.profile);
    if (!prof) { fprintf(stderr, "unknown profile '%s' (water|air|soil)\n", o.profile); return 2; }
    int strategy = !strcmp(o.strategy, "fixed") ? STRAT_FIXED : !strcmp(o.strategy, "adaptive_tx") ? STRAT_ADAPTIVE_TX :
                   !strcmp(o.strategy, "adaptive") ? STRAT_ADAPTIVE : -1;
    if (strategy < 0) { fprintf(stderr, "unknown strategy (fixed|adaptive_tx|adaptive)\n"); return 2; }
    if (!o.trace[0] || load_trace(o.trace)) { fprintf(stderr, "a valid --trace is required\n"); return 2; }

    env_set_trace(&g_trace);
    vdev_load_board(prof->id);
    radio_sim_cfg_t rc = {(float)o.loss, (float)o.ack_loss, (float)o.corrupt, o.seed};
    radio_sim_init(&rc);
    gateway_init(on_gateway, NULL);

    g_samples = open_out(o.out, "samples.csv"); g_tx = open_out(o.out, "tx.csv"); g_gw = open_out(o.out, "gateway.csv");
    if (!g_samples || !g_tx || !g_gw) return 3;
    fprintf(g_samples, "t,mode,interval,node_state,n_tx");
    for (int i = 0; i < prof->n_channels; i++) fprintf(g_samples, ",val%d,filt%d,st%d,rs%d,fl%d", i, i, i, i, i);
    fputc('\n', g_samples);
    fprintf(g_tx, "t,seq,type,prio,len,airtime_ms,attempt,acked,stored,created_t\n");
    fprintf(g_gw, "t_rx,ts,seq,type,prio,node_state,stored,mask,len,v0,v1,v2,v3,mx0,mx1,mx2,mx3\n");

    char flash_path[700];
    if (o.flash[0]) snprintf(flash_path, sizeof flash_path, "%s", o.flash);
    else snprintf(flash_path, sizeof flash_path, "%s/flash.bin", o.out);
    FILE *flash = fopen(flash_path, "w+b");
    if (!flash) { fprintf(stderr, "cannot create flash file\n"); return 3; }
    storage_backend_t be = {file_write, file_read, flash};
    storage_t st;
    storage_init(&st, &be, o.storage_cap);

    node_config_t cfg;
    node_default_config(&cfg);
    cfg.profile_id = prof->id; cfg.strategy = (uint8_t)strategy; cfg.node_id = (uint16_t)o.node_id;
    cfg.seed = o.seed; cfg.override_stable_s = o.stable_s; cfg.lora.sf = (uint8_t)o.sf;

    static node_t node;
    if (node_init(&node, &cfg, &st)) { fprintf(stderr, "node_init failed\n"); return 3; }
    node.cb = on_sample; node.cb_ctx = &node;
    node.link.on_attempt = on_attempt;

    uint32_t dur = env_duration_s(), t = 0;
    clock_t c0 = clock();
    while (t < dur) {
        hal_sim_set_time(t);
        t = node_step(&node, t);
    }
    /* final flush attempt at end of run (does not count as extra samples) */
    hal_sim_set_time(dur);
    link_service(&node.link, dur);
    node_finish(&node, dur);
    double host_ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;

    /* local storage retrieval: dump all retained raw records */
    FILE *sd = open_out(o.out, "storage_dump.csv");
    uint32_t retrieved = 0;
    if (sd) {
        fprintf(sd, "seq,ts,node_state,mode,valid_mask,val0,val1,val2,val3\n");
        log_record_t r;
        for (uint32_t q = storage_oldest_seq(&st); q && q < st.next_seq; q++) {
            if (storage_get_by_seq(&st, q, &r)) continue;
            retrieved++;
            fprintf(sd, "%u,%u,%u,%u,%u", r.seq, r.ts, r.node_state, r.mode, r.valid_mask);
            for (int i = 0; i < MAX_CHANNELS; i++) fmt_val(sd, i < r.n ? r.value[i] : NAN);
            fputc('\n', sd);
        }
        fclose(sd);
    }
    fclose(flash); fclose(g_samples); fclose(g_tx); fclose(g_gw);

    const link_stats_t *ls = &node.link.st;
    const gw_stats_t *gs = gateway_stats();
    const radio_sim_stats_t *rs = radio_sim_stats();
    energy_acc_t *e = &node.e;
    double q = energy_total_mas(e), v = cfg.energy.v_supply;
    double avg_ma = q / dur;
    FILE *js = open_out(o.out, "summary.json");
    if (!js) return 3;
    fprintf(js, "{\n \"profile\": \"%s\", \"strategy\": \"%s\", \"seed\": %u, \"duration_s\": %u,\n", prof->name, strat_name(strategy), o.seed, dur);
    fprintf(js, " \"loss\": %g, \"ack_loss\": %g, \"corrupt\": %g, \"sf\": %u, \"stable_s\": %u,\n", o.loss, o.ack_loss, o.corrupt, o.sf, node.prof->sampling.stable_s);
    fprintf(js, " \"samples\": %u, \"mode_samples\": [%u, %u, %u, %u, %u],\n", node.n_samples,
            node.mode_samples[0], node.mode_samples[1], node.mode_samples[2], node.mode_samples[3], node.mode_samples[4]);
    fprintf(js, " \"packets_generated\": %u, \"packets_delivered\": %u, \"packets_dropped_queue\": %u, \"packets_in_queue_at_end\": %u,\n",
            node.n_packets, ls->delivered, ls->dropped_queue, node.link.count);
    fprintf(js, " \"tx_attempts\": %u, \"retries\": %u, \"failed_sends\": %u, \"outages_detected\": %u, \"recovered_after_outage\": %u, \"max_queue\": %u,\n",
            ls->attempts, ls->retries, ls->failed_sends, ls->outages, ls->recovered_after_outage, ls->max_queue);
    fprintf(js, " \"bytes_air_app\": %u, \"bytes_air_phy\": %u, \"bytes_delivered\": %u, \"airtime_ms\": %.2f, \"rx_ms\": %.2f, \"duty_cycle_pct\": %.5f,\n",
            ls->bytes_air, ls->bytes_air + ls->attempts * cfg.lora.mac_overhead, ls->bytes_delivered, ls->airtime_ms, ls->rx_ms,
            ls->airtime_ms / 10.0 / dur);
    fprintf(js, " \"gateway_rx\": %u, \"gateway_crc_fail\": %u, \"gateway_duplicates\": %u,\n", gs->rx_ok, gs->crc_fail, gs->duplicates);
    fprintf(js, " \"radio_lost_outage\": %u, \"radio_lost_channel\": %u, \"radio_corrupted\": %u, \"radio_ack_lost\": %u,\n",
            rs->lost_outage, rs->lost_channel, rs->corrupted, rs->ack_lost);
    fprintf(js, " \"storage_records\": %u, \"storage_overwritten\": %u, \"storage_retrieved\": %u, \"telemetry_suppressed\": %u,\n",
            st.next_seq - 1, st.overwritten, retrieved, node.tel.suppressed);
    fprintf(js, " \"sensor_reads\": %u, \"sensor_errors\": %u, \"sensor_activations\": %u, \"dsp_mac_ops\": %u,\n",
            node.sm.reads, node.sm.errors, node.sm.activations, node_dsp_mac_ops(&node));
    fprintf(js, " \"energy_mas\": {\"sensor\": %.3f, \"processing\": %.3f, \"tx\": %.3f, \"rx\": %.3f, \"idle\": %.3f, \"sleep\": %.3f, \"switching\": %.3f, \"total\": %.3f},\n",
            e->q_sensor, e->q_proc, e->q_tx, e->q_rx, e->q_idle, e->q_sleep, e->q_switch, q);
    fprintf(js, " \"energy_mj_total\": %.3f, \"avg_current_ma\": %.6f, \"projected_battery_days\": %.2f, \"t_active_s\": %.2f,\n",
            q * v, avg_ma, cfg.energy.battery_mah / (avg_ma > 0 ? avg_ma : 1e-9) / 24.0, e->t_active_s);
    fprintf(js, " \"host_exec_ms\": %.3f, \"host_us_per_sample\": %.3f\n}\n", host_ms, node.n_samples ? host_ms * 1000.0 / node.n_samples : 0.0);
    fclose(js);
    printf("%s/%s: samples=%u packets=%u delivered=%u airtime=%.1fms energy=%.1fmJ\n", prof->name, strat_name(strategy),
           node.n_samples, node.n_packets, ls->delivered, ls->airtime_ms, q * v);
    return 0;
}
