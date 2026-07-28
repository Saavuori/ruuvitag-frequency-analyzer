/*
 * Environmental sensing. Temperature and humidity are not filler: safe-sleep
 * guidance cares about nursery temperature, and these come free with the
 * hardware (docs/05-advertising-format.md).
 */

#ifndef RFA_ENV_H
#define RFA_ENV_H

#include <stdbool.h>
#include <stdint.h>

struct rfa_env {
	int32_t temperature_mc;   /* millidegrees C */
	int32_t humidity_mpct;    /* milli-percent RH */
	int32_t pressure_pa;
	bool    valid;            /* false -> emit DF5 invalid sentinels */
};

int rfa_env_init(void);
void rfa_env_read(struct rfa_env *out);

#endif /* RFA_ENV_H */
