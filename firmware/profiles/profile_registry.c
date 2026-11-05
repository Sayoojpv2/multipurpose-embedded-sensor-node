/* profile_registry.c - profile lookup by id or name, shared filter taps. */
#include "profile.h"
#include <string.h>

/* 7-tap binomial low-pass FIR: coefficients of (1 + z^-1)^6 / 64, unity DC gain. */
const float FIR_BINOMIAL7[7] = {1/64.0f, 6/64.0f, 15/64.0f, 20/64.0f, 15/64.0f, 6/64.0f, 1/64.0f};

static const app_profile_t *const PROFILES[] = { &PROFILE_WATER_CFG, &PROFILE_AIR_CFG, &PROFILE_SOIL_CFG };

int profile_count(void) { return (int)(sizeof PROFILES / sizeof PROFILES[0]); }

const app_profile_t *profile_at(int i) { return (i >= 0 && i < profile_count()) ? PROFILES[i] : NULL; }

const app_profile_t *profile_get(uint8_t id)
{
    for (int i = 0; i < profile_count(); i++) if (PROFILES[i]->id == id) return PROFILES[i];
    return NULL;
}

const app_profile_t *profile_find(const char *name)
{
    for (int i = 0; i < profile_count(); i++) if (strcmp(PROFILES[i]->name, name) == 0) return PROFILES[i];
    return NULL;
}
