#include <psp2/types.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>

int module_stop(SceSize argc, const void *args) {
	(void)argc;
	(void)args;
	return SCE_KERNEL_STOP_SUCCESS;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, void *args) {
	(void)argc;
	(void)args;
	/* Intentionally no hooks or background work. Safe compatibility SUPRX. */
	(void)sceKernelGetProcessTimeWide();
	return SCE_KERNEL_START_SUCCESS;
}
