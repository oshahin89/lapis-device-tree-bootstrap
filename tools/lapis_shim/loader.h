#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload appended to the shim Image; returns only on error. */
const char *loader_boot(void);
