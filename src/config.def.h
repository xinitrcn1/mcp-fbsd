/* Build-time defaults. `make` copies this to config.h once; edit config.h. */

#define CFG_SHELL          "/bin/sh"
#define CFG_MAX_ROOTS      8               /* -r DIR flags accepted */
#define CFG_MAX_REQ        (16u << 20)     /* longest request line, bytes */

/* read */
#define CFG_READ_LINES     2000            /* default line limit */
#define CFG_READ_BYTES     (256u << 10)    /* hard cap per call */
#define CFG_LINE_MAX       2000            /* chars kept per line (read, search) */

/* processes */
#define CFG_PROCS          16              /* concurrent sessions */
#define CFG_RING           (256u << 10)    /* output kept per session; power of 2 */
#define CFG_PROC_OUT       (32u << 10)     /* bytes returned per call */
#define CFG_WAITERS        16              /* calls blocked on processes at once */
#define CFG_EXEC_MS        10000           /* default exec wait */
#define CFG_MAX_MS         600000          /* longest wait any call may ask */

/* search / ls */
#define CFG_SEARCH_MAX     200             /* default result cap */
#define CFG_SEARCH_MS      10000           /* time budget */
#define CFG_SEARCH_FSIZE   (8u << 20)      /* skip larger files in content search */
#define CFG_LS_MAX         1000            /* entries per call */

/* images */
#define CFG_IMG_DIM        1568            /* default max side when viewing */
#define CFG_IMG_PIXELS     (32u << 20)     /* refuse to decode larger */
#define CFG_IMG_PASS       (4u << 20)      /* pass-through cap for gif/webp */
