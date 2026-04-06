#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/power.h>
#include <taihen.h>
#include <stdarg.h>
#include <string.h>

#define SECOND_US 1000000ULL
#define DATA_DIR "ux0:data/BatteryConsumption"
#define SESSIONS_PATH "ux0:data/BatteryConsumption_sessions.csv"
#define SCREEN_PATH "ux0:data/BatteryConsumption_screen.csv"
#define LOG_DIR "ux0:logs/BatteryConsumption"
#define PLUGIN_LOG_PATH "ux0:logs/BatteryConsumption/BatteryConsumptionPlugin.log"
#define PLUGIN_TITLEID "BATC00001"
#define MIN_WRITE_SESSION_US (15ULL * SECOND_US)
#define MIN_WRITE_SCREEN_US (5ULL * SECOND_US)
#define SCREEN_ROLL_MIN_US (10ULL * SECOND_US)
#define EVENT_DEBOUNCE_US (800ULL * 1000ULL)

typedef struct SessionState {
	int active;
	SceUID pid;
	char app[32];
	SceUInt64 start_tick;
	int start_pct;
	int start_mah;
	int start_mv;
} SessionState;

typedef struct ScreenState {
	int active;
	SceUInt64 start_tick;
	int start_pct;
	int start_mah;
	int start_mv;
} ScreenState;

static tai_hook_ref_t g_proc_event_ref;
static SceUID g_hook_uid = -1;
static SessionState g_state;
static ScreenState g_screen;
static SceUID g_last_event_pid = -1;
static int g_last_event_code = -1;
static SceUInt64 g_last_event_tick = 0;
static void screen_close(const char *reason);

static void plugin_logf(const char *fmt, ...) {
	char body[224];
	char line[320];
	va_list ap;
	SceUID fd;
	SceUInt64 tick = ksceKernelGetSystemTimeWide();
	unsigned int sec = (unsigned int)(tick / SECOND_US);
	unsigned int ms = (unsigned int)((tick % SECOND_US) / 1000ULL);

	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);

	snprintf(line, sizeof(line), "[%u.%03u] %s\n", sec, ms, body);
	fd = ksceIoOpen(PLUGIN_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd < 0) {
		return;
	}
	ksceIoWrite(fd, line, (SceSize)strlen(line));
	ksceIoClose(fd);
}

static int get_titleid_from_pid(SceUID pid, char *out, unsigned int out_size) {
	SceUID modid;
	SceKernelModuleInfo info;
	const char *s;
	unsigned int len;
	unsigned int i;
	int ret;
	if (!out || out_size < 10) {
		return -1;
	}
	out[0] = '\0';

	ret = ksceKernelGetProcessTitleId(pid, out, out_size);
	if (ret >= 0) {
		out[out_size - 1] = '\0';
		if ((out[0] >= 'A' && out[0] <= 'Z') &&
			(out[1] >= 'A' && out[1] <= 'Z') &&
			(out[2] >= 'A' && out[2] <= 'Z') &&
			(out[3] >= 'A' && out[3] <= 'Z') &&
			(out[4] >= '0' && out[4] <= '9') &&
			(out[5] >= '0' && out[5] <= '9') &&
			(out[6] >= '0' && out[6] <= '9') &&
			(out[7] >= '0' && out[7] <= '9') &&
			(out[8] >= '0' && out[8] <= '9')) {
			out[9] = '\0';
			return 0;
		}
	}

	modid = ksceKernelGetProcessMainModule(pid);
	if (modid < 0) {
		return -2;
	}

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (ksceKernelGetModuleInfo(pid, modid, &info) < 0) {
		return -3;
	}

	s = info.path;
	len = (unsigned int)strlen(s);
	for (i = 0; (i + 8) < len; i++) {
		if (i > 0) {
			char prev = s[i - 1];
			if ((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9')) {
				continue;
			}
		}
		if ((s[i + 0] >= 'A' && s[i + 0] <= 'Z') &&
			(s[i + 1] >= 'A' && s[i + 1] <= 'Z') &&
			(s[i + 2] >= 'A' && s[i + 2] <= 'Z') &&
			(s[i + 3] >= 'A' && s[i + 3] <= 'Z') &&
			(s[i + 4] >= '0' && s[i + 4] <= '9') &&
			(s[i + 5] >= '0' && s[i + 5] <= '9') &&
			(s[i + 6] >= '0' && s[i + 6] <= '9') &&
			(s[i + 7] >= '0' && s[i + 7] <= '9') &&
			(s[i + 8] >= '0' && s[i + 8] <= '9')) {
			char next = s[i + 9];
			if (next == '\0' || next == '/' || next == ':' || next == '.') {
				out[0] = s[i + 0];
				out[1] = s[i + 1];
				out[2] = s[i + 2];
				out[3] = s[i + 3];
				out[4] = s[i + 4];
				out[5] = s[i + 5];
				out[6] = s[i + 6];
				out[7] = s[i + 7];
				out[8] = s[i + 8];
				out[9] = '\0';
				return 0;
			}
		}
	}

	return -4;
}

static int app_should_track(const char *titleid) {
	if (!titleid || !titleid[0] ||
		!((titleid[0] >= 'A' && titleid[0] <= 'Z') &&
		  (titleid[1] >= 'A' && titleid[1] <= 'Z') &&
		  (titleid[2] >= 'A' && titleid[2] <= 'Z') &&
		  (titleid[3] >= 'A' && titleid[3] <= 'Z') &&
		  (titleid[4] >= '0' && titleid[4] <= '9') &&
		  (titleid[5] >= '0' && titleid[5] <= '9') &&
		  (titleid[6] >= '0' && titleid[6] <= '9') &&
		  (titleid[7] >= '0' && titleid[7] <= '9') &&
		  (titleid[8] >= '0' && titleid[8] <= '9') &&
		  titleid[9] == '\0')) {
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
	ksceIoMkdir("ux0:logs", 6);
	ksceIoMkdir(LOG_DIR, 6);

	fd = ksceIoOpen(SESSIONS_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoClose(fd);
	} else {
		fd = ksceIoOpen(SESSIONS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
		if (fd >= 0) {
			static const char *header =
				"#start_tick,end_tick,app,start_pct,end_pct,delta_pct,start_mah,end_mah,delta_mah,start_mv,end_mv,duration_min\n";
			ksceIoWrite(fd, header, (SceSize)strlen(header));
			ksceIoClose(fd);
		}
	}

	fd = ksceIoOpen(SCREEN_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoClose(fd);
	} else {
		fd = ksceIoOpen(SCREEN_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
		if (fd >= 0) {
			static const char *header =
				"#start_tick,end_tick,state,start_pct,end_pct,delta_pct,start_mah,end_mah,delta_mah,start_mv,end_mv,duration_min,reason\n";
			ksceIoWrite(fd, header, (SceSize)strlen(header));
			ksceIoClose(fd);
		}
	}
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
		plugin_logf("session close: reason=%s app=%s dmah=%d dpct=%d dur_ms=%u", reason ? reason : "?", g_state.app, d_mah, d_pct, (unsigned int)(delta_us / 1000ULL));
	}

	memset(&g_state, 0, sizeof(g_state));
}

static void screen_start_if_needed(void) {
	if (g_screen.active) {
		return;
	}
	g_screen.active = 1;
	g_screen.start_tick = ksceKernelGetSystemTimeWide();
	g_screen.start_pct = kscePowerGetBatteryLifePercent();
	g_screen.start_mah = kscePowerGetBatteryRemainCapacity();
	g_screen.start_mv = kscePowerGetBatteryVolt();
}

static void screen_roll(const char *reason) {
	SceUInt64 now_tick;
	if (!g_screen.active) {
		screen_start_if_needed();
		return;
	}
	now_tick = ksceKernelGetSystemTimeWide();
	if ((now_tick - g_screen.start_tick) < SCREEN_ROLL_MIN_US) {
		return;
	}
	screen_close(reason);
	screen_start_if_needed();
}

static void screen_close(const char *reason) {
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
	char line[352];
	SceUID fd;

	if (!g_screen.active) {
		return;
	}

	end_tick = ksceKernelGetSystemTimeWide();
	end_pct = kscePowerGetBatteryLifePercent();
	end_mah = kscePowerGetBatteryRemainCapacity();
	end_mv = kscePowerGetBatteryVolt();

	delta_us = (end_tick > g_screen.start_tick) ? (end_tick - g_screen.start_tick) : 0;
	min_x1000 = delta_us / 60000ULL;
	min_int = (unsigned int)(min_x1000 / 1000ULL);
	min_frac = (unsigned int)(min_x1000 % 1000ULL);
	d_pct = g_screen.start_pct - end_pct;
	d_mah = g_screen.start_mah - end_mah;

	if (delta_us >= MIN_WRITE_SCREEN_US || d_mah > 0 || d_pct > 0) {
		snprintf(line, sizeof(line),
			"%llu,%llu,SCREEN_ON,%d,%d,%d,%d,%d,%d,%d,%d,%u.%03u,%s\n",
			(unsigned long long)g_screen.start_tick,
			(unsigned long long)end_tick,
			g_screen.start_pct,
			end_pct,
			d_pct,
			g_screen.start_mah,
			end_mah,
			d_mah,
			g_screen.start_mv,
			end_mv,
			min_int,
			min_frac,
			reason ? reason : "?");

		fd = ksceIoOpen(SCREEN_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
		if (fd >= 0) {
			ksceIoWrite(fd, line, (SceSize)strlen(line));
			ksceIoClose(fd);
		}
		plugin_logf("screen close: reason=%s dmah=%d dpct=%d dur_ms=%u", reason ? reason : "?", d_mah, d_pct, (unsigned int)(delta_us / 1000ULL));
	}

	memset(&g_screen, 0, sizeof(g_screen));
}

static int proc_event_handler(int pid, int ev, int a3, int a4, int *a5, int a6) {
	SceUInt64 now_tick;

	if (!(ev == 3 || ev == 4 || ev == 5)) {
		return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
	}

	now_tick = ksceKernelGetSystemTimeWide();
	if (pid == g_last_event_pid && ev == g_last_event_code && (now_tick - g_last_event_tick) < EVENT_DEBOUNCE_US) {
		return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
	}
	g_last_event_pid = pid;
	g_last_event_code = ev;
	g_last_event_tick = now_tick;

	if (ev == 5) {
		char titleid[32];
		screen_roll("switch");
		memset(titleid, 0, sizeof(titleid));
		if (get_titleid_from_pid(pid, titleid, sizeof(titleid)) < 0) {
			return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
		}
		if (!app_should_track(titleid)) {
			return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
		}

		if (g_state.active && g_state.pid != pid) {
			session_close("switch");
		}
		if (!g_state.active || g_state.pid != pid) {
			session_start(pid, titleid);
		}
	} else if (ev == 3 || ev == 4) {
		if (g_state.active && g_state.pid == pid) {
			session_close(ev == 3 ? "exit" : "suspend");
		}
		if (ev == 4) {
			screen_close("suspend");
		}
	}

	return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	memset(&g_state, 0, sizeof(g_state));
	memset(&g_screen, 0, sizeof(g_screen));
	ensure_files_ready();

	g_hook_uid = taiHookFunctionImportForKernel(
		KERNEL_PID,
		&g_proc_event_ref,
		"SceProcessmgr",
		TAI_ANY_LIBRARY,
		0x414CC813,
		proc_event_handler);

	if (g_hook_uid < 0) {
		plugin_logf("module start failed: hook=%d", (int)g_hook_uid);
		return SCE_KERNEL_START_FAILED;
	}
	screen_start_if_needed();
	plugin_logf("module start ok");
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	session_close("module_stop");
	screen_close("module_stop");
	if (g_hook_uid >= 0) {
		taiHookReleaseForKernel(g_hook_uid, g_proc_event_ref);
		g_hook_uid = -1;
	}
	plugin_logf("module stop");
	return SCE_KERNEL_STOP_SUCCESS;
}
