/*
 * Upstream uIP resolver header.
 *
 * Vendored from adamdunkels/uip with ELKS integration changes limited to
 * coexistence with the existing unified UDP app dispatcher.
 */
#ifndef __RESOLV_H__
#define __RESOLV_H__

#include "uipopt.h"

void resolv_found(char *name, u16_t *ipaddr);
void resolv_appcall(void);
void resolv_conf(u16_t (*dnsserver)[2]);
u16_t *resolv_getserver(void);
void resolv_init(void);
u16_t *resolv_lookup(char *name);
void resolv_query(char *name);

#endif
