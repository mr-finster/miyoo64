/* Shared state between the pmcap input and video plugins (one instance per process). */
volatile unsigned pmcap_poll;      /* GetKeys(0) calls so far */
volatile int pmcap_cap_left;       /* gfx tasks still to capture */
volatile int pmcap_cap_index;      /* running index within the current capture */
char pmcap_cap_name[64];
