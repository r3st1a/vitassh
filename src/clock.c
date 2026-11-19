#include "clock.h"

#include <psp2/kernel/processmgr.h>

uint64_t now_ms(void)
{
	return (uint64_t)sceKernelGetSystemTimeWide() / 1000u;
}
