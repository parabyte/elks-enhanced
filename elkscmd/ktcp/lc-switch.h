/*
 * Local continuation implementation based on switch statements.
 *
 * Vendored from the upstream Adam Dunkels uIP tree with only path-level
 * integration changes.
 */
#ifndef __LC_SWITCH_H__
#define __LC_SWTICH_H__

typedef unsigned short lc_t;

#define LC_INIT(s) s = 0
#define LC_RESUME(s) switch(s) { case 0:
#define LC_SET(s) s = __LINE__; case __LINE__:
#define LC_END(s) }

#endif
