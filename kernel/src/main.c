#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/power.h>
#include <taihen.h>
#include <stdarg.h>
#include <string.h>

#define SECOND_US 1000000ULL
#define DATA_DIR "ux0:data/BatteryConsumption"
#define SESSIONS_PATH "ux0:data/BatteryConsumption_sessions.csv"
#define SCREEN_PATH "ux0:data/BatteryConsumption_screen.csv"
#define KERNEL_STATE_PATH "ux0:data/BatteryConsumption_kernel_state.txt"
#define LOG_DIR "ux0:logs/BatteryConsumption"
#define PLUGIN_LOG_PATH "ux0:logs/BatteryConsumption/BatteryConsumptionPlugin.log"
#define PLUGIN_TITLEID "BATC00001"
#define MIN_WRITE_SESSION_US (15ULL * SECOND_US)
#define MIN_WRITE_SCREEN_US (5ULL * SECOND_US)
#define SCREEN_ROLL_MIN_US (10ULL * SECOND_US)
#define SCREEN_ROLL_THREAD_US (30ULL * SECOND_US)
#define EVENT_DEBOUNCE_US (800ULL * 1000ULL)
#define CACHE_TOTAL_BYTES (10 * 1024 * 1024)
#define SESSIONS_CACHE_BYTES (7 * 1024 * 1024)
#define SCREEN_CACHE_BYTES (3 * 1024 * 1024)

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
static SceUID g_screen_thread_uid = -1;
static volatile int g_running = 0;
static SceUID g_cache_lock = -1;
static SceUID g_cache_memid = -1;
static char *g_cache_base = NULL;
static char *g_sessions_cache = NULL;
static char *g_screen_cache = NULL;
static int g_sessions_len = 0;
static int g_screen_len = 0;
static volatile int g_flush_requested = 0;
static int g_cache_drop_count = 0;
static void screen_close(const char *reason);

static int is_titleid_like(const char *s) {
	if (!s) {
		return 0;
	}
	return
		(s[0] >= 'A' && s[0] <= 'Z') &&
		(s[1] >= 'A' && s[1] <= 'Z') &&
		(s[2] >= 'A' && s[2] <= 'Z') &&
		(s[3] >= 'A' && s[3] <= 'Z') &&
		(s[4] >= '0' && s[4] <= '9') &&
		(s[5] >= '0' && s[5] <= '9') &&
		(s[6] >= '0' && s[6] <= '9') &&
		(s[7] >= '0' && s[7] <= '9') &&
		(s[8] >= '0' && s[8] <= '9');
}

static int extract_titleid_from_path(const char *path, char *out, unsigned int out_size) {
	const char *p;
	const char *s;
	unsigned int i;
	unsigned int len;
	static const char *roots[] = { "/app/", ":app/", "/patch/", ":patch/", "/addcont/", ":addcont/" };

	if (!path || !out || out_size < 10) {
		return -1;
	}
	for (i = 0; i < (unsigned int)(sizeof(roots) / sizeof(roots[0])); i++) {
		p = strstr(path, roots[i]);
		if (!p) {
			continue;
		}
		p += strlen(roots[i]);
		if (is_titleid_like(p)) {
			memcpy(out, p, 9);
			out[9] = '\0';
			return 0;
		}
	}

	s = path;
	len = (unsigned int)strlen(s);
	for (i = 0; (i + 8) < len; i++) {
		if (is_titleid_like(&s[i])) {
			char next = s[i + 9];
			if (next == '\0' || next == '/' || next == ':' || next == '.') {
				memcpy(out, &s[i], 9);
				out[9] = '\0';
				return 0;
			}
		}
	}
	return -2;
}

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

static void append_file_data(const char *path, const char *data, int len) {
	SceUID fd;
	if (!path || !data || len <= 0) {
		return;
	}
	fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd < 0) {
		return;
	}
	ksceIoWrite(fd, data, (SceSize)len);
	ksceIoClose(fd);
}

static int init_cache_buffers(void) {
	if (g_cache_memid >= 0) {
		return 0;
	}
	g_cache_memid = ksceKernelAllocMemBlock(
		"BatteryConsumptionCache",
		SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
		CACHE_TOTAL_BYTES,
		NULL);
	if (g_cache_memid < 0) {
		return -1;
	}
	if (ksceKernelGetMemBlockBase(g_cache_memid, (void **)&g_cache_base) < 0 || !g_cache_base) {
		ksceKernelFreeMemBlock(g_cache_memid);
		g_cache_memid = -1;
		return -2;
	}
	memset(g_cache_base, 0, CACHE_TOTAL_BYTES);
	g_sessions_cache = g_cache_base;
	g_screen_cache = g_cache_base + SESSIONS_CACHE_BYTES;
	g_sessions_len = 0;
	g_screen_len = 0;
	g_flush_requested = 0;
	g_cache_drop_count = 0;
	return 0;
}

static void free_cache_buffers(void) {
	if (g_cache_memid >= 0) {
		ksceKernelFreeMemBlock(g_cache_memid);
	}
	g_cache_memid = -1;
	g_cache_base = NULL;
	g_sessions_cache = NULL;
	g_screen_cache = NULL;
	g_sessions_len = 0;
	g_screen_len = 0;
}

static void flush_cache_locked(void) {
	if (!g_cache_base || g_cache_lock < 0) {
		return;
	}
	ksceKernelLockMutex(g_cache_lock, 1, NULL);
	if (g_sessions_len > 0) {
		append_file_data(SESSIONS_PATH, g_sessions_cache, g_sessions_len);
		g_sessions_len = 0;
	}
	if (g_screen_len > 0) {
		append_file_data(SCREEN_PATH, g_screen_cache, g_screen_len);
		g_screen_len = 0;
	}
	g_flush_requested = 0;
	ksceKernelUnlockMutex(g_cache_lock, 1);
}

static void cache_append_line(int is_screen, const char *line) {
	char *buf;
	int *lenp;
	int cap;
	int line_len;
	if (!line || !line[0]) {
		return;
	}
	line_len = (int)strlen(line);

	if (!g_cache_base || g_cache_lock < 0 || line_len <= 0) {
		append_file_data(is_screen ? SCREEN_PATH : SESSIONS_PATH, line, line_len);
		return;
	}

	buf = is_screen ? g_screen_cache : g_sessions_cache;
	lenp = is_screen ? &g_screen_len : &g_sessions_len;
	cap = is_screen ? SCREEN_CACHE_BYTES : SESSIONS_CACHE_BYTES;

	ksceKernelLockMutex(g_cache_lock, 1, NULL);
	if ((*lenp + line_len) > cap) {
		g_flush_requested = 1;
		if (*lenp > 0) {
			g_cache_drop_count++;
			ksceKernelUnlockMutex(g_cache_lock, 1);
			return;
		}
	}
	memcpy(buf + *lenp, line, (size_t)line_len);
	*lenp += line_len;
	ksceKernelUnlockMutex(g_cache_lock, 1);
}

static int get_titleid_from_pid(SceUID pid, char *out, unsigned int out_size) {
	SceUID modid;
	SceKernelModuleInfo info;
	int ret;
	if (!out || out_size < 10) {
		return -1;
	}
	out[0] = '\0';

	ret = ksceKernelGetProcessTitleId(pid, out, out_size);
	if (ret >= 0) {
		out[out_size - 1] = '\0';
		if (is_titleid_like(out)) {
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
	if (extract_titleid_from_path(info.path, out, out_size) == 0) {
		return 0;
	}
	return -4;
}

static int app_should_track(const char *titleid) {
	if (!titleid || !titleid[0]) {
		return 0;
	}
	if (strncmp(titleid, PLUGIN_TITLEID, 9) == 0) {
		return 0;
	}
	return 1;
}

static void write_kernel_state(void) {
	char line[320];
	SceUID fd;

	if (g_state.active) {
		snprintf(line, sizeof(line),
			"active=1\napp=%s\npid=%d\nstart_tick=%llu\nstart_pct=%d\nstart_mah=%d\nstart_mv=%d\n",
			g_state.app,
			(int)g_state.pid,
			(unsigned long long)g_state.start_tick,
			g_state.start_pct,
			g_state.start_mah,
			g_state.start_mv);
	} else {
		snprintf(line, sizeof(line), "active=0\n");
	}

	fd = ksceIoOpen(KERNEL_STATE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		return;
	}
	ksceIoWrite(fd, line, (SceSize)strlen(line));
	ksceIoClose(fd);
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

	fd = ksceIoOpen(KERNEL_STATE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd >= 0) {
		static const char *empty_state = "active=0\n";
		ksceIoWrite(fd, empty_state, (SceSize)strlen(empty_state));
		ksceIoClose(fd);
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
	write_kernel_state();
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

		cache_append_line(0, line);
		plugin_logf("session close: reason=%s app=%s dmah=%d dpct=%d dur_ms=%u", reason ? reason : "?", g_state.app, d_mah, d_pct, (unsigned int)(delta_us / 1000ULL));
	}

	memset(&g_state, 0, sizeof(g_state));
	write_kernel_state();
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

static int screen_roll_thread(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	while (g_running) {
		ksceKernelDelayThread((SceUInt32)SCREEN_ROLL_THREAD_US);
		if (!g_running) {
			break;
		}
		flush_cache_locked();
		if (kscePowerIsSuspendRequired()) {
			continue;
		}
		screen_roll("tick");
	}
	return 0;
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

		cache_append_line(1, line);
		plugin_logf("screen close: reason=%s dmah=%d dpct=%d dur_ms=%u", reason ? reason : "?", d_mah, d_pct, (unsigned int)(delta_us / 1000ULL));
	}

	memset(&g_screen, 0, sizeof(g_screen));
}

static int proc_event_handler(int pid, int ev, int a3, int a4, int *a5, int a6) {
	SceUInt64 now_tick;
	int resolved = 0;
	char titleid[32];

	if (!(ev == 1 || ev == 3 || ev == 4 || ev == 5)) {
		return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
	}

	now_tick = ksceKernelGetSystemTimeWide();
	if (pid == g_last_event_pid && ev == g_last_event_code && (now_tick - g_last_event_tick) < EVENT_DEBOUNCE_US) {
		return TAI_CONTINUE(int, g_proc_event_ref, pid, ev, a3, a4, a5, a6);
	}
	g_last_event_pid = pid;
	g_last_event_code = ev;
	g_last_event_tick = now_tick;

	memset(titleid, 0, sizeof(titleid));
	if (get_titleid_from_pid(pid, titleid, sizeof(titleid)) >= 0) {
		resolved = 1;
	}

	if (ev == 1 || ev == 5) {
		screen_roll("switch");
		if (!resolved) {
			strncpy(titleid, "UNKNOWN", sizeof(titleid) - 1);
			titleid[sizeof(titleid) - 1] = '\0';
			plugin_logf("event start fallback unknown: pid=%d ev=%d", pid, ev);
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
	g_cache_lock = ksceKernelCreateMutex("BatteryConsumptionCacheLock", 0, 1, NULL);
	if (g_cache_lock >= 0) {
		if (init_cache_buffers() < 0) {
			plugin_logf("cache init failed; fallback to direct writes");
		} else {
			plugin_logf("cache init ok: %d bytes", CACHE_TOTAL_BYTES);
		}
	}

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

	g_running = 1;
	g_screen_thread_uid = ksceKernelCreateThread(
		"BatteryConsumptionScreenRoll",
		screen_roll_thread,
		0x10000100,
		0x1000,
		0,
		0,
		NULL);
	if (g_screen_thread_uid >= 0) {
		ksceKernelStartThread(g_screen_thread_uid, 0, NULL);
	}
	screen_start_if_needed();
	plugin_logf("module start ok");
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	g_running = 0;
	if (g_screen_thread_uid >= 0) {
		int thread_stat = 0;
		ksceKernelWaitThreadEnd(g_screen_thread_uid, &thread_stat, NULL);
		ksceKernelDeleteThread(g_screen_thread_uid);
		g_screen_thread_uid = -1;
	}
	session_close("module_stop");
	memset(&g_state, 0, sizeof(g_state));
	write_kernel_state();
	screen_close("module_stop");
	flush_cache_locked();
	free_cache_buffers();
	if (g_cache_lock >= 0) {
		ksceKernelDeleteMutex(g_cache_lock);
		g_cache_lock = -1;
	}
	if (g_hook_uid >= 0) {
		taiHookReleaseForKernel(g_hook_uid, g_proc_event_ref);
		g_hook_uid = -1;
	}
	plugin_logf("module stop; dropped_lines=%d", g_cache_drop_count);
	return SCE_KERNEL_STOP_SUCCESS;
}
