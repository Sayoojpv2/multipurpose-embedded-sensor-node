/* node.c - the node application: one call of node_step() = one wake-up.
 *
 *  wake -> power sensors -> acquire/convert/validate -> Hampel/MA/FIR
 *       -> event state machines -> store record -> telemetry policy
 *       -> link layer (ACK/retry/queue) -> choose next interval -> sleep
 */
#include "node.h"
#include "hal.h"
#include <math.h>
#include <string.h>

void node_default_config(node_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->profile_id = PROFILE_WATER;
    c->strategy = STRAT_ADAPTIVE;
    c->node_id = 1;
    c->seed = 1;
    link_default_cfg(&c->link);
    lora_default_cfg(&c->lora);
    energy_default_cfg(&c->energy);
}

int node_init(node_t *n, const node_config_t *c, storage_t *st)
{
    const app_profile_t *p = profile_get(c->profile_id);
    if (p == NULL) return -1;
    memset(n, 0, sizeof(*n));
    n->cfg = *c;
    n->prof_rt = *p;
    if (c->override_stable_s) n->prof_rt.sampling.stable_s = c->override_stable_s;
    n->prof = &n->prof_rt;
    n->storage = st;
    sensor_manager_init(&n->sm, n->prof);
    for (int i = 0; i < p->n_channels; i++) { chan_dsp_init(&n->dsp[i], &p->ch[i]); event_init(&n->ev[i]); }
    sampler_init(&n->sampler, c->strategy);
    telemetry_init(&n->tel);
    link_init(&n->link, &c->link, &c->lora, c->seed * 2654435761u + 7u);
    n->first = true;
    return 0;
}

static float sensor_on_time(const sensor_channel_cfg_t *c) { return c->pwr.warmup_s + c->pwr.measure_s; }

uint32_t node_step(node_t *n, uint32_t now)
{
    const app_profile_t *p = n->prof;
    const energy_cfg_t *ec = &n->cfg.energy;
    int nc = p->n_channels;
    float dt = n->first ? 0.0f : (float)(now - n->last_sample_t);
    node_sample_log_t lg;
    memset(&lg, 0, sizeof lg);
    lg.t = now;

    /* ---- acquisition ---- */
    uint8_t switched = sensor_manager_sample(&n->sm, now, lg.rd);

    /* ---- energy: sensors, idle wait, processing ---- */
    float idle = 0.0f;
    for (int i = 0; i < nc; i++) {
        const sensor_channel_cfg_t *c = &p->ch[i];
        float on = sensor_on_time(c);
        if (switched & (1u << i)) {
            float off = dt > on ? dt - on : 0.0f;
            n->e.q_sensor += c->pwr.active_ma * on + c->pwr.off_ua / 1000.0f * off;
            n->e.q_switch += ec->sensor_switch_uc / 1000.0f;
            if (on > idle) idle = on;
        } else {
            n->e.q_sensor += c->pwr.active_ma * dt;           /* stayed powered */
            if (c->pwr.measure_s > idle) idle = c->pwr.measure_s;
        }
    }
    n->e.q_idle += ec->mcu_idle_ma * idle;
    n->e.q_proc += ec->mcu_active_ma * ec->proc_ms_per_sample / 1000.0f;
    n->e.t_active_s += idle + ec->proc_ms_per_sample / 1000.0f;

    /* ---- DSP + event detection ---- */
    sample_cues_t cues;
    memset(&cues, 0, sizeof cues);
    for (int i = 0; i < nc; i++) {
        const sensor_channel_cfg_t *c = &p->ch[i];
        bool valid = reading_valid(&lg.rd[i]);
        float f = NAN;
        if (valid) {
            if (n->fault_reset[i]) { chan_dsp_reset(&n->dsp[i]); lg.flags[i] |= DSP_FLAG_RESET; n->fault_reset[i] = 0; }
            f = chan_dsp_process(&n->dsp[i], lg.rd[i].value, &lg.flags[i]);
            if (n->prev_valid[i] && dt > 0.0f && fabsf(f - n->prev_filt[i]) / dt * 60.0f >= c->roc_per_min) cues.roc_high = true;
            int raw_lvl = event_level(&c->thr, lg.rd[i].value, 0.0f);   /* unfiltered cue */
            if (raw_lvl == 2) cues.pending_crit = true;
            if (raw_lvl == 1) cues.pending_warn = true;
            n->prev_filt[i] = f; n->prev_valid[i] = true;
        } else {
            n->prev_valid[i] = false;
        }
        lg.ch_state[i] = event_update(&n->ev[i], &c->thr, &p->event, valid, f);
        if (lg.ch_state[i] == EV_FAULT) n->fault_reset[i] = 1;
        if (event_pending_level(&n->ev[i], 2)) cues.pending_crit = true;
        if (event_pending_level(&n->ev[i], 1)) cues.pending_warn = true;
        if (lg.flags[i] & DSP_FLAG_SUSPECT) cues.suspect = true;
        lg.filt[i] = f;
    }
    lg.node_state = event_node_state(n->ev, nc);
    cues.node_state = lg.node_state;

    /* ---- local storage (every sample) ---- */
    if (n->storage) {
        log_record_t r;
        memset(&r, 0, sizeof r);
        r.ts = now; r.node_state = lg.node_state; r.mode = n->sampler.mode; r.n = (uint8_t)nc;
        for (int i = 0; i < nc; i++) {
            r.value[i] = lg.rd[i].value; r.filt[i] = lg.filt[i];
            if (reading_valid(&lg.rd[i])) r.valid_mask |= (uint8_t)(1u << i);
        }
        storage_append(n->storage, &r);
    }

    /* ---- telemetry + link ---- */
    float air0 = n->link.st.airtime_ms, rx0 = n->link.st.rx_ms;
    packet_t pk[2];
    int k = telemetry_on_sample(&n->tel, p, n->cfg.strategy, now, lg.filt, lg.rd, lg.ch_state, lg.node_state, pk, 2);
    for (int j = 0; j < k; j++) {
        uint8_t buf[PKT_MAX_LEN];
        pk[j].node_id = n->cfg.node_id; pk[j].profile_id = p->id; pk[j].seq = n->seq++;
        int len = packet_encode(&pk[j], buf, sizeof buf);
        if (len > 0) {
            link_submit(&n->link, buf, (uint8_t)len, pk[j].priority, pk[j].seq, pk[j].type, now);
            n->n_packets++;
        }
        n->e.q_proc += ec->mcu_active_ma * ec->proc_ms_per_packet / 1000.0f;
        n->e.t_active_s += ec->proc_ms_per_packet / 1000.0f;
    }
    link_service(&n->link, now);
    float dair = n->link.st.airtime_ms - air0, drx = n->link.st.rx_ms - rx0;
    n->e.q_tx += ec->radio_tx_ma * dair / 1000.0f;
    n->e.q_rx += ec->radio_rx_ma * drx / 1000.0f;
    n->e.t_active_s += (dair + drx) / 1000.0f;
    lg.n_tx = (uint8_t)k;

    /* ---- next wake-up + sensor power management ---- */
    uint32_t iv = sampler_next(&n->sampler, &p->sampling, n->cfg.strategy, &cues);
    uint8_t keep = 0;
    for (int i = 0; i < nc; i++)
        if ((float)iv <= sensor_on_time(&p->ch[i]) + 1.0f) keep |= (uint8_t)(1u << i);  /* cheaper to stay on */
    sensor_manager_release(&n->sm, keep);

    lg.mode = n->sampler.mode; lg.interval = iv;
    if (lg.mode < 5) n->mode_samples[lg.mode]++;
    n->n_samples++;
    if (n->cb) n->cb(n->cb_ctx, &lg);
    n->last_sample_t = now; n->first = false;
    n->next_t = now + iv;
    hal_timer_set_wakeup(n->next_t);
    return n->next_t;
}

void node_finish(node_t *n, uint32_t end_t)
{
    const app_profile_t *p = n->prof;
    float tail = end_t > n->last_sample_t ? (float)(end_t - n->last_sample_t) : 0.0f;
    for (int i = 0; i < p->n_channels; i++)
        n->e.q_sensor += (n->sm.powered[i] ? p->ch[i].pwr.active_ma : p->ch[i].pwr.off_ua / 1000.0f) * tail;
    energy_finalize(&n->e, &n->cfg.energy, end_t);
}

uint32_t node_dsp_mac_ops(const node_t *n)
{
    uint32_t s = 0;
    for (int i = 0; i < n->prof->n_channels; i++) s += n->dsp[i].mac_ops;
    return s;
}
