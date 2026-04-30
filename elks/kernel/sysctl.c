#include <linuxmt/mm.h>
#include <linuxmt/config.h>
#include <linuxmt/errno.h>
#include <linuxmt/string.h>
#include <linuxmt/sysctl.h>

#include <linuxmt/trace.h>
#include <linuxmt/kernel.h>

struct sysctl {
    const char *name;
    int *value;
};

static int malloc_debug;
static int net_debug;

#ifdef CONFIG_BLK_DEV_MFMHD
extern int mfmhd_debug_stage;
extern int mfmhd_debug_drive;
extern int mfmhd_debug_port;
extern int mfmhd_debug_status;
extern int mfmhd_debug_cmd;
extern int mfmhd_debug_csb;
extern int mfmhd_debug_sense0;
extern int mfmhd_debug_sense1;
extern int mfmhd_debug_sense2;
extern int mfmhd_debug_sense3;
extern int mfmhd_debug_hdcnt;
extern int mfmhd_debug_error;
extern int mfmhd_debug_count;
extern int mfmhd_debug_fail_stage;
extern int mfmhd_debug_fail_drive;
extern int mfmhd_debug_fail_port;
extern int mfmhd_debug_fail_status;
extern int mfmhd_debug_fail_cmd;
extern int mfmhd_debug_fail_csb;
extern int mfmhd_debug_fail_error;
#endif

struct sysctl sysctl[] = {
    { "kern.debug",         &debug_level        },  /* debug level (^P toggled) */
    { "kern.strace",        &tracing            },  /* strace=1, kstack=2 */
    { "kern.console",       (int *)&dev_console },  /* console */
    { "malloc.debug",       &malloc_debug       },
    { "net.debug",          &net_debug          },
#ifdef CONFIG_BLK_DEV_MFMHD
    { "mfmhd.stage",        &mfmhd_debug_stage  },
    { "mfmhd.drive",        &mfmhd_debug_drive  },
    { "mfmhd.port",         &mfmhd_debug_port   },
    { "mfmhd.status",       &mfmhd_debug_status },
    { "mfmhd.cmd",          &mfmhd_debug_cmd    },
    { "mfmhd.csb",          &mfmhd_debug_csb    },
    { "mfmhd.sense0",       &mfmhd_debug_sense0 },
    { "mfmhd.sense1",       &mfmhd_debug_sense1 },
    { "mfmhd.sense2",       &mfmhd_debug_sense2 },
    { "mfmhd.sense3",       &mfmhd_debug_sense3 },
    { "mfmhd.hdcnt",        &mfmhd_debug_hdcnt  },
    { "mfmhd.error",        &mfmhd_debug_error  },
    { "mfmhd.count",        &mfmhd_debug_count  },
    { "mfmhd.fail_stage",   &mfmhd_debug_fail_stage  },
    { "mfmhd.fail_drive",   &mfmhd_debug_fail_drive  },
    { "mfmhd.fail_port",    &mfmhd_debug_fail_port   },
    { "mfmhd.fail_status",  &mfmhd_debug_fail_status },
    { "mfmhd.fail_cmd",     &mfmhd_debug_fail_cmd    },
    { "mfmhd.fail_csb",     &mfmhd_debug_fail_csb    },
    { "mfmhd.fail_error",   &mfmhd_debug_fail_error  },
#endif
};

static char ctlname[CTL_MAXNAMESZ];

#define ARRAYLEN(a)     (sizeof(a)/sizeof(a[0]))

int sys_sysctl(int op, char *name, int *value)
{
    int error, n;
    struct sysctl *sc;

    if (op >= CTL_LIST) {
        if (!name)
            return -EFAULT;
        if (op >= ARRAYLEN(sysctl))
            return -ENOTDIR;
        sc = &sysctl[op];
        return verified_memcpy_tofs(name, (void *)sc->name, strlen(sc->name)+1);
    }

    n = strlen_fromfs(name, CTL_MAXNAMESZ) + 1;
    if (n >= CTL_MAXNAMESZ) return -EFAULT;
    error = verified_memcpy_fromfs(ctlname, name, n);
    if (error) return error;
    error = verfy_area(value, sizeof(int));
    if (error) return error;

    for (sc = sysctl;; sc++) {
        if (sc >= &sysctl[ARRAYLEN(sysctl)]) return -ENOTDIR;
        if (!strcmp(sc->name, ctlname))
            break;
    }

    if (op == CTL_GET)
            put_user(*sc->value, value);
    else if (op == CTL_SET)
            *sc->value = get_user(value);
    else
        return -EINVAL;
    return 0;
}
