#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#define APPS_PATH "ux0:data/BatteryConsumption_apps.txt"
#define SAMPLES_PATH "ux0:data/BatteryConsumption_samples.csv"
#define SESSIONS_PATH "ux0:data/BatteryConsumption_sessions.csv"
#define SCREEN_PATH "ux0:data/BatteryConsumption_screen.csv"
#define PENDING_PATH "ux0:data/BatteryConsumption_pending.txt"
#define KERNEL_STATE_PATH "ux0:data/BatteryConsumption_kernel_state.txt"
#define TITLE_CACHE_PATH "ux0:data/BatteryConsumption_title_cache.csv"
#define APP_LOG_DIR "ux0:logs/BatteryConsumption"
#define APP_LOG_PATH "ux0:logs/BatteryConsumption/BatteryConsumptionApp.log"
#define PLUGIN_LOG_PATH "ux0:logs/BatteryConsumption/BatteryConsumptionPlugin.log"
#define APP_PLUGIN_SKPRX_PATH "app0:plugin/BatteryConsumptionKernel.skprx"
#define APP_PLUGIN_SUPRX_PATH "app0:plugin/BatteryConsumptionKernel.suprx"
#define UR0_PLUGIN_SKPRX_PATH "ur0:tai/BatteryConsumptionKernel.skprx"
#define UR0_PLUGIN_SUPRX_PATH "ur0:tai/BatteryConsumptionKernel.suprx"
#define UX0_PLUGIN_SKPRX_PATH "ux0:tai/BatteryConsumptionKernel.skprx"
#define UX0_PLUGIN_SUPRX_PATH "ux0:tai/BatteryConsumptionKernel.suprx"
#define UR0_OLD_PLUGIN_SKPRX_PATH "ur0:tai/batteryconsumption_kernel.skprx"
#define UR0_OLD_PLUGIN_SUPRX_PATH "ur0:tai/batteryconsumption_kernel.suprx"
#define UX0_OLD_PLUGIN_SKPRX_PATH "ux0:tai/batteryconsumption_kernel.skprx"
#define UX0_OLD_PLUGIN_SUPRX_PATH "ux0:tai/batteryconsumption_kernel.suprx"
#define UR0_CONFIG_PATH "ur0:tai/config.txt"
#define UX0_CONFIG_PATH "ux0:tai/config.txt"

#define MAX_APPS 48
#define MAX_APP_NAME 64
#define MAX_STATUS 128
#define MAX_RECENT_SAMPLES 24
#define MAX_AGG 64
#define MAX_TITLE_CACHE 512
#define NET_POOL_SIZE (256 * 1024)
#define SAMPLE_INTERVAL_US 300000000ULL
#define DRAW_INTERVAL_US 33333ULL
#define STATS_POLL_US 3000000ULL
#define HEAVY_REFRESH_US 30000000ULL
#define UI_LOOP_SLEEP_US 16000ULL
#define WORKER_POLL_US 250000ULL
#define WORKER_HEAVY_US 4000000ULL
#define WORKER_STACK_SIZE 0x20000
#define UI_LINE_WIDTH 58

#define SCREEN_W 960.0f
#define SCREEN_H 544.0f
#define COLOR_BG RGBA8(0x00, 0x2b, 0x36, 0xFF)
#define COLOR_PANEL RGBA8(0x07, 0x36, 0x42, 0xF2)
#define COLOR_PANEL_ALT RGBA8(0x0B, 0x3D, 0x4A, 0xF2)
#define COLOR_BORDER RGBA8(0x11, 0x55, 0x66, 0xFF)
#define COLOR_ACCENT RGBA8(0x35, 0xD8, 0xFF, 0xFF)
#define COLOR_TEXT RGBA8(0xB8, 0xF0, 0xFF, 0xFF)
#define COLOR_MUTED RGBA8(0x7E, 0xB8, 0xC3, 0xFF)
#define COLOR_WARN RGBA8(0xFF, 0xCC, 0x66, 0xFF)
#define COLOR_OK RGBA8(0x77, 0xE4, 0x9A, 0xFF)

typedef struct BatterySnapshot {
	SceRtcTick tick_utc;
	int percent;
	int life_min;
	int charging;
	int online;
	int wifi_connected;
	int low;
	int soh;
	int cycle_count;
	int temp_centi;
	int mv;
	int remain_mah;
	int full_mah;
	int arm_mhz;
	int bus_mhz;
	int gpu_mhz;
	int xbar_mhz;
} BatterySnapshot;

typedef struct PendingSession {
	int active;
	char app[MAX_APP_NAME];
	SceRtcTick start_tick_utc;
	int start_percent;
	int start_mah;
	int start_full_mah;
	int start_mv;
} PendingSession;

typedef struct SampleRow {
	SceRtcTick tick_utc;
	int percent;
	int remain_mah;
	int full_mah;
	int mv;
	int temp_centi;
	int charging;
	int online;
} SampleRow;

typedef struct AppAgg {
	char app[MAX_APP_NAME];
	char label[MAX_APP_NAME];
	double consumed_mah;
	double consumed_pct;
	double minutes;
	int sessions;
} AppAgg;

typedef struct ScreenAgg {
	double consumed_mah;
	double consumed_pct;
	double minutes;
	int sessions;
} ScreenAgg;

typedef struct ActiveKernelState {
	int active;
	char app[MAX_APP_NAME];
	SceUInt64 start_tick;
	int start_mah;
	int start_pct;
	int start_mv;
} ActiveKernelState;

typedef struct TitleCacheEntry {
	char titleid[10];
	char name[MAX_APP_NAME];
} TitleCacheEntry;

typedef enum DashboardTab {
	DASH_TAB_SUMMARY = 0,
	DASH_TAB_STATS = 1
} DashboardTab;

static vita2d_pgf *g_font = NULL;

typedef struct RuntimeCache {
	BatterySnapshot snapshot;
	SampleRow recent_samples[MAX_RECENT_SAMPLES];
	int recent_count;
	AppAgg aggregates[MAX_AGG];
	int agg_count;
	ScreenAgg screen_agg;
	ActiveKernelState active_state;
	int kernel_enabled;
	int valid;
} RuntimeCache;

static RuntimeCache g_cache;
static SceUID g_cache_mutex = -1;
static SceUID g_worker_thread = -1;
static volatile int g_worker_running = 0;
static volatile int g_worker_force_refresh = 1;
static void *g_net_mem = NULL;
static int g_net_ready = 0;
static int g_net_module_loaded = 0;
static TitleCacheEntry g_title_cache[MAX_TITLE_CACHE];
static int g_title_cache_count = 0;
static int g_title_cache_loaded = 0;
static int ensure_sessions_file(void);
static void request_worker_refresh(void);

static void app_logf(const char *fmt, ...) {
	char body[256];
	char line[352];
	va_list ap;
	SceDateTime dt;
	SceUID fd;

	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);

	memset(&dt, 0, sizeof(dt));
	sceRtcGetCurrentClockLocalTime(&dt);
	snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
		dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, body);

	sceIoMkdir("ux0:logs", 6);
	sceIoMkdir(APP_LOG_DIR, 6);
	fd = sceIoOpen(APP_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd < 0) {
		return;
	}
	sceIoWrite(fd, line, (unsigned int)strlen(line));
	sceIoClose(fd);
}

static void shutdown_net_status(void);

static int init_net_status(void) {
	SceNetInitParam net_init;
	int ret;

	if (g_net_ready) {
		return 0;
	}

	ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	if (ret >= 0) {
		g_net_module_loaded = 1;
	}

	g_net_mem = malloc(NET_POOL_SIZE);
	if (!g_net_mem) {
		return -1;
	}
	memset(&net_init, 0, sizeof(net_init));
	net_init.memory = g_net_mem;
	net_init.size = NET_POOL_SIZE;

	if (sceNetInit(&net_init) < 0) {
		shutdown_net_status();
		return -2;
	}
	if (sceNetCtlInit() < 0) {
		shutdown_net_status();
		return -3;
	}
	g_net_ready = 1;
	return 0;
}

static void shutdown_net_status(void) {
	if (g_net_ready) {
		sceNetCtlTerm();
		sceNetTerm();
		g_net_ready = 0;
	}
	if (g_net_mem) {
		free(g_net_mem);
		g_net_mem = NULL;
	}
	if (g_net_module_loaded) {
		sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
		g_net_module_loaded = 0;
	}
}

static int query_wifi_connected(void) {
	int state = 0;
	if (!g_net_ready) {
		return -1;
	}
	if (sceNetCtlInetGetState(&state) < 0) {
		return -1;
	}
	return (state == SCE_NETCTL_STATE_CONNECTED) ? 1 : 0;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
	if (!dst || dst_size == 0) {
		return;
	}
	if (!src) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static void trim_text(char *s) {
	size_t i;
	size_t len;
	if (!s) {
		return;
	}
	len = strlen(s);
	while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || isspace((unsigned char)s[len - 1]))) {
		s[--len] = '\0';
	}
	i = 0;
	while (s[i] && isspace((unsigned char)s[i])) {
		i++;
	}
	if (i > 0) {
		memmove(s, s + i, strlen(s + i) + 1);
	}
}

static void sanitize_app_name(char *s) {
	int i;
	for (i = 0; s && s[i]; i++) {
		if (s[i] == ',') {
			s[i] = ' ';
		}
	}
	trim_text(s);
}

static int is_titleid_string(const char *s) {
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
		(s[8] >= '0' && s[8] <= '9') &&
		s[9] == '\0';
}

static int app_index_of(char apps[][MAX_APP_NAME], int app_count, const char *name) {
	int i;
	for (i = 0; i < app_count; i++) {
		if (strcmp(apps[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

static int add_app_unique(char apps[][MAX_APP_NAME], int *app_count, const char *name) {
	char tmp[MAX_APP_NAME];
	if (!apps || !app_count || !name || !name[0]) {
		return -1;
	}
	copy_text(tmp, sizeof(tmp), name);
	sanitize_app_name(tmp);
	if (!tmp[0]) {
		return -1;
	}
	if (app_index_of(apps, *app_count, tmp) >= 0) {
		return 0;
	}
	if (*app_count >= MAX_APPS) {
		return -1;
	}
	copy_text(apps[*app_count], MAX_APP_NAME, tmp);
	(*app_count)++;
	return 1;
}

static void ensure_default_apps(void) {
	static const char *defaults =
		"Unknown\n"
		"Current game\n"
		"Adrenaline\n"
		"RetroArch\n"
		"VitaShell\n";
	FILE *f = fopen(APPS_PATH, "r");
	if (f) {
		fclose(f);
		return;
	}
	f = fopen(APPS_PATH, "w");
	if (!f) {
		return;
	}
	fputs(defaults, f);
	fclose(f);
}

static int load_apps(char apps[][MAX_APP_NAME]) {
	FILE *f;
	char line[192];
	int app_count = 0;

	ensure_default_apps();
	f = fopen(APPS_PATH, "r");
	if (!f) {
		copy_text(apps[0], MAX_APP_NAME, "Unknown");
		return 1;
	}
	while (fgets(line, sizeof(line), f)) {
		trim_text(line);
		if (!line[0]) {
			continue;
		}
		if (add_app_unique(apps, &app_count, line) < 0 && app_count >= MAX_APPS) {
			break;
		}
	}
	fclose(f);

	if (app_count <= 0) {
		copy_text(apps[0], MAX_APP_NAME, "Unknown");
		app_count = 1;
	}
	return app_count;
}

static int try_discover_titles_for_shell(char apps[][MAX_APP_NAME], int *app_count) {
	SceInt32 app_ids[16];
	int ret = sceAppMgrGetRunningAppIdListForShell(app_ids, (int)(sizeof(app_ids) / sizeof(app_ids[0])));
	int i;
	if (ret < 0) {
		return ret;
	}
	for (i = 0; i < ret; i++) {
		SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(app_ids[i]);
		if (pid < 0) {
			continue;
		}
		char titleid[32];
		memset(titleid, 0, sizeof(titleid));
		if (sceAppMgrGetNameById(pid, titleid) >= 0) {
			add_app_unique(apps, app_count, titleid);
		}
	}
	return 0;
}

static void collect_snapshot(BatterySnapshot *s) {
	int raw_charging;
	memset(s, 0, sizeof(*s));
	sceRtcGetCurrentTick(&s->tick_utc);
	s->percent = scePowerGetBatteryLifePercent();
	s->life_min = scePowerGetBatteryLifeTime();
	raw_charging = scePowerIsBatteryCharging();
	s->online = scePowerIsPowerOnline();
	s->charging = (raw_charging || s->online) ? 1 : 0;
	s->wifi_connected = query_wifi_connected();
	s->low = scePowerIsLowBattery();
	s->soh = scePowerGetBatterySOH();
	s->cycle_count = scePowerGetBatteryCycleCount();
	s->temp_centi = scePowerGetBatteryTemp();
	s->mv = scePowerGetBatteryVolt();
	s->remain_mah = scePowerGetBatteryRemainCapacity();
	s->full_mah = scePowerGetBatteryFullCapacity();
	s->arm_mhz = scePowerGetArmClockFrequency();
	s->bus_mhz = scePowerGetBusClockFrequency();
	s->gpu_mhz = scePowerGetGpuClockFrequency();
	s->xbar_mhz = scePowerGetGpuXbarClockFrequency();
}

static void format_tick_local(const SceRtcTick *utc_tick, char *out, size_t out_size) {
	SceRtcTick local_tick;
	SceDateTime dt;
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (!utc_tick) {
		return;
	}

	local_tick = *utc_tick;
	sceRtcConvertUtcToLocalTime(utc_tick, &local_tick);
	memset(&dt, 0, sizeof(dt));
	if (sceRtcSetTick(&dt, &local_tick) < 0) {
		copy_text(out, out_size, "n/a");
		return;
	}
	snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d",
		dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

static int path_exists(const char *path) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return 0;
	}
	sceIoClose(fd);
	return 1;
}

static int get_file_size_sce(const char *path, int *out_size) {
	SceIoStat st;
	if (!path || !out_size) {
		return -1;
	}
	memset(&st, 0, sizeof(st));
	if (sceIoGetstat(path, &st) < 0) {
		return -1;
	}
	*out_size = (int)st.st_size;
	return 0;
}

static void remove_file_if_exists(const char *path) {
	if (!path) {
		return;
	}
	if (path_exists(path)) {
		sceIoRemove(path);
	}
}

static int copy_file_sce(const char *src_path, const char *dst_path) {
	SceUID src = -1;
	SceUID dst = -1;
	char buf[8192];
	int rd;

	src = sceIoOpen(src_path, SCE_O_RDONLY, 0);
	if (src < 0) {
		return -1;
	}
	dst = sceIoOpen(dst_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (dst < 0) {
		sceIoClose(src);
		return -2;
	}

	while ((rd = sceIoRead(src, buf, sizeof(buf))) > 0) {
		int wr = sceIoWrite(dst, buf, rd);
		if (wr != rd) {
			sceIoClose(src);
			sceIoClose(dst);
			return -3;
		}
	}

	sceIoClose(src);
	sceIoClose(dst);
	return (rd < 0) ? -4 : 0;
}

static int read_text_sce(const char *path, char *out, int out_size) {
	SceUID fd;
	int rd;
	if (!out || out_size <= 1) {
		return -1;
	}
	fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return -1;
	}
	rd = sceIoRead(fd, out, out_size - 1);
	sceIoClose(fd);
	if (rd < 0) {
		return -1;
	}
	out[rd] = '\0';
	return rd;
}

static int write_text_sce(const char *path, const char *text) {
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		return -1;
	}
	if (text && text[0]) {
		sceIoWrite(fd, text, (unsigned int)strlen(text));
	}
	sceIoClose(fd);
	return 0;
}

static int is_plugin_config_line(const char *line) {
	if (!line) {
		return 0;
	}
	return strstr(line, "BatteryConsumptionKernel.skprx") ||
		strstr(line, "batteryconsumption_kernel.skprx") ||
		strstr(line, "BatteryConsumptionKernel.suprx") ||
		strstr(line, "batteryconsumption_kernel.suprx");
}

static int remove_kernel_config_line(const char *config_path) {
	char cfg[32768];
	char out[33792];
	char *p;
	int n;

	if (!config_path) {
		return -1;
	}
	n = read_text_sce(config_path, cfg, sizeof(cfg));
	if (n < 0) {
		return 0;
	}

	out[0] = '\0';
	p = cfg;
	while (*p) {
		char one[512];
		char trim[512];
		size_t len = 0;

		while (p[len] && p[len] != '\n' && len < sizeof(one) - 1) {
			one[len] = p[len];
			len++;
		}
		one[len] = '\0';
		copy_text(trim, sizeof(trim), one);
		trim_text(trim);

		if (!is_plugin_config_line(trim)) {
			if (strlen(out) + len + 2 >= sizeof(out)) {
				return -3;
			}
			strcat(out, one);
			strcat(out, "\n");
		}

		p += len;
		if (*p == '\n') {
			p++;
		}
	}
	return write_text_sce(config_path, out);
}

static int read_file_sce(const char *path, unsigned char *out, int out_size) {
	SceUID fd;
	int rd;
	if (!path || !out || out_size <= 0) {
		return -1;
	}
	fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return -1;
	}
	rd = sceIoRead(fd, out, out_size);
	sceIoClose(fd);
	return rd;
}

static unsigned int rd_u32_le(const unsigned char *p) {
	return (unsigned int)p[0] |
		((unsigned int)p[1] << 8) |
		((unsigned int)p[2] << 16) |
		((unsigned int)p[3] << 24);
}

static unsigned short rd_u16_le(const unsigned char *p) {
	return (unsigned short)(p[0] | (p[1] << 8));
}

static int sfo_get_string_value(const unsigned char *buf, int size, const char *key, char *out, int out_size) {
	unsigned int key_off;
	unsigned int data_off;
	unsigned int count;
	unsigned int i;

	if (!buf || size < 20 || !key || !out || out_size <= 1) {
		return -1;
	}
	out[0] = '\0';
	if (!(buf[0] == 0x00 && buf[1] == 'P' && buf[2] == 'S' && buf[3] == 'F')) {
		return -2;
	}
	key_off = rd_u32_le(buf + 8);
	data_off = rd_u32_le(buf + 12);
	count = rd_u32_le(buf + 16);
	if (key_off >= (unsigned int)size || data_off >= (unsigned int)size) {
		return -3;
	}

	for (i = 0; i < count; i++) {
		unsigned int ent = 20 + i * 16;
		unsigned int kpos;
		unsigned int vpos;
		unsigned int vlen;
		const char *kname;

		if (ent + 16 > (unsigned int)size) {
			break;
		}

		kpos = key_off + rd_u16_le(buf + ent);
		vlen = rd_u32_le(buf + ent + 4);
		vpos = data_off + rd_u32_le(buf + ent + 12);
		if (kpos >= (unsigned int)size || vpos >= (unsigned int)size) {
			continue;
		}
		kname = (const char *)(buf + kpos);
		if (strcmp(kname, key) != 0) {
			continue;
		}

		if (vlen == 0) {
			return -4;
		}
		if ((vpos + vlen) > (unsigned int)size) {
			vlen = (unsigned int)size - vpos;
		}
		if ((int)vlen >= out_size) {
			vlen = (unsigned int)(out_size - 1);
		}
		memcpy(out, buf + vpos, vlen);
		out[vlen] = '\0';
		trim_text(out);
		return out[0] ? 0 : -5;
	}

	return -6;
}

static int title_cache_find(const char *titleid) {
	int i;
	for (i = 0; i < g_title_cache_count; i++) {
		if (strcmp(g_title_cache[i].titleid, titleid) == 0) {
			return i;
		}
	}
	return -1;
}

static void title_cache_load(void) {
	char buf[32768];
	char *line;
	int n;

	if (g_title_cache_loaded) {
		return;
	}
	g_title_cache_loaded = 1;
	g_title_cache_count = 0;

	n = read_text_sce(TITLE_CACHE_PATH, buf, sizeof(buf));
	if (n < 0) {
		return;
	}

	line = strtok(buf, "\n");
	while (line && g_title_cache_count < MAX_TITLE_CACHE) {
		char id[16];
		char name[MAX_APP_NAME];
		char *comma = strchr(line, ',');
		if (!comma) {
			line = strtok(NULL, "\n");
			continue;
		}
		*comma++ = '\0';
		copy_text(id, sizeof(id), line);
		copy_text(name, sizeof(name), comma);
		trim_text(id);
		sanitize_app_name(name);
		if (is_titleid_string(id) && name[0] && title_cache_find(id) < 0) {
			copy_text(g_title_cache[g_title_cache_count].titleid, sizeof(g_title_cache[g_title_cache_count].titleid), id);
			copy_text(g_title_cache[g_title_cache_count].name, sizeof(g_title_cache[g_title_cache_count].name), name);
			g_title_cache_count++;
		}
		line = strtok(NULL, "\n");
	}
}

static void title_cache_add(const char *titleid, const char *name) {
	SceUID fd;
	char line[128];
	int idx;

	if (!titleid || !name || !titleid[0] || !name[0] || !is_titleid_string(titleid)) {
		return;
	}
	title_cache_load();
	idx = title_cache_find(titleid);
	if (idx >= 0) {
		copy_text(g_title_cache[idx].name, sizeof(g_title_cache[idx].name), name);
		return;
	}
	if (g_title_cache_count >= MAX_TITLE_CACHE) {
		return;
	}
	copy_text(g_title_cache[g_title_cache_count].titleid, sizeof(g_title_cache[g_title_cache_count].titleid), titleid);
	copy_text(g_title_cache[g_title_cache_count].name, sizeof(g_title_cache[g_title_cache_count].name), name);
	g_title_cache_count++;

	snprintf(line, sizeof(line), "%s,%s\n", titleid, name);
	fd = sceIoOpen(TITLE_CACHE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, line, (unsigned int)strlen(line));
		sceIoClose(fd);
	}
}

static int resolve_title_name_from_sfo(const char *sfo_path, char *out, int out_size) {
	unsigned char buf[8192];
	int n;
	if (!sfo_path || !out || out_size <= 1) {
		return -1;
	}
	n = read_file_sce(sfo_path, buf, sizeof(buf));
	if (n <= 0) {
		return -2;
	}
	if (sfo_get_string_value(buf, n, "TITLE", out, out_size) == 0) {
		return 0;
	}
	if (sfo_get_string_value(buf, n, "STITLE", out, out_size) == 0) {
		return 0;
	}
	return -3;
}

static void resolve_title_label(const char *app, char *out, int out_size) {
	char path[128];
	char name[MAX_APP_NAME];
	int idx;

	if (!out || out_size <= 1) {
		return;
	}
	out[0] = '\0';
	if (!app || !app[0]) {
		copy_text(out, out_size, "-");
		return;
	}
	if (!is_titleid_string(app)) {
		copy_text(out, out_size, app);
		return;
	}

	title_cache_load();
	idx = title_cache_find(app);
	if (idx >= 0) {
		copy_text(out, out_size, g_title_cache[idx].name);
		return;
	}

	snprintf(path, sizeof(path), "ux0:appmeta/%s/param.sfo", app);
	if (resolve_title_name_from_sfo(path, name, sizeof(name)) < 0) {
		snprintf(path, sizeof(path), "ur0:appmeta/%s/param.sfo", app);
		if (resolve_title_name_from_sfo(path, name, sizeof(name)) < 0) {
			snprintf(path, sizeof(path), "ux0:app/%s/sce_sys/param.sfo", app);
			if (resolve_title_name_from_sfo(path, name, sizeof(name)) < 0) {
				snprintf(path, sizeof(path), "ux0:patch/%s/sce_sys/param.sfo", app);
				if (resolve_title_name_from_sfo(path, name, sizeof(name)) < 0) {
					snprintf(path, sizeof(path), "vs0:app/%s/sce_sys/param.sfo", app);
					if (resolve_title_name_from_sfo(path, name, sizeof(name)) < 0) {
						copy_text(out, out_size, app);
						return;
					}
				}
			}
		}
	}

	sanitize_app_name(name);
	if (!name[0]) {
		copy_text(out, out_size, app);
		return;
	}
	title_cache_add(app, name);
	copy_text(out, out_size, name);
}

static int ensure_kernel_config_line(const char *config_path) {
	char cfg[32768];
	const char *line = "ur0:tai/BatteryConsumptionKernel.skprx";
	char out[33792];
	int n = read_text_sce(config_path, cfg, sizeof(cfg));
	int has_kernel = 0;
	int inserted = 0;
	char *p;

	if (n < 0) {
		snprintf(out, sizeof(out), "*KERNEL\n%s\n", line);
		return write_text_sce(config_path, out);
	}

	out[0] = '\0';
	p = cfg;
	while (*p) {
		char one[512];
		char trim[512];
		size_t len = 0;
		while (p[len] && p[len] != '\n' && len < sizeof(one) - 1) {
			one[len] = p[len];
			len++;
		}
		one[len] = '\0';
		copy_text(trim, sizeof(trim), one);
		trim_text(trim);

		if (strcmp(trim, "*KERNEL") == 0) {
			has_kernel = 1;
			if (strlen(out) + len + 2 >= sizeof(out)) {
				return -3;
			}
			strcat(out, one);
			strcat(out, "\n");
			if (!inserted) {
				if (strlen(out) + strlen(line) + 2 >= sizeof(out)) {
					return -3;
				}
				strcat(out, line);
				strcat(out, "\n");
				inserted = 1;
			}
		} else if (strstr(trim, "BatteryConsumptionKernel.skprx") ||
		           strstr(trim, "batteryconsumption_kernel.skprx") ||
		           strstr(trim, "BatteryConsumptionKernel.suprx") ||
		           strstr(trim, "batteryconsumption_kernel.suprx")) {
			/* drop old plugin lines and keep only the new canonical line */
		} else {
			if (strlen(out) + len + 2 >= sizeof(out)) {
				return -3;
			}
			strcat(out, one);
			strcat(out, "\n");
		}
		p += len;
		if (*p == '\n') {
			p++;
		}
	}

	if (!has_kernel) {
		if (strlen(out) > 0 && out[strlen(out) - 1] != '\n') {
			if (strlen(out) + 1 >= sizeof(out)) {
				return -3;
			}
			strcat(out, "\n");
		}
		if (strlen(out) + strlen("*KERNEL\n") + strlen(line) + 2 >= sizeof(out)) {
			return -3;
		}
		strcat(out, "*KERNEL\n");
		strcat(out, line);
		strcat(out, "\n");
	} else if (!inserted) {
		if (strlen(out) + strlen(line) + 2 >= sizeof(out)) {
			return -3;
		}
		strcat(out, line);
		strcat(out, "\n");
	}

	return write_text_sce(config_path, out);
}

static int __attribute__((noinline)) kernel_plugin_is_enabled(void) {
	char cfg[2048];
	int n;
	if (!path_exists(UR0_PLUGIN_SKPRX_PATH)) {
		return 0;
	}
	n = read_text_sce(UR0_CONFIG_PATH, cfg, sizeof(cfg));
	if (n < 0) {
		return 0;
	}
	return strstr(cfg, "ur0:tai/BatteryConsumptionKernel.skprx") != NULL;
}

static int plugins_need_update(void) {
	int app_skprx = 0;
	int ur0_skprx = 0;
	int app_suprx = 0;
	int ur0_suprx = 0;
	if (get_file_size_sce(APP_PLUGIN_SKPRX_PATH, &app_skprx) < 0) {
		return -1;
	}
	if (get_file_size_sce(APP_PLUGIN_SUPRX_PATH, &app_suprx) < 0) {
		return -2;
	}
	if (get_file_size_sce(UR0_PLUGIN_SKPRX_PATH, &ur0_skprx) < 0) {
		return 1;
	}
	if (get_file_size_sce(UR0_PLUGIN_SUPRX_PATH, &ur0_suprx) < 0) {
		return 1;
	}
	if (app_skprx != ur0_skprx || app_suprx != ur0_suprx) {
		return 1;
	}
	return 0;
}

static int install_plugins(void) {
	int ret_skprx;
	int ret_suprx;
	int ret_cfg;
	sceIoMkdir("ur0:tai", 6);
	sceIoMkdir("ux0:tai", 6);
	remove_file_if_exists(UR0_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UR0_PLUGIN_SUPRX_PATH);
	remove_file_if_exists(UR0_OLD_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UR0_OLD_PLUGIN_SUPRX_PATH);
	remove_file_if_exists(UX0_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UX0_PLUGIN_SUPRX_PATH);
	remove_file_if_exists(UX0_OLD_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UX0_OLD_PLUGIN_SUPRX_PATH);

	ret_skprx = copy_file_sce(APP_PLUGIN_SKPRX_PATH, UR0_PLUGIN_SKPRX_PATH);
	if (ret_skprx < 0) {
		app_logf("plugin install failed: copy skprx %s -> %s ret=%d", APP_PLUGIN_SKPRX_PATH, UR0_PLUGIN_SKPRX_PATH, ret_skprx);
		return -10 + ret_skprx;
	}
	ret_suprx = copy_file_sce(APP_PLUGIN_SUPRX_PATH, UR0_PLUGIN_SUPRX_PATH);
	if (ret_suprx < 0) {
		app_logf("plugin install failed: copy suprx %s -> %s ret=%d", APP_PLUGIN_SUPRX_PATH, UR0_PLUGIN_SUPRX_PATH, ret_suprx);
		return -20 + ret_suprx;
	}

	ret_cfg = ensure_kernel_config_line(UR0_CONFIG_PATH);
	if (ret_cfg < 0) {
		app_logf("plugin install failed: config update ret=%d", ret_cfg);
		return -30 + ret_cfg;
	}
	if (path_exists(UX0_CONFIG_PATH)) {
		ensure_kernel_config_line(UX0_CONFIG_PATH);
	}
	app_logf("plugin install ok: %s and %s", UR0_PLUGIN_SKPRX_PATH, UR0_PLUGIN_SUPRX_PATH);
	return 0;
}

static int reset_stats_and_uninstall_plugins(void) {
	int err = 0;
	int ret;

	remove_file_if_exists(SESSIONS_PATH);
	remove_file_if_exists(SCREEN_PATH);
	remove_file_if_exists(SAMPLES_PATH);
	remove_file_if_exists(PENDING_PATH);
	remove_file_if_exists(KERNEL_STATE_PATH);
	remove_file_if_exists(TITLE_CACHE_PATH);
	remove_file_if_exists(APP_LOG_PATH);
	remove_file_if_exists(PLUGIN_LOG_PATH);

	remove_file_if_exists(UR0_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UR0_PLUGIN_SUPRX_PATH);
	remove_file_if_exists(UR0_OLD_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UR0_OLD_PLUGIN_SUPRX_PATH);
	remove_file_if_exists(UX0_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UX0_PLUGIN_SUPRX_PATH);
	remove_file_if_exists(UX0_OLD_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UX0_OLD_PLUGIN_SUPRX_PATH);

	ret = remove_kernel_config_line(UR0_CONFIG_PATH);
	if (ret < 0) {
		err = ret;
	}
	if (path_exists(UX0_CONFIG_PATH)) {
		ret = remove_kernel_config_line(UX0_CONFIG_PATH);
		if (ret < 0 && err == 0) {
			err = ret;
		}
	}

	g_title_cache_count = 0;
	g_title_cache_loaded = 0;
	ensure_sessions_file();
	request_worker_refresh();
	return err;
}

static int __attribute__((noinline)) append_sample(const BatterySnapshot *s) {
	FILE *f = fopen(SAMPLES_PATH, "a");
	if (!f) {
		return -1;
	}
	fprintf(f, "%llu,%d,%d,%d,%d,%d,%d,%d\n",
		(unsigned long long)s->tick_utc.tick,
		s->percent,
		s->remain_mah,
		s->full_mah,
		s->mv,
		s->temp_centi,
		s->charging,
		s->online);
	fclose(f);
	return 0;
}

static int __attribute__((noinline)) load_recent_samples(SampleRow *rows, int max_rows) {
	FILE *f = fopen(SAMPLES_PATH, "r");
	char line[256];
	int count = 0;
	int head = 0;
	if (!f || !rows || max_rows <= 0) {
		if (f) fclose(f);
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned long long tick = 0;
		SampleRow r;
		int parsed = sscanf(line, "%llu,%d,%d,%d,%d,%d,%d,%d",
			&tick, &r.percent, &r.remain_mah, &r.full_mah, &r.mv, &r.temp_centi, &r.charging, &r.online);
		if (parsed != 8) {
			continue;
		}
		r.tick_utc.tick = (SceUInt64)tick;
		rows[head] = r;
		head = (head + 1) % max_rows;
		if (count < max_rows) {
			count++;
		}
	}
	fclose(f);

	if (count > 0) {
		SampleRow tmp[MAX_RECENT_SAMPLES];
		int i;
		for (i = 0; i < count; i++) {
			int src = (head - count + i + max_rows) % max_rows;
			tmp[i] = rows[src];
		}
		for (i = 0; i < count; i++) {
			rows[i] = tmp[i];
		}
	}
	return count;
}

static int save_pending(const PendingSession *p) {
	FILE *f;
	if (!p || !p->active) {
		return -1;
	}
	f = fopen(PENDING_PATH, "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "app=%s\n", p->app);
	fprintf(f, "start_tick=%llu\n", (unsigned long long)p->start_tick_utc.tick);
	fprintf(f, "start_pct=%d\n", p->start_percent);
	fprintf(f, "start_mah=%d\n", p->start_mah);
	fprintf(f, "start_full_mah=%d\n", p->start_full_mah);
	fprintf(f, "start_mv=%d\n", p->start_mv);
	fclose(f);
	return 0;
}

static void clear_pending(PendingSession *p) {
	if (p) {
		memset(p, 0, sizeof(*p));
	}
	remove(PENDING_PATH);
}

static int load_pending(PendingSession *p) {
	FILE *f = fopen(PENDING_PATH, "r");
	char line[192];
	if (!p) {
		return -1;
	}
	memset(p, 0, sizeof(*p));
	if (!f) {
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *eq;
		trim_text(line);
		eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq++ = '\0';
		trim_text(line);
		trim_text(eq);
		if (strcmp(line, "app") == 0) {
			copy_text(p->app, sizeof(p->app), eq);
		} else if (strcmp(line, "start_tick") == 0) {
			p->start_tick_utc.tick = (SceUInt64)strtoull(eq, NULL, 10);
		} else if (strcmp(line, "start_pct") == 0) {
			p->start_percent = atoi(eq);
		} else if (strcmp(line, "start_mah") == 0) {
			p->start_mah = atoi(eq);
		} else if (strcmp(line, "start_full_mah") == 0) {
			p->start_full_mah = atoi(eq);
		} else if (strcmp(line, "start_mv") == 0) {
			p->start_mv = atoi(eq);
		}
	}
	fclose(f);
	p->active = (p->app[0] != '\0' && p->start_tick_utc.tick > 0) ? 1 : 0;
	return p->active ? 0 : -1;
}

static int ensure_sessions_file(void) {
	FILE *f = fopen(SESSIONS_PATH, "r");
	if (f) {
		fclose(f);
		return 0;
	}
	f = fopen(SESSIONS_PATH, "w");
	if (!f) {
		return -1;
	}
	fputs("#start_tick,end_tick,app,start_pct,end_pct,delta_pct,start_mah,end_mah,delta_mah,start_mv,end_mv,duration_min\n", f);
	fclose(f);
	return 0;
}

static int __attribute__((noinline)) load_screen_aggregate(ScreenAgg *screen) {
	FILE *f = fopen(SCREEN_PATH, "r");
	char line[384];
	if (!screen) {
		return -1;
	}
	memset(screen, 0, sizeof(*screen));
	if (!f) {
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned long long start_tick = 0, end_tick = 0;
		char state[32];
		int start_pct = 0, end_pct = 0, d_pct = 0;
		int start_mah = 0, end_mah = 0, d_mah = 0;
		int start_mv = 0, end_mv = 0;
		double minutes = 0.0;
		char reason[32];
		int parsed;

		if (line[0] == '#') {
			continue;
		}

		memset(state, 0, sizeof(state));
		memset(reason, 0, sizeof(reason));
		parsed = sscanf(line, "%llu,%llu,%31[^,],%d,%d,%d,%d,%d,%d,%d,%d,%lf,%31s",
			&start_tick, &end_tick, state,
			&start_pct, &end_pct, &d_pct,
			&start_mah, &end_mah, &d_mah,
			&start_mv, &end_mv, &minutes, reason);
		if (parsed != 13) {
			continue;
		}
		if (strcmp(state, "SCREEN_ON") != 0) {
			continue;
		}
		screen->sessions += 1;
		screen->minutes += minutes;
		if (d_mah > 0) {
			screen->consumed_mah += (double)d_mah;
		}
		if (d_pct > 0) {
			screen->consumed_pct += (double)d_pct;
		}
	}
	fclose(f);
	return 0;
}

static int append_session(const PendingSession *p, const BatterySnapshot *end_s, int *out_dmah, int *out_dpct) {
	FILE *f;
	SceUInt64 delta_tick;
	double minutes;
	int dmah;
	int dpct;
	char app[MAX_APP_NAME];
	if (!p || !p->active || !end_s) {
		return -1;
	}
	ensure_sessions_file();
	f = fopen(SESSIONS_PATH, "a");
	if (!f) {
		return -1;
	}

	copy_text(app, sizeof(app), p->app);
	sanitize_app_name(app);

	dmah = p->start_mah - end_s->remain_mah;
	dpct = p->start_percent - end_s->percent;
	delta_tick = (end_s->tick_utc.tick > p->start_tick_utc.tick) ? (end_s->tick_utc.tick - p->start_tick_utc.tick) : 0;
	minutes = (double)delta_tick / 60000000.0;

	fprintf(f, "%llu,%llu,%s,%d,%d,%d,%d,%d,%d,%d,%d,%.3f\n",
		(unsigned long long)p->start_tick_utc.tick,
		(unsigned long long)end_s->tick_utc.tick,
		app,
		p->start_percent,
		end_s->percent,
		dpct,
		p->start_mah,
		end_s->remain_mah,
		dmah,
		p->start_mv,
		end_s->mv,
		minutes);
	fclose(f);

	if (out_dmah) {
		*out_dmah = dmah;
	}
	if (out_dpct) {
		*out_dpct = dpct;
	}
	return 0;
}

static int agg_find(AppAgg *aggs, int count, const char *app) {
	int i;
	for (i = 0; i < count; i++) {
		if (strcmp(aggs[i].app, app) == 0) {
			return i;
		}
	}
	return -1;
}

static int cmp_agg_desc(const void *a, const void *b) {
	const AppAgg *aa = (const AppAgg *)a;
	const AppAgg *bb = (const AppAgg *)b;
	if (aa->consumed_mah < bb->consumed_mah) return 1;
	if (aa->consumed_mah > bb->consumed_mah) return -1;
	return 0;
}

static int __attribute__((noinline)) load_aggregates(AppAgg *aggs, int max_aggs) {
	FILE *f = fopen(SESSIONS_PATH, "r");
	char line[320];
	int count = 0;
	if (!f || !aggs || max_aggs <= 0) {
		if (f) fclose(f);
		return 0;
	}
	memset(aggs, 0, sizeof(AppAgg) * (size_t)max_aggs);
	while (fgets(line, sizeof(line), f)) {
		unsigned long long start_tick = 0, end_tick = 0;
		char app[MAX_APP_NAME];
		int start_pct = 0, end_pct = 0, d_pct = 0;
		int start_mah = 0, end_mah = 0, d_mah = 0;
		int start_mv = 0, end_mv = 0;
		double minutes = 0.0;
		int idx;
		int parsed;

		if (line[0] == '#') {
			continue;
		}

		memset(app, 0, sizeof(app));
		parsed = sscanf(line, "%llu,%llu,%63[^,],%d,%d,%d,%d,%d,%d,%d,%d,%lf",
			&start_tick, &end_tick, app,
			&start_pct, &end_pct, &d_pct,
			&start_mah, &end_mah, &d_mah,
			&start_mv, &end_mv, &minutes);
		if (parsed != 12) {
			continue;
		}
		sanitize_app_name(app);
		if (!app[0]) {
			continue;
		}

		idx = agg_find(aggs, count, app);
		if (idx < 0) {
			if (count >= max_aggs) {
				continue;
			}
			idx = count++;
			memset(&aggs[idx], 0, sizeof(aggs[idx]));
			copy_text(aggs[idx].app, sizeof(aggs[idx].app), app);
			resolve_title_label(app, aggs[idx].label, sizeof(aggs[idx].label));
		}

		aggs[idx].sessions += 1;
		aggs[idx].minutes += minutes;
		if (d_mah > 0) {
			aggs[idx].consumed_mah += (double)d_mah;
		}
		if (d_pct > 0) {
			aggs[idx].consumed_pct += (double)d_pct;
		}
	}
	fclose(f);
	qsort(aggs, (size_t)count, sizeof(aggs[0]), cmp_agg_desc);
	return count;
}

static int load_kernel_active_state(ActiveKernelState *state) {
	char buf[512];
	char *line;
	int n;

	if (!state) {
		return -1;
	}
	memset(state, 0, sizeof(*state));
	n = read_text_sce(KERNEL_STATE_PATH, buf, sizeof(buf));
	if (n < 0) {
		return -2;
	}

	line = strtok(buf, "\n");
	while (line) {
		char *eq = strchr(line, '=');
		if (eq) {
			*eq++ = '\0';
			trim_text(line);
			trim_text(eq);
			if (strcmp(line, "active") == 0) {
				state->active = atoi(eq);
			} else if (strcmp(line, "app") == 0) {
				copy_text(state->app, sizeof(state->app), eq);
				sanitize_app_name(state->app);
			} else if (strcmp(line, "start_tick") == 0) {
				state->start_tick = (SceUInt64)strtoull(eq, NULL, 10);
			} else if (strcmp(line, "start_mah") == 0) {
				state->start_mah = atoi(eq);
			} else if (strcmp(line, "start_pct") == 0) {
				state->start_pct = atoi(eq);
			} else if (strcmp(line, "start_mv") == 0) {
				state->start_mv = atoi(eq);
			}
		}
		line = strtok(NULL, "\n");
	}

	if (state->active && !state->app[0]) {
		copy_text(state->app, sizeof(state->app), "UNKNOWN");
	}
	if (!state->active) {
		memset(state, 0, sizeof(*state));
	}
	return 0;
}

static void request_worker_refresh(void) {
	g_worker_force_refresh = 1;
}

static int stats_worker_thread(SceSize args, void *argp) {
	RuntimeCache local;
	SceUInt64 last_sample_us = 0;
	SceUInt64 last_heavy_us = 0;
	(void)args;
	(void)argp;
	memset(&local, 0, sizeof(local));

	while (g_worker_running) {
		SceUInt64 now_us = sceKernelGetProcessTimeWide();
		collect_snapshot(&local.snapshot);

		if (last_sample_us == 0 || (now_us - last_sample_us) >= SAMPLE_INTERVAL_US) {
			append_sample(&local.snapshot);
			last_sample_us = now_us;
			g_worker_force_refresh = 1;
		}

		if (g_worker_force_refresh || last_heavy_us == 0 || (now_us - last_heavy_us) >= WORKER_HEAVY_US) {
			local.recent_count = load_recent_samples(local.recent_samples, MAX_RECENT_SAMPLES);
			local.agg_count = load_aggregates(local.aggregates, MAX_AGG);
			load_screen_aggregate(&local.screen_agg);
			load_kernel_active_state(&local.active_state);
			local.kernel_enabled = kernel_plugin_is_enabled();
			local.valid = 1;
			last_heavy_us = now_us;
			g_worker_force_refresh = 0;
		}

		if (g_cache_mutex >= 0) {
			sceKernelLockMutex(g_cache_mutex, 1, NULL);
			g_cache = local;
			sceKernelUnlockMutex(g_cache_mutex, 1);
		}

		sceKernelDelayThread(WORKER_POLL_US);
	}

	return 0;
}

static int start_stats_worker(void) {
	memset(&g_cache, 0, sizeof(g_cache));
	g_cache_mutex = sceKernelCreateMutex("BatteryConsumptionCache", 0, 1, NULL);
	if (g_cache_mutex < 0) {
		return -1;
	}
	g_worker_running = 1;
	g_worker_force_refresh = 1;
	g_worker_thread = sceKernelCreateThread(
		"BatteryConsumptionWorker",
		stats_worker_thread,
		0x10000100,
		WORKER_STACK_SIZE,
		0,
		0,
		NULL);
	if (g_worker_thread < 0) {
		g_worker_running = 0;
		sceKernelDeleteMutex(g_cache_mutex);
		g_cache_mutex = -1;
		return -2;
	}
	if (sceKernelStartThread(g_worker_thread, 0, NULL) < 0) {
		g_worker_running = 0;
		sceKernelDeleteThread(g_worker_thread);
		g_worker_thread = -1;
		sceKernelDeleteMutex(g_cache_mutex);
		g_cache_mutex = -1;
		return -3;
	}
	return 0;
}

static void stop_stats_worker(void) {
	if (g_worker_thread >= 0) {
		int stat = 0;
		g_worker_running = 0;
		sceKernelWaitThreadEnd(g_worker_thread, &stat, NULL);
		sceKernelDeleteThread(g_worker_thread);
		g_worker_thread = -1;
	}
	if (g_cache_mutex >= 0) {
		sceKernelDeleteMutex(g_cache_mutex);
		g_cache_mutex = -1;
	}
}

static int copy_cache(RuntimeCache *out) {
	if (!out || g_cache_mutex < 0) {
		return -1;
	}
	sceKernelLockMutex(g_cache_mutex, 1, NULL);
	*out = g_cache;
	sceKernelUnlockMutex(g_cache_mutex, 1);
	return out->valid ? 0 : -1;
}

static void gui_textf(float x, float y, float scale, unsigned int color, const char *fmt, ...) {
	char buf[256];
	va_list ap;
	if (!g_font) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	vita2d_pgf_draw_text(g_font, (int)x, (int)y, color, scale, buf);
}

static int gui_text_width(float scale, const char *text) {
	if (!g_font || !text) {
		return 0;
	}
	return vita2d_pgf_text_width(g_font, scale, text);
}

static void gui_panel(float x, float y, float w, float h, unsigned int fill, unsigned int border) {
	vita2d_draw_rectangle(x, y, w, h, fill);
	vita2d_draw_rectangle(x, y, w, 2.0f, border);
	vita2d_draw_rectangle(x, y + h - 2.0f, w, 2.0f, border);
	vita2d_draw_rectangle(x, y, 2.0f, h, border);
	vita2d_draw_rectangle(x + w - 2.0f, y, 2.0f, h, border);
}

static void shorten_label(const char *src, char *dst, size_t dst_size, size_t max_chars) {
	size_t len;
	if (!dst || dst_size == 0) {
		return;
	}
	dst[0] = '\0';
	if (!src) {
		return;
	}
	len = strlen(src);
	if (len <= max_chars || max_chars < 4) {
		copy_text(dst, dst_size, src);
		return;
	}
	if (max_chars >= dst_size) {
		max_chars = dst_size - 1;
	}
	memcpy(dst, src, max_chars - 3);
	dst[max_chars - 3] = '.';
	dst[max_chars - 2] = '.';
	dst[max_chars - 1] = '.';
	dst[max_chars] = '\0';
}

static void get_power_state_text(
	const BatterySnapshot *s,
	const SampleRow *samples,
	int sample_count,
	char *out,
	size_t out_size
) {
	int trend = 0;
	if (!out || out_size == 0 || !s) {
		return;
	}
	if (sample_count >= 2) {
		trend = samples[sample_count - 1].remain_mah - samples[sample_count - 2].remain_mah;
	}

	if (trend >= 2) {
		copy_text(out, out_size, "Charging");
	} else if (trend <= -2) {
		copy_text(out, out_size, "Discharging");
	} else if (s->online || s->charging) {
		copy_text(out, out_size, "USB connected");
	} else {
		copy_text(out, out_size, "On battery");
	}
}

static int build_top_rows_with_system(
	const AppAgg *aggs,
	int agg_count,
	const ScreenAgg *screen,
	AppAgg *out,
	int max_out
) {
	int i;
	int count = 0;
	if (!out || max_out <= 0) {
		return 0;
	}
	memset(out, 0, sizeof(AppAgg) * (size_t)max_out);

	for (i = 0; i < agg_count && count < max_out; i++) {
		out[count++] = aggs[i];
	}

	if (count < max_out) {
		memset(&out[count], 0, sizeof(out[count]));
		copy_text(out[count].app, sizeof(out[count].app), "System");
		copy_text(out[count].label, sizeof(out[count].label), "System");
		if (screen) {
			out[count].consumed_mah = screen->consumed_mah;
			out[count].consumed_pct = screen->consumed_pct;
			out[count].minutes = screen->minutes;
			out[count].sessions = screen->sessions;
		}
		count++;
	}

	qsort(out, (size_t)count, sizeof(out[0]), cmp_agg_desc);
	return count;
}

static void draw_header(DashboardTab tab, const char *time_str) {
	const char *s0 = "Summary";
	const char *s1 = "Stats";
	float tab_y = 48.0f;
	gui_panel(0.0f, 0.0f, SCREEN_W, 40.0f, COLOR_PANEL_ALT, COLOR_BORDER);
	gui_textf(20.0f, 27.0f, 1.05f, COLOR_ACCENT, "BatteryConsumption");
	gui_textf(720.0f, 27.0f, 0.9f, COLOR_MUTED, "%s", time_str);

	gui_panel(20.0f, tab_y, 150.0f, 34.0f, tab == DASH_TAB_SUMMARY ? COLOR_ACCENT : COLOR_PANEL, COLOR_BORDER);
	gui_panel(176.0f, tab_y, 150.0f, 34.0f, tab == DASH_TAB_STATS ? COLOR_ACCENT : COLOR_PANEL, COLOR_BORDER);
	gui_textf(44.0f, tab_y + 24.0f, 0.95f, tab == DASH_TAB_SUMMARY ? COLOR_BG : COLOR_TEXT, "%s", s0);
	gui_textf(220.0f, tab_y + 24.0f, 0.95f, tab == DASH_TAB_STATS ? COLOR_BG : COLOR_TEXT, "%s", s1);
}

static void draw_battery_meter(float x, float y, float w, float h, int percent) {
	float fill_w;
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}
	gui_panel(x, y, w, h, COLOR_BG, COLOR_BORDER);
	fill_w = (w - 6.0f) * ((float)percent / 100.0f);
	vita2d_draw_rectangle(x + 3.0f, y + 3.0f, fill_w, h - 6.0f, percent >= 20 ? COLOR_OK : COLOR_WARN);
}

static void draw_summary_tab(
	const BatterySnapshot *s,
	const ScreenAgg *screen,
	const AppAgg *aggs,
	int agg_count,
	int kernel_enabled,
	const char *status,
	const SampleRow *samples,
	int sample_count
) {
	char power[48];
	char life[32];
	char temp_text[32];
	char state_short[64];
	char history_text[64];
	int charging_flag;
	int power_flag;
	int wifi_flag;
	int k;
	int from;
	float y0 = 96.0f;
	ScreenAgg effective_screen;
	get_power_state_text(s, samples, sample_count, power, sizeof(power));
	memset(&effective_screen, 0, sizeof(effective_screen));
	if (screen) {
		effective_screen = *screen;
	}
	if (effective_screen.consumed_mah <= 0.01 && sample_count > 1) {
		int dmah = samples[0].remain_mah - samples[sample_count - 1].remain_mah;
		SceUInt64 dt = (samples[sample_count - 1].tick_utc.tick > samples[0].tick_utc.tick)
			? (samples[sample_count - 1].tick_utc.tick - samples[0].tick_utc.tick)
			: 0;
		if (dmah > 0) {
			effective_screen.consumed_mah = (double)dmah;
			effective_screen.minutes = (double)dt / 60000000.0;
		}
	}
	if (s->life_min >= 0) {
		snprintf(life, sizeof(life), "%d min", s->life_min);
	} else {
		copy_text(life, sizeof(life), "n/a");
	}
	if (s->temp_centi >= 0) {
		snprintf(temp_text, sizeof(temp_text), "%d.%02d C", s->temp_centi / 100, abs(s->temp_centi % 100));
	} else {
		copy_text(temp_text, sizeof(temp_text), "n/a");
	}
	shorten_label(status, state_short, sizeof(state_short), 40);
	if (sample_count > 1) {
		int d_pct = samples[0].percent - samples[sample_count - 1].percent;
		int d_mah = samples[0].remain_mah - samples[sample_count - 1].remain_mah;
		snprintf(history_text, sizeof(history_text), "Window drop: %d%% / %d mAh", d_pct, d_mah);
	} else {
		copy_text(history_text, sizeof(history_text), "Window drop: n/a");
	}
	charging_flag = s->charging ? 1 : 0;
	power_flag = s->online ? 1 : 0;
	wifi_flag = s->wifi_connected;
	if (sample_count >= 2) {
		int inc = 0;
		from = sample_count - 4;
		if (from < 1) {
			from = 1;
		}
		for (k = from; k < sample_count; k++) {
			int d = samples[k].remain_mah - samples[k - 1].remain_mah;
			if (d >= 1) {
				inc = 1;
				break;
			}
		}
		if (inc) {
			charging_flag = 1;
			power_flag = 1;
		}
	}

	gui_panel(20.0f, y0, 920.0f, 170.0f, COLOR_PANEL, COLOR_BORDER);
	gui_textf(42.0f, y0 + 32.0f, 0.95f, COLOR_MUTED, "Battery");
	gui_textf(42.0f, y0 + 98.0f, 2.2f, COLOR_ACCENT, "%d%%", s->percent);
	draw_battery_meter(220.0f, y0 + 58.0f, 360.0f, 36.0f, s->percent);
	gui_textf(220.0f, y0 + 132.0f, 0.95f, COLOR_TEXT, "%d / %d mAh", s->remain_mah, s->full_mah);
	gui_textf(620.0f, y0 + 58.0f, 0.84f, COLOR_TEXT, "Power: %s", power);
	gui_textf(620.0f, y0 + 84.0f, 0.84f, COLOR_TEXT, "Runtime: %s", life);
	gui_textf(620.0f, y0 + 110.0f, 0.84f, COLOR_TEXT, "Volt/Temp: %d mV  %s", s->mv, temp_text);
	gui_textf(620.0f, y0 + 136.0f, 0.84f, COLOR_TEXT, "Health: SOH %d%%  cycles %d", s->soh, s->cycle_count);
	gui_textf(620.0f, y0 + 162.0f, 0.80f, COLOR_TEXT, "Flags: chg=%s  power=%s  wifi=%s  low=%s",
		charging_flag ? "Y" : "N",
		power_flag ? "Y" : "N",
		(wifi_flag < 0) ? "?" : (wifi_flag ? "Y" : "N"),
		s->low ? "Y" : "N");

	gui_panel(20.0f, 278.0f, 452.0f, 220.0f, COLOR_PANEL, COLOR_BORDER);
	gui_textf(40.0f, 308.0f, 0.95f, COLOR_MUTED, "Usage");
	if (effective_screen.minutes > 0.0 || effective_screen.consumed_mah > 0.0) {
		gui_textf(40.0f, 344.0f, 0.9f, COLOR_TEXT, "Screen active: %.1f min", effective_screen.minutes);
		gui_textf(40.0f, 374.0f, 0.9f, COLOR_TEXT, "Screen drain:  %.1f mAh", effective_screen.consumed_mah);
	}
	if (agg_count > 0) {
		gui_textf(40.0f, 414.0f, 0.9f, COLOR_TEXT, "Top app: %s", aggs[0].label[0] ? aggs[0].label : aggs[0].app);
		gui_textf(40.0f, 444.0f, 0.9f, COLOR_TEXT, "Top app drain: %.1f mAh", aggs[0].consumed_mah);
	} else {
		gui_textf(40.0f, 414.0f, 0.9f, COLOR_TEXT, "No app stats yet");
	}

	gui_panel(488.0f, 278.0f, 452.0f, 220.0f, COLOR_PANEL, COLOR_BORDER);
	gui_textf(508.0f, 308.0f, 0.95f, COLOR_MUTED, "Telemetry");
	gui_textf(508.0f, 344.0f, 0.86f, kernel_enabled ? COLOR_OK : COLOR_WARN, "Tracker: %s", kernel_enabled ? "enabled" : "not ready");
	gui_textf(508.0f, 372.0f, 0.78f, COLOR_TEXT, "State: %s", state_short);
	gui_textf(508.0f, 398.0f, 0.78f, COLOR_TEXT, "Clocks: ARM %d  BUS %d MHz", s->arm_mhz, s->bus_mhz);
	gui_textf(508.0f, 422.0f, 0.78f, COLOR_TEXT, "GPU %d  XBAR %d MHz", s->gpu_mhz, s->xbar_mhz);
	gui_textf(508.0f, 446.0f, 0.78f, COLOR_TEXT, "%s", history_text);
	gui_textf(508.0f, 470.0f, 0.74f, COLOR_MUTED, "L/R tabs  CROSS sample  TRIANGLE reload");
	gui_textf(508.0f, 492.0f, 0.74f, COLOR_MUTED, "SQUARE sync  SELECT reboot  L+R+START reset");
}

static void draw_stats_tab(
	const BatterySnapshot *snapshot,
	const SampleRow *samples,
	int sample_count,
	const AppAgg *aggs,
	int agg_count,
	const ScreenAgg *screen,
	const ActiveKernelState *active_state,
	const char *status
) {
	int i;
	ScreenAgg effective_screen;
	AppAgg ranked[16];
	const float table_x = 34.0f;
	const float table_y = 150.0f;
	const float table_w = 892.0f;
	const float header_h = 30.0f;
	const float row_h = 30.0f;
	const int table_rows = 7;
	const float col_app = 40.0f;
	const float col_cur = 340.0f;
	const float col_total = 470.0f;
	const float col_uptime = 610.0f;
	const float col_rate = 760.0f;
	memset(&effective_screen, 0, sizeof(effective_screen));
	if (screen) {
		effective_screen = *screen;
	}
	if (effective_screen.consumed_mah <= 0.01 && sample_count > 1) {
		int dmah = samples[0].remain_mah - samples[sample_count - 1].remain_mah;
		SceUInt64 dt = (samples[sample_count - 1].tick_utc.tick > samples[0].tick_utc.tick)
			? (samples[sample_count - 1].tick_utc.tick - samples[0].tick_utc.tick)
			: 0;
		if (dmah > 0) {
			effective_screen.consumed_mah = (double)dmah;
			effective_screen.minutes = (double)dt / 60000000.0;
		}
	}
	int ranked_count = build_top_rows_with_system(aggs, agg_count, &effective_screen, ranked, 16);
	float y;

	gui_panel(20.0f, 96.0f, 920.0f, 300.0f, COLOR_PANEL, COLOR_BORDER);
	gui_textf(40.0f, 126.0f, 0.95f, COLOR_MUTED, "Top Apps");
	vita2d_draw_rectangle(table_x, table_y, table_w, header_h, COLOR_PANEL_ALT);
	vita2d_draw_rectangle(320.0f, table_y, 1.0f, header_h + row_h * table_rows, COLOR_BORDER);
	vita2d_draw_rectangle(450.0f, table_y, 1.0f, header_h + row_h * table_rows, COLOR_BORDER);
	vita2d_draw_rectangle(580.0f, table_y, 1.0f, header_h + row_h * table_rows, COLOR_BORDER);
	vita2d_draw_rectangle(740.0f, table_y, 1.0f, header_h + row_h * table_rows, COLOR_BORDER);
	gui_textf(col_app, table_y + 21.0f, 0.78f, COLOR_MUTED, "App");
	gui_textf(col_cur, table_y + 21.0f, 0.78f, COLOR_MUTED, "Cur mAh");
	gui_textf(col_total, table_y + 21.0f, 0.78f, COLOR_MUTED, "Total mAh");
	gui_textf(col_uptime, table_y + 21.0f, 0.78f, COLOR_MUTED, "Up min");
	gui_textf(col_rate, table_y + 21.0f, 0.78f, COLOR_MUTED, "mAh/h");

	y = table_y + header_h;
	for (i = 0; i < table_rows; i++) {
		if ((i % 2) == 0) {
			vita2d_draw_rectangle(table_x, y, table_w, row_h, COLOR_PANEL_ALT);
		}
		if (i < ranked_count) {
			char label[28];
			double rate = (ranked[i].minutes > 0.01) ? (ranked[i].consumed_mah * 60.0 / ranked[i].minutes) : 0.0;
			double cur_mah = 0.0;
			shorten_label(ranked[i].label[0] ? ranked[i].label : ranked[i].app, label, sizeof(label), 20);
			if (active_state && active_state->active && snapshot &&
				strcmp(ranked[i].app, active_state->app) == 0 &&
				active_state->start_mah > 0 && snapshot->remain_mah > 0) {
				int dmah = active_state->start_mah - snapshot->remain_mah;
				if (dmah > 0) {
					cur_mah = (double)dmah;
				}
			} else if ((strcmp(ranked[i].app, "System") == 0 || strcmp(ranked[i].app, "SYSTEM") == 0) && sample_count >= 2) {
				int dmah_sys = samples[sample_count - 2].remain_mah - samples[sample_count - 1].remain_mah;
				if (dmah_sys > 0) {
					cur_mah = (double)dmah_sys;
				}
			}
			gui_textf(col_app, y + 22.0f, 0.76f, COLOR_TEXT, "%d. %s", i + 1, label);
			gui_textf(col_cur, y + 22.0f, 0.76f, COLOR_TEXT, "%.1f", cur_mah);
			gui_textf(col_total, y + 22.0f, 0.76f, COLOR_TEXT, "%.1f", ranked[i].consumed_mah);
			gui_textf(col_uptime, y + 22.0f, 0.76f, COLOR_TEXT, "%.1f", ranked[i].minutes);
			gui_textf(col_rate, y + 22.0f, 0.76f, COLOR_TEXT, "%.1f", rate);
		}
		y += row_h;
	}

	gui_panel(20.0f, 378.0f, 920.0f, 120.0f, COLOR_PANEL, COLOR_BORDER);
	gui_textf(40.0f, 404.0f, 0.92f, COLOR_MUTED, "Recent samples");
	if (sample_count > 0) {
		int start = sample_count - 3;
		if (start < 0) {
			start = 0;
		}
		for (i = start; i < sample_count; i++) {
			char ts[40];
			format_tick_local(&samples[i].tick_utc, ts, sizeof(ts));
			gui_textf(40.0f + (float)(i - start) * 300.0f, 440.0f, 0.78f, COLOR_TEXT, "%s", ts);
			gui_textf(40.0f + (float)(i - start) * 300.0f, 468.0f, 0.82f, COLOR_ACCENT, "%d%%  %dmAh", samples[i].percent, samples[i].remain_mah);
		}
	} else {
		gui_textf(40.0f, 450.0f, 0.85f, COLOR_TEXT, "No sample data");
	}
	gui_textf(620.0f, 490.0f, 0.78f, COLOR_MUTED, "Status: %s", status);
}

static void draw_reboot_modal(void) {
	vita2d_draw_rectangle(0.0f, 0.0f, SCREEN_W, SCREEN_H, RGBA8(0x00, 0x00, 0x00, 0x90));
	gui_panel(220.0f, 182.0f, 520.0f, 180.0f, COLOR_PANEL_ALT, COLOR_ACCENT);
	gui_textf(260.0f, 236.0f, 1.2f, COLOR_ACCENT, "Reboot PS Vita?");
	gui_textf(260.0f, 276.0f, 0.9f, COLOR_TEXT, "Apply updated plugins and restart system.");
	gui_textf(260.0f, 330.0f, 0.95f, COLOR_OK, "CROSS: Reboot");
	gui_textf(500.0f, 330.0f, 0.95f, COLOR_WARN, "CIRCLE: Cancel");
}

static void draw_reset_modal(void) {
	vita2d_draw_rectangle(0.0f, 0.0f, SCREEN_W, SCREEN_H, RGBA8(0x00, 0x00, 0x00, 0x90));
	gui_panel(180.0f, 162.0f, 600.0f, 220.0f, COLOR_PANEL_ALT, COLOR_WARN);
	gui_textf(220.0f, 216.0f, 1.05f, COLOR_WARN, "Clear all stats and uninstall plugin?");
	gui_textf(220.0f, 252.0f, 0.82f, COLOR_TEXT, "This removes BatteryConsumption data/logs and");
	gui_textf(220.0f, 278.0f, 0.82f, COLOR_TEXT, "deletes plugin entries from ur0:/ux0: tai config.");
	gui_textf(220.0f, 304.0f, 0.82f, COLOR_TEXT, "App stays installed. Reboot needed after confirm.");
	gui_textf(220.0f, 352.0f, 0.95f, COLOR_OK, "CROSS: Confirm reset");
	gui_textf(520.0f, 352.0f, 0.95f, COLOR_WARN, "CIRCLE: Cancel");
}

static void draw_dashboard(
	const BatterySnapshot *s,
	const char *status,
	int kernel_enabled,
	const SampleRow *samples,
	int sample_count,
	const AppAgg *aggs,
	int agg_count,
	const ScreenAgg *screen,
	const ActiveKernelState *active_state,
	DashboardTab tab,
	int reboot_confirm,
	int reset_confirm
) {
	char now[40];
	format_tick_local(&s->tick_utc, now, sizeof(now));
	vita2d_set_clear_color(COLOR_BG);
	vita2d_clear_screen();
	draw_header(tab, now);
	if (tab == DASH_TAB_SUMMARY) {
		draw_summary_tab(s, screen, aggs, agg_count, kernel_enabled, status, samples, sample_count);
	} else {
		draw_stats_tab(s, samples, sample_count, aggs, agg_count, screen, active_state, status);
	}
	if (reboot_confirm) {
		draw_reboot_modal();
	} else if (reset_confirm) {
		draw_reset_modal();
	}
}

int main(int argc, char *argv[]) {
	SceCtrlData pad, prev;
	BatterySnapshot snapshot;
	SampleRow recent_samples[MAX_RECENT_SAMPLES];
	AppAgg aggregates[MAX_AGG];
	ScreenAgg screen_agg;
	ActiveKernelState active_state;
	RuntimeCache cache;
	char status[MAX_STATUS];
	SceUInt64 last_draw_us = 0;
	int recent_count = 0;
	int agg_count = 0;
	int running = 1;
	int kernel_enabled = 0;
	int reboot_confirm = 0;
	int reset_confirm = 0;
	DashboardTab active_tab = DASH_TAB_SUMMARY;
	int dirty = 1;

	(void)argc;
	(void)argv;

	memset(&pad, 0, sizeof(pad));
	memset(&prev, 0, sizeof(prev));
	memset(&snapshot, 0, sizeof(snapshot));
	memset(&screen_agg, 0, sizeof(screen_agg));
	memset(&active_state, 0, sizeof(active_state));
	memset(&cache, 0, sizeof(cache));
	memset(status, 0, sizeof(status));
	copy_text(status, sizeof(status), "Ready");

	if (vita2d_init() < 0) {
		app_logf("vita2d_init failed");
		sceKernelExitProcess(1);
		return 1;
	}
	g_font = vita2d_load_default_pgf();
	if (!g_font) {
		app_logf("vita2d_load_default_pgf failed");
		vita2d_fini();
		sceKernelExitProcess(1);
		return 1;
	}
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	if (init_net_status() < 0) {
		app_logf("net status init failed");
	}

	ensure_sessions_file();
	if (start_stats_worker() < 0) {
		copy_text(status, sizeof(status), "Worker start failed");
		app_logf("worker start failed");
	}
	sceKernelDelayThread(200000);
	if (copy_cache(&cache) >= 0) {
		snapshot = cache.snapshot;
		recent_count = cache.recent_count;
		memcpy(recent_samples, cache.recent_samples, sizeof(recent_samples));
		agg_count = cache.agg_count;
		memcpy(aggregates, cache.aggregates, sizeof(aggregates));
		screen_agg = cache.screen_agg;
		active_state = cache.active_state;
		kernel_enabled = cache.kernel_enabled;
	}
	app_logf("app start: kernel_enabled=%d", kernel_enabled);
	{
		int need_update = plugins_need_update();
		if (!kernel_enabled || need_update > 0) {
			int auto_install = install_plugins();
			request_worker_refresh();
			if (copy_cache(&cache) >= 0) {
				kernel_enabled = cache.kernel_enabled;
			}
			if (auto_install == 0 && kernel_enabled) {
				copy_text(status, sizeof(status), "Plugins synced from VPK. Reboot required.");
				app_logf("auto plugin sync success; reboot required");
			} else if (auto_install != 0) {
				snprintf(status, sizeof(status), "Auto plugin sync failed: %d", auto_install);
				app_logf("auto plugin sync failed: %d", auto_install);
			}
		} else if (need_update < 0) {
			snprintf(status, sizeof(status), "Plugin payload missing in VPK: %d", need_update);
			app_logf("plugin payload check failed: %d", need_update);
		}
	}
	while (running) {
		SceUInt64 now_us = sceKernelGetProcessTimeWide();
		unsigned int pressed;

		sceCtrlPeekBufferPositive(0, &pad, 1);
		pressed = pad.buttons & ~prev.buttons;

		if (reboot_confirm || reset_confirm) {
			if (pressed & SCE_CTRL_CROSS) {
				if (reboot_confirm) {
					app_logf("reboot confirmed");
					copy_text(status, sizeof(status), "Rebooting...");
					dirty = 1;
					scePowerRequestColdReset();
				} else {
					int rret = reset_stats_and_uninstall_plugins();
					reset_confirm = 0;
					kernel_enabled = 0;
					if (rret == 0) {
						copy_text(status, sizeof(status), "Stats cleared, plugin removed. Reboot required.");
						app_logf("factory reset completed; reboot required");
					} else {
						snprintf(status, sizeof(status), "Reset completed with warnings: %d", rret);
						app_logf("factory reset warning: %d", rret);
					}
					request_worker_refresh();
					dirty = 1;
				}
			} else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {
				reboot_confirm = 0;
				reset_confirm = 0;
				copy_text(status, sizeof(status), "Action canceled");
				app_logf("modal canceled");
				dirty = 1;
			}
		} else {
			if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
				(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START) &&
				(pressed & SCE_CTRL_START)) {
				reset_confirm = 1;
				copy_text(status, sizeof(status), "Reset confirm: CROSS yes, CIRCLE no");
				app_logf("reset confirm opened");
				dirty = 1;
			} else if (pressed & SCE_CTRL_START) {
				app_logf("app stop requested");
				running = 0;
			}
			if (pressed & SCE_CTRL_SELECT) {
				reboot_confirm = 1;
				copy_text(status, sizeof(status), "Reboot confirm: CROSS yes, CIRCLE no");
				app_logf("reboot confirm opened");
				dirty = 1;
			}
			if (pressed & SCE_CTRL_LTRIGGER) {
				active_tab = DASH_TAB_SUMMARY;
				dirty = 1;
			}
			if (pressed & SCE_CTRL_RTRIGGER) {
				active_tab = DASH_TAB_STATS;
				dirty = 1;
			}
			if (pressed & SCE_CTRL_CROSS) {
				collect_snapshot(&snapshot);
				if (append_sample(&snapshot) >= 0) {
					request_worker_refresh();
					copy_text(status, sizeof(status), "Sample appended");
					app_logf("manual sample appended: pct=%d mAh=%d", snapshot.percent, snapshot.remain_mah);
				} else {
					copy_text(status, sizeof(status), "Sample append failed");
					app_logf("manual sample append failed");
				}
				dirty = 1;
			}
			if (pressed & SCE_CTRL_TRIANGLE) {
				request_worker_refresh();
				copy_text(status, sizeof(status), "Reload done");
				app_logf("reload requested");
				dirty = 1;
			}
			if (pressed & SCE_CTRL_SQUARE) {
				int iret = install_plugins();
				request_worker_refresh();
				if (iret == 0) {
					copy_text(status, sizeof(status), "Plugins synced from VPK. Reboot required.");
					app_logf("manual plugin sync success; reboot required");
				} else {
					snprintf(status, sizeof(status), "Plugin sync failed: %d", iret);
					app_logf("manual plugin sync failed: %d", iret);
				}
				dirty = 1;
			}
		}

		if (copy_cache(&cache) >= 0) {
			snapshot = cache.snapshot;
			recent_count = cache.recent_count;
			memcpy(recent_samples, cache.recent_samples, sizeof(recent_samples));
			agg_count = cache.agg_count;
			memcpy(aggregates, cache.aggregates, sizeof(aggregates));
			screen_agg = cache.screen_agg;
			active_state = cache.active_state;
			kernel_enabled = cache.kernel_enabled;
		}

		if (now_us - last_draw_us >= DRAW_INTERVAL_US) {
			vita2d_start_drawing();
			draw_dashboard(
				&snapshot,
				status,
				kernel_enabled,
				recent_samples,
				recent_count,
				aggregates,
				agg_count,
				&screen_agg,
				&active_state,
				active_tab,
				reboot_confirm,
				reset_confirm);
			vita2d_end_drawing();
			vita2d_swap_buffers();
			last_draw_us = now_us;
			dirty = 0;
		}

		prev = pad;
		sceKernelDelayThread(UI_LOOP_SLEEP_US);
	}

	if (g_font) {
		vita2d_free_pgf(g_font);
		g_font = NULL;
	}
	stop_stats_worker();
	shutdown_net_status();
	vita2d_fini();
	app_logf("app stopped");
	sceKernelExitProcess(0);
	return 0;
}
