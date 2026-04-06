#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>

#include "debugScreen.h"

#define printf psvDebugScreenPrintf

#define APPS_PATH "ux0:data/BatteryConsumption_apps.txt"
#define SAMPLES_PATH "ux0:data/BatteryConsumption_samples.csv"
#define SESSIONS_PATH "ux0:data/BatteryConsumption_sessions.csv"
#define SCREEN_PATH "ux0:data/BatteryConsumption_screen.csv"
#define PENDING_PATH "ux0:data/BatteryConsumption_pending.txt"
#define KERNEL_STATE_PATH "ux0:data/BatteryConsumption_kernel_state.txt"
#define APP_LOG_DIR "ux0:logs/BatteryConsumption"
#define APP_LOG_PATH "ux0:logs/BatteryConsumption/BatteryConsumptionApp.log"
#define APP_PLUGIN_SKPRX_PATH "app0:plugin/BatteryConsumptionKernel.skprx"
#define APP_PLUGIN_SUPRX_PATH "app0:plugin/BatteryConsumptionKernel.suprx"
#define UR0_PLUGIN_SKPRX_PATH "ur0:tai/BatteryConsumptionKernel.skprx"
#define UR0_PLUGIN_SUPRX_PATH "ur0:tai/BatteryConsumptionKernel.suprx"
#define UR0_OLD_PLUGIN_SKPRX_PATH "ur0:tai/batteryconsumption_kernel.skprx"
#define UR0_OLD_PLUGIN_SUPRX_PATH "ur0:tai/batteryconsumption_kernel.suprx"
#define UR0_CONFIG_PATH "ur0:tai/config.txt"

#define MAX_APPS 48
#define MAX_APP_NAME 64
#define MAX_STATUS 128
#define MAX_RECENT_SAMPLES 24
#define MAX_AGG 64
#define SAMPLE_INTERVAL_US 300000000ULL
#define DRAW_INTERVAL_US 1500000ULL
#define STATS_POLL_US 3000000ULL
#define HEAVY_REFRESH_US 30000000ULL
#define UI_LOOP_SLEEP_US 200000ULL
#define UI_LINE_WIDTH 74

typedef struct BatterySnapshot {
	SceRtcTick tick_utc;
	int percent;
	int life_min;
	int charging;
	int online;
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

static int ensure_kernel_config_line(void) {
	char cfg[32768];
	const char *line = "ur0:tai/BatteryConsumptionKernel.skprx";
	char out[33792];
	int n = read_text_sce(UR0_CONFIG_PATH, cfg, sizeof(cfg));
	int has_kernel = 0;
	int inserted = 0;
	char *p;

	if (n < 0) {
		snprintf(out, sizeof(out), "*KERNEL\n%s\n", line);
		return write_text_sce(UR0_CONFIG_PATH, out);
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

	return write_text_sce(UR0_CONFIG_PATH, out);
}

static int kernel_plugin_is_enabled(void) {
	char cfg[32768];
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
	remove_file_if_exists(UR0_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UR0_PLUGIN_SUPRX_PATH);
	remove_file_if_exists(UR0_OLD_PLUGIN_SKPRX_PATH);
	remove_file_if_exists(UR0_OLD_PLUGIN_SUPRX_PATH);

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

	ret_cfg = ensure_kernel_config_line();
	if (ret_cfg < 0) {
		app_logf("plugin install failed: config update ret=%d", ret_cfg);
		return -30 + ret_cfg;
	}
	app_logf("plugin install ok: %s and %s", UR0_PLUGIN_SKPRX_PATH, UR0_PLUGIN_SUPRX_PATH);
	return 0;
}

static int append_sample(const BatterySnapshot *s) {
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

static int load_recent_samples(SampleRow *rows, int max_rows) {
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

static int load_screen_aggregate(ScreenAgg *screen) {
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

static int load_aggregates(AppAgg *aggs, int max_aggs) {
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
		if (!app[0] || !is_titleid_string(app)) {
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

static void print_line_fixed(const char *fmt, ...) {
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%-*.*s\n", UI_LINE_WIDTH, UI_LINE_WIDTH, line);
}

static void draw_history_ascii(const SampleRow *samples, int sample_count) {
	int i;
	int start = (sample_count > 6) ? (sample_count - 6) : 0;
	int shown = 0;
	print_line_fixed("+---------------------+-----+------+-----+-----+");
	print_line_fixed("| Time                | pct | mAh  | mV  | chg |");
	print_line_fixed("+---------------------+-----+------+-----+-----+");
	for (i = start; i < sample_count; i++) {
		char ts[40];
		format_tick_local(&samples[i].tick_utc, ts, sizeof(ts));
		print_line_fixed("| %-19.19s | %3d | %4d |%4d |  %c  |",
			ts, samples[i].percent, samples[i].remain_mah, samples[i].mv, samples[i].charging ? 'Y' : 'N');
		shown++;
	}
	while (shown < 6) {
		print_line_fixed("| %-19.19s | %3s | %4s |%4s |  %c  |", "-", "-", "-", "-", '-');
		shown++;
	}
	print_line_fixed("+---------------------+-----+------+-----+-----+");
}

static void draw_agg_ascii(const AppAgg *aggs, int agg_count) {
	int i;
	print_line_fixed("+----------------------+--------+------+-----+---------+");
	print_line_fixed("| App                  | mAh    | %%    | ses | mAh/h   |");
	print_line_fixed("+----------------------+--------+------+-----+---------+");
	for (i = 0; i < 5; i++) {
		if (i < agg_count) {
			double rate = 0.0;
			if (aggs[i].minutes > 0.01) {
				rate = aggs[i].consumed_mah * 60.0 / aggs[i].minutes;
			}
			print_line_fixed("| %-20.20s | %6.1f | %4.1f | %3d | %7.1f |",
				aggs[i].app, aggs[i].consumed_mah, aggs[i].consumed_pct, aggs[i].sessions, rate);
		} else {
			print_line_fixed("| %-20.20s | %6s | %4s | %3s | %7s |", "-", "-", "-", "-", "-");
		}
	}
	print_line_fixed("+----------------------+--------+------+-----+---------+");
}

static void draw_dashboard(
	const BatterySnapshot *s,
	const PendingSession *pending,
	const char *selected_app,
	const char *status,
	int discover_ret,
	int kernel_enabled,
	const SampleRow *samples,
	int sample_count,
	const AppAgg *aggs,
	int agg_count,
	const ScreenAgg *screen
) {
	char now[40];
	char start[40];
	char t0[40];
	char t1[40];
	int d_pct = 0;
	int d_mah = 0;

	format_tick_local(&s->tick_utc, now, sizeof(now));
	print_line_fixed("BatteryConsumption v0.3  |  one-screen dashboard");
	print_line_fixed("Now: %s", now);
	print_line_fixed("Battery: %d%%  %d/%d mAh  %d mV  | chg=%s online=%s low=%s",
		s->percent, s->remain_mah, s->full_mah, s->mv,
		s->charging ? "Y" : "N", s->online ? "Y" : "N", s->low ? "Y" : "N");
	print_line_fixed("Health: SOH=%d%% cycles=%d temp=%d.%02dC life=%dmin",
		s->soh, s->cycle_count, s->temp_centi / 100, abs(s->temp_centi % 100), s->life_min);
	print_line_fixed("Clocks: ARM=%d BUS=%d GPU=%d XBAR=%d MHz", s->arm_mhz, s->bus_mhz, s->gpu_mhz, s->xbar_mhz);
	print_line_fixed("Selected app: %-20.20s | Kernel plugin: %s | shell detect: %s",
		selected_app ? selected_app : "(none)",
		kernel_enabled ? "ON" : "OFF",
		(discover_ret < 0) ? "NO" : "YES");

	if (pending && pending->active) {
		format_tick_local(&pending->start_tick_utc, start, sizeof(start));
		print_line_fixed("Pending: ACTIVE app=%-16.16s from=%s  dNow=%d%%/%dmAh",
			pending->app, start, pending->start_percent - s->percent, pending->start_mah - s->remain_mah);
	} else {
		print_line_fixed("Pending: none");
	}

	if (sample_count > 1) {
		format_tick_local(&samples[0].tick_utc, t0, sizeof(t0));
		format_tick_local(&samples[sample_count - 1].tick_utc, t1, sizeof(t1));
		d_pct = samples[0].percent - samples[sample_count - 1].percent;
		d_mah = samples[0].remain_mah - samples[sample_count - 1].remain_mah;
		print_line_fixed("History window: %s -> %s   drop=%d%%/%dmAh (%d rows)", t0, t1, d_pct, d_mah, sample_count);
	} else {
		print_line_fixed("History window: not enough data (%d row)", sample_count);
	}
	if (screen) {
		print_line_fixed("Screen active: %.1f min | %.1f mAh | %.1f%% | slices=%d",
			screen->minutes, screen->consumed_mah, screen->consumed_pct, screen->sessions);
	}

	draw_history_ascii(samples, sample_count);
	print_line_fixed("Top apps by total consumption:");
	draw_agg_ascii(aggs, agg_count);
	print_line_fixed("Status: %s", status);
	print_line_fixed("Controls: UP/DOWN app | CROSS sample | SQUARE arm | CIRCLE close | TRIANGLE reload | R sync plugins | SELECT reboot | START exit");
}

int main(int argc, char *argv[]) {
	SceCtrlData pad, prev;
	BatterySnapshot snapshot;
	PendingSession pending;
	SampleRow recent_samples[MAX_RECENT_SAMPLES];
	AppAgg aggregates[MAX_AGG];
	ScreenAgg screen_agg;
	char apps[MAX_APPS][MAX_APP_NAME];
	char status[MAX_STATUS];
	SceUInt64 last_sample_us = 0;
	SceUInt64 last_draw_us = 0;
	SceUInt64 last_stats_poll_us = 0;
	SceUInt64 last_heavy_refresh_us = 0;
	int recent_count = 0;
	int agg_count = 0;
	int app_count = 0;
	int selected_app = 0;
	int running = 1;
	int discover_ret = -1;
	int kernel_enabled = 0;
	int reboot_confirm = 0;
	int dirty = 1;

	(void)argc;
	(void)argv;

	memset(&pad, 0, sizeof(pad));
	memset(&prev, 0, sizeof(prev));
	memset(&snapshot, 0, sizeof(snapshot));
	memset(&pending, 0, sizeof(pending));
	memset(&screen_agg, 0, sizeof(screen_agg));
	memset(status, 0, sizeof(status));
	copy_text(status, sizeof(status), "Ready");

	psvDebugScreenInit();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

	app_count = load_apps(apps);
	discover_ret = try_discover_titles_for_shell(apps, &app_count);
	load_pending(&pending);
	collect_snapshot(&snapshot);
	append_sample(&snapshot);
	last_sample_us = sceKernelGetProcessTimeWide();
	last_stats_poll_us = last_sample_us;
	recent_count = load_recent_samples(recent_samples, MAX_RECENT_SAMPLES);
	ensure_sessions_file();
	agg_count = load_aggregates(aggregates, MAX_AGG);
	load_screen_aggregate(&screen_agg);
	kernel_enabled = kernel_plugin_is_enabled();
	last_heavy_refresh_us = last_sample_us;
	app_logf("app start: apps=%d discover_ret=0x%08X kernel_enabled=%d", app_count, discover_ret, kernel_enabled);
	{
		int need_update = plugins_need_update();
		if (!kernel_enabled || need_update > 0) {
			int auto_install = install_plugins();
			kernel_enabled = kernel_plugin_is_enabled();
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
	printf("\e[0m\e[37;40m\e[2J\e[H");

	while (running) {
		SceUInt64 now_us = sceKernelGetProcessTimeWide();
		unsigned int pressed;

		sceCtrlPeekBufferPositive(0, &pad, 1);
		pressed = pad.buttons & ~prev.buttons;

		if (reboot_confirm) {
			if (pressed & SCE_CTRL_CROSS) {
				app_logf("reboot confirmed");
				copy_text(status, sizeof(status), "Rebooting...");
				dirty = 1;
				scePowerRequestColdReset();
			} else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) {
				reboot_confirm = 0;
				copy_text(status, sizeof(status), "Reboot canceled");
				app_logf("reboot canceled");
				dirty = 1;
			}
			prev = pad;
			sceKernelDelayThread(UI_LOOP_SLEEP_US);
			continue;
		}

		if (pressed & SCE_CTRL_START) {
			app_logf("app stop requested");
			running = 0;
		}
		if (pressed & SCE_CTRL_SELECT) {
			reboot_confirm = 1;
			copy_text(status, sizeof(status), "Reboot confirm: CROSS yes, CIRCLE no");
			app_logf("reboot confirm opened");
			dirty = 1;
		}
		if (pressed & SCE_CTRL_UP) {
			if (selected_app > 0) {
				selected_app--;
				dirty = 1;
			}
		}
		if (pressed & SCE_CTRL_DOWN) {
			if (selected_app < app_count - 1) {
				selected_app++;
				dirty = 1;
			}
		}
		if (pressed & SCE_CTRL_CROSS) {
			collect_snapshot(&snapshot);
			if (append_sample(&snapshot) >= 0) {
				recent_count = load_recent_samples(recent_samples, MAX_RECENT_SAMPLES);
				copy_text(status, sizeof(status), "Sample appended");
				app_logf("manual sample appended: pct=%d mAh=%d", snapshot.percent, snapshot.remain_mah);
			} else {
				copy_text(status, sizeof(status), "Sample append failed");
				app_logf("manual sample append failed");
			}
			dirty = 1;
		}
		if (pressed & SCE_CTRL_SQUARE) {
			if (pending.active) {
				copy_text(status, sizeof(status), "Pending already active");
			} else {
				collect_snapshot(&snapshot);
				memset(&pending, 0, sizeof(pending));
				pending.active = 1;
				copy_text(pending.app, sizeof(pending.app), apps[selected_app]);
				pending.start_tick_utc = snapshot.tick_utc;
				pending.start_percent = snapshot.percent;
				pending.start_mah = snapshot.remain_mah;
				pending.start_full_mah = snapshot.full_mah;
				pending.start_mv = snapshot.mv;
				if (save_pending(&pending) >= 0) {
					copy_text(status, sizeof(status), "Session armed. Exit and play.");
					app_logf("pending armed: app=%s start_pct=%d start_mah=%d", pending.app, pending.start_percent, pending.start_mah);
				} else {
					copy_text(status, sizeof(status), "Failed to save pending");
					app_logf("pending arm failed: app=%s", pending.app);
				}
			}
			dirty = 1;
		}
		if (pressed & SCE_CTRL_CIRCLE) {
			if (!pending.active) {
				copy_text(status, sizeof(status), "No pending session");
			} else {
				int dmah = 0;
				int dpct = 0;
				collect_snapshot(&snapshot);
				if (append_session(&pending, &snapshot, &dmah, &dpct) >= 0) {
					clear_pending(&pending);
					agg_count = load_aggregates(aggregates, MAX_AGG);
					snprintf(status, sizeof(status), "Session closed: d=%d mAh, %d%%", dmah, dpct);
					app_logf("pending closed: dmah=%d dpct=%d", dmah, dpct);
				} else {
					copy_text(status, sizeof(status), "Failed to append session");
					app_logf("pending close failed");
				}
			}
			dirty = 1;
		}
		if (pressed & SCE_CTRL_TRIANGLE) {
			app_count = load_apps(apps);
			if (selected_app >= app_count) {
				selected_app = app_count - 1;
				if (selected_app < 0) {
					selected_app = 0;
				}
			}
			discover_ret = try_discover_titles_for_shell(apps, &app_count);
			load_pending(&pending);
			recent_count = load_recent_samples(recent_samples, MAX_RECENT_SAMPLES);
			agg_count = load_aggregates(aggregates, MAX_AGG);
			load_screen_aggregate(&screen_agg);
			kernel_enabled = kernel_plugin_is_enabled();
			copy_text(status, sizeof(status), "Reload done");
			app_logf("reload: apps=%d discover_ret=0x%08X kernel_enabled=%d", app_count, discover_ret, kernel_enabled);
			dirty = 1;
		}
		if (pressed & SCE_CTRL_RTRIGGER) {
			int iret = install_plugins();
			kernel_enabled = kernel_plugin_is_enabled();
			if (iret == 0) {
				copy_text(status, sizeof(status), "Plugins synced from VPK. Reboot required.");
				app_logf("manual plugin sync success; reboot required");
			} else {
				snprintf(status, sizeof(status), "Plugin sync failed: %d", iret);
				app_logf("manual plugin sync failed: %d", iret);
			}
			dirty = 1;
		}

		if (now_us - last_sample_us >= SAMPLE_INTERVAL_US) {
			collect_snapshot(&snapshot);
			append_sample(&snapshot);
			recent_count = load_recent_samples(recent_samples, MAX_RECENT_SAMPLES);
			last_sample_us = now_us;
			app_logf("periodic sample: pct=%d mAh=%d", snapshot.percent, snapshot.remain_mah);
			dirty = 1;
		}

		if (now_us - last_stats_poll_us >= STATS_POLL_US) {
			collect_snapshot(&snapshot);
			last_stats_poll_us = now_us;
			dirty = 1;
		}

		if (now_us - last_heavy_refresh_us >= HEAVY_REFRESH_US) {
			load_pending(&pending);
			recent_count = load_recent_samples(recent_samples, MAX_RECENT_SAMPLES);
			agg_count = load_aggregates(aggregates, MAX_AGG);
			load_screen_aggregate(&screen_agg);
			kernel_enabled = kernel_plugin_is_enabled();
			last_heavy_refresh_us = now_us;
			dirty = 1;
		}

		if (dirty && now_us - last_draw_us >= DRAW_INTERVAL_US) {
			printf("\e[H");
			draw_dashboard(
				&snapshot,
				&pending,
				apps[selected_app],
				status,
				discover_ret,
				kernel_enabled,
				recent_samples,
				recent_count,
				aggregates,
				agg_count,
				&screen_agg);
			last_draw_us = now_us;
			dirty = 0;
		}

		prev = pad;
		sceKernelDelayThread(UI_LOOP_SLEEP_US);
	}

	psvDebugScreenFinish();
	app_logf("app stopped");
	sceKernelExitProcess(0);
	return 0;
}
