#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/power.h>
#include <taihen.h>
#include <string.h>

#define SECOND_US 1000000ULL
#define DATA_DIR "ux0:data/BatteryConsumption"
#define SESSIONS_PATH "ux0:data/BatteryConsumption_sessions.csv"
#define STATE_PATH "ux0:data/BatteryConsumption_kernel_state.txt"
#define PLUGIN_TITLEID "BATC00001"
#define MIN_WRITE_SESSION_US (15ULL * SECOND_US)

typedef struct SessionState {
	int active;
	SceUID pid;
	char app[32];
	SceUInt64 start_tick;
	int start_pct;
	int start_mah;
	int start_mv;
} SessionState;

static tai_hook_ref_t g_proc_event_ref;
static SceUID g_hook_uid = -1;
static SessionState g_state;

static int get_titleid_from_pid(SceUID pid, char *out, unsigned int out_size) {
	SceUID modid;
	SceKernelModuleInfo info;
	char *p;
	if (!out || out_size < 10) {
		return -1;
	}
	out[0] = '\0';

	modid = ksceKernelGetProcessMainModule(pid);
	if (modid < 0) {
		return -2;
	}

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (ksceKernelGetModuleInfo(pid, modid, &info) < 0) {
		return -3;
	}

	p = strstr(info.path, "/app/");
	if (p && strlen(p) >= 14) {
		p += 5;
		strncpy(out, p, out_size - 1);
		out[out_size - 1] = '\0';
		out[9] = '\0';
		return 0;
	}

	p = strrchr(info.path, '/');
	if (p && p > info.path) {
		char *q = p;
		while (q > info.path && q[-1] != '/') {
			q--;
		}
		strncpy(out, q, out_size - 1);
		out[out_size - 1] = '\0';
		if (strlen(out) > 9) {
			out[9] = '\0';
		}
		return 0;
	}

	return -4;
}

static int app_should_track(const char *titleid) {
	if (!titleid || !titleid[0]) {
		return 0;
	}
	if (strncmp(titleid, "UNKNOWN", 7) == 0) {
		return 0;
	}
	if (strncmp(titleid, "NPXS", 4) == 0 || strncmp(titleid, "main", 4) == 0) {
		return 0;
	}
	if (strncmp(titleid, PLUGIN_TITLEID, 9) == 0) {
		return 0;
	}
	return 1;
}

static void ensure_files_ready(void) {
	SceUID fd;
	ksceIoMkdir("ux0:data", 6);
	ksceIoMkdir(DATA_DIR, 6);

	fd = ksceIoOpen(SESSIONS_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoClose(fd);
		return;
	}

	fd = ksceIoOpen(SESSIONS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd >= 0) {
		static const char *header =
			"#start_tick,end_tick,app,start_pct,end_pct,delta_pct,start_mah,end_mah,delta_mah,start_mv,end_mv,duration_min\n";
		ksceIoWrite(fd, header, (SceSize)strlen(header));
		ksceIoClose(fd);
	}
}

static void write_state_line(const char *line) {
	SceUID fd = ksceIoOpen(STATE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		return;
	}
	ksceIoWrite(fd, line, (SceSize)strlen(line));
	ksceIoClose(fd);
}

static void session_start(SceUID pid, const char *titleid) {
	if (!app_should_track(titleid)) {
		return;
	}

	g_state.active = 1;
	g_state.pid = pid;
	strncpy(g_state.app, titleid, sizeof(g_state.app) - 1);
	g_state.app[sizeof(g_state.app) - 1] = '\0';
	g_state.start_tick = ksceKernelGetSystemTimeWide();
	g_state.start_pct = kscePowerGetBatteryLifePercent();
	g_state.start_mah = kscePowerGetBatteryRemainCapacity();
	g_state.start_mv = kscePowerGetBatteryVolt();
}

static void session_close(const char *reason) {
	SceUInt64 end_tick;
	SceUInt64 delta_us;
	SceUInt64 min_x1000;
	unsigned int min_int;
	unsigned int min_frac;
	int end_pct;
	int end_mah;
	int end_mv;
	int d_pct;
	int d_mah;
	char line[320];
	SceUID fd;
	(void)reason;

	if (!g_state.active) {
		return;
	}

	end_tick = ksceKernelGetSystemTimeWide();
	end_pct = kscePowerGetBatteryLifePercent();
	end_mah = kscePowerGetBatteryRemainCapacity();
	end_mv = kscePowerGetBatteryVolt();

	delta_us = (end_tick > g_state.start_tick) ? (end_tick - g_state.start_tick) : 0;
	min_x1000 = delta_us / 60000ULL;
	min_int = (unsigned int)(min_x1000 / 1000ULL);
	min_frac = (unsigned int)(min_x1000 % 1000ULL);

	d_pct = g_state.start_pct - end_pct;
	d_mah = g_state.start_mah - end_mah;

	if (delta_us >= MIN_WRITE_SESSION_US || d_mah > 0 || d_pct > 0) {
		snprintf(line, sizeof(line),
			"%llu,%llu,%s,%d,%d,%d,%d,%d,%d,%d,%d,%u.%03u\n",
			(unsigned long long)g_state.start_tick,
			(unsigned long long)end_tick,
			g_state.app,
			g_state.start_pct,
			end_pct,
			d_pct,
			g_state.start_mah,
			end_mah,
			d_mah,
			g_state.start_mv,
			end_mv,
			min_int,
			min_frac);

		fd = ksceIoOpen(SESSIONS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
		if (fd >= 0) {
			ksceIoWrite(fd, line, (SceSize)strlen(line));
			ksceIoClose(fd);
		}
	}

	memset(&g_state, 0, sizeof(g_state));
}

static int proc_event_handler(int pid, int ev, int a3, int a4, int *a5, int a6) {
	char titleid[32];
	memset(titleid, 0, sizeof(titleid));
	if (get_titleid_from_pid(pid, titleid, sizeof(titleid)) < 0) {
		strncpy(titleid, "UNKNOWN", sizeof(titleid) - 1);
		titleid[sizeof(titleid) - 1] = '\0';
	}

	if (ev == 1 || ev == 5) {
		if (g_state.active && strncmp(g_state.app, titleid, sizeof(g_state.app) - 1) != 0) {
			session_close("switch");
		}
		if (!g_state.active) {
			session_start(pid, titleid);
		}
	} else if (ev == 3 || ev == 4) {
		if (g_state.active && strncmp(g_state.app, titleid, sizeof(g_state.app) - 1) == 0) {
			session_close(ev == 3 ? "exit" : "suspend");
		}
	}

	return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	memset(&g_state, 0, sizeof(g_state));
	ensure_files_ready();

	g_hook_uid = taiHookFunctionImportForKernel(
		KERNEL_PID,
		&g_proc_event_ref,
		"SceProcessmgr",
		TAI_ANY_LIBRARY,
		0x414CC813,
		proc_event_handler);

	if (g_hook_uid < 0) {
		return SCE_KERNEL_START_FAILED;
	}
	write_state_line("active=0 started=1\n");
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	session_close("module_stop");
	if (g_hook_uid >= 0) {
		taiHookReleaseForKernel(g_hook_uid, g_proc_event_ref);
		g_hook_uid = -1;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
