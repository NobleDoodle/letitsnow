/*
 * letitsnowd -- falling snow on a Hyprland desktop.
 *
 * One wlr-layer-shell surface per output, drawn with GLES2 point sprites and
 * clicked straight through. The flakes are the snow from the Joyful theme's
 * preview reel, re-expressed per frame rather than approximated: the same
 * size, fall speed, drift, sway and alpha distributions, the same Gaussian
 * sprite with the same 12 alpha levels and 8-bit truncation, and the same
 * whole-pixel placement. 240 flakes per 1920x1080 at density 1.
 *
 * Three modes: desktop (on the background layer, behind everything), top (on
 * the overlay layer, above everything) and dynamic: desktop, plus a second
 * surface just above the windows -- the veil -- that draws the same flakes at
 * a tenth of the brightness. Windows hide the full snow behind them, so over a
 * window only the veil shows; on the bare desktop the two composite to exactly
 * the full snow (the background layer is dimmed by what the veil adds).
 *
 * Settings live in $XDG_CONFIG_HOME/letitsnow/config (key=value) and apply as
 * soon as the file changes. `letitsnow` is the command that edits it.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef LETITSNOW_VERSION
#define LETITSNOW_VERSION "dev"
#endif

#define NAMESPACE "letitsnow"
#define NAMESPACE_VEIL "letitsnow-veil"
/* Dynamic mode: the snow over windows, as a share of the desktop brightness. */
#define DYNAMIC_VEIL 0.10
#define EX_UNAVAILABLE 69

/* ------------------------------------------------------------- the look */

/* The reel snowed 240 flakes over a 1920x1080 frame. */
#define REF_FLAKES 240.0
#define REF_AREA (1920.0 * 1080.0)
#define ALPHA_LEVELS 12
static const float FLAKE_RGB[3] = {250.f / 255.f, 248.f / 255.f, 240.f / 255.f};

struct flake {
	double ux, uy;      /* start position, as a fraction of the wrap box */
	double s;           /* size parameter */
	double v;           /* fall speed, px/s */
	double amp, f, ph;  /* sideways sway */
	double a;           /* alpha before intensity */
	float r;            /* Gaussian radius, px */
	float coef;         /* Gaussian exponent factor */
	float half;         /* sprite half-extent, px */
};

/* ------------------------------------------------------------- settings */

enum layer_mode { MODE_DESKTOP, MODE_TOP, MODE_DYNAMIC };

/* Desktop and dynamic both put the snow itself on the background layer. */
static bool on_background(enum layer_mode m)
{
	return m != MODE_TOP;
}

static const char *layer_name(enum layer_mode m)
{
	return m == MODE_TOP ? "top" : m == MODE_DYNAMIC ? "dynamic" : "desktop";
}

struct settings {
	double speed;
	double density;
	double opacity;      /* the reel's intensity: 1 on its title cards */
	enum layer_mode layer;
	int fps;
};

#define SPEED_MIN 0.1
#define SPEED_MAX 5.0
#define DENSITY_MIN 0.1
#define DENSITY_MAX 5.0
#define OPACITY_MIN 0.05
#define OPACITY_MAX 1.0
#define FPS_MIN 10
#define FPS_MAX 240

static const struct settings DEFAULTS = {1.0, 1.0, 1.0, MODE_DESKTOP, 60};

/* ------------------------------------------------------------- state */

struct snow;

struct output {
	struct wl_list link;
	uint32_t global;
	struct wl_output *wl;
	char *name;        /* connector name (wl_output v4), e.g. eDP-1 */
	int32_t int_scale;
	bool done;
	struct snow *snow;    /* the snow itself */
	struct snow *veil;    /* dynamic mode: the faint copy above the windows */
	double restack_at[8]; /* recent restack times, for the rate limit */
	int restack_i;
	bool restack_gave_up;
	bool covered;       /* desktop or dynamic, fullscreen window on top: paused */
	int reserved[4];    /* left, top, right, bottom kept for bars (Hyprland), logical px */
	/* The flakes belong to the output, so its two surfaces draw the same ones. */
	uint64_t rng;
	bool seeded;
	struct flake *flakes;
	size_t nflakes, cap;
};

struct snow {
	struct output *out;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *frac;
	struct wl_egl_window *egl_win;
	EGLSurface egl_surf;
	struct wl_callback *frame_cb;
	int width, height;   /* logical */
	int buf_w, buf_h;    /* buffer pixels */
	double frac_scale;   /* preferred fractional scale, 0 until told */
	enum layer_mode mode;
	bool configured;
	bool ready;          /* may draw: configured and no frame in flight */
	bool interval_set;
	bool clear_pending;  /* paused: draw one empty frame first */
	bool veil;           /* this is the output's veil */
	uint64_t frames;
	bool dumped;
};

static struct {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct zwlr_layer_shell_v1 *layer_shell;
	uint32_t layer_shell_version;
	struct wp_fractional_scale_manager_v1 *frac_mgr;
	struct wp_viewporter *viewporter;
	struct wl_list outputs;
	char wl_socket[PATH_MAX]; /* socket name we connected to */

	EGLDisplay egl_dpy;
	EGLConfig egl_cfg;
	EGLContext egl_ctx;
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window;
	bool gl_ready;
	GLuint prog, vbo;
	GLint loc_flake, loc_shape, loc_buf, loc_scale, loc_rgb, loc_mode, loc_frac;
	float point_max;
	float *verts;
	size_t vcap;

	struct settings cfg;
	char cfg_dir[PATH_MAX];
	char cfg_path[PATH_MAX];

	int sig_fd, ino_fd, timer_fd, ipc_fd, hypr_fd;
	bool want_restack, want_fullscreen;
	bool fullscreen_recheck; /* look again once animations have settled */
	bool timer_armed;
	uint64_t last_frame_ns, last_tick_ns;
	double tau;          /* snow time: real seconds scaled by speed */
	double freeze_tau;   /* LETITSNOW_FREEZE: test hook, <0 when unset */
	const char *dump_path;

	char hypr_dir[PATH_MAX];
	char hypr_buf[16384];
	size_t hypr_len;

	bool verbose;
	bool running;
	int exit_code;
	uint64_t stats_ns;
} app;

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("letitsnow: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* ------------------------------------------------------------- random */

static uint64_t splitmix64(uint64_t *x)
{
	uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

static double rnd(uint64_t *x)
{
	return (double)(splitmix64(x) >> 11) * 0x1.0p-53;
}

static uint64_t seed_for(const char *name)
{
	uint64_t h = 1469598103934665603ull; /* FNV-1a, so each output keeps its own pattern */
	for (const char *p = name ? name : ""; *p; p++)
		h = (h ^ (unsigned char)*p) * 1099511628211ull;
	return h ^ 11;
}

/* The reel's Snow.__init__, one flake at a time. */
static struct flake make_flake(uint64_t *rng)
{
	struct flake fl;
	double u = rnd(rng);
	fl.s = 1.1 + 5.2 * pow(u, 2.3);
	fl.ux = rnd(rng);
	fl.uy = rnd(rng);
	fl.v = 32 + 20 * fl.s;
	fl.amp = 8 + 28 * rnd(rng);
	fl.f = 0.12 + 0.35 * rnd(rng);
	fl.ph = rnd(rng) * 6.28;
	fl.a = (0.5 + 0.45 * rnd(rng)) * (fl.s > 4.5 ? 0.7 : 1.0);
	double bucket = nearbyint(fl.s * 2); /* half-to-even, as numpy rounds */
	fl.r = (float)(bucket / 2);
	fl.coef = fl.r < 4.5f ? 1.4f : 0.9f;
	fl.half = ceilf(fl.r * 3);
	return fl;
}

/* ------------------------------------------------------------- settings io */

static bool parse_layer(const char *v, enum layer_mode *out)
{
	if (!strcmp(v, "desktop") || !strcmp(v, "background") || !strcmp(v, "bottom")) {
		*out = MODE_DESKTOP;
		return true;
	}
	if (!strcmp(v, "top") || !strcmp(v, "overlay")) {
		*out = MODE_TOP;
		return true;
	}
	if (!strcmp(v, "dynamic")) {
		*out = MODE_DYNAMIC;
		return true;
	}
	return false;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static struct settings read_settings(void)
{
	struct settings c = DEFAULTS;
	FILE *f = fopen(app.cfg_path, "r");
	if (!f)
		return c;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line);
		if (!*l || *l == '#')
			continue;
		char *eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = trim(l), *v = trim(eq + 1), *end;
		if (!strcmp(k, "speed")) {
			double d = strtod(v, &end);
			if (end != v && isfinite(d))
				c.speed = clampd(d, SPEED_MIN, SPEED_MAX);
		} else if (!strcmp(k, "density")) {
			double d = strtod(v, &end);
			if (end != v && isfinite(d))
				c.density = clampd(d, DENSITY_MIN, DENSITY_MAX);
		} else if (!strcmp(k, "opacity")) {
			double d = strtod(v, &end);
			if (end != v && isfinite(d))
				c.opacity = clampd(d, OPACITY_MIN, OPACITY_MAX);
		} else if (!strcmp(k, "layer")) {
			if (!parse_layer(v, &c.layer))
				logmsg("ignoring layer=%s (desktop, dynamic or top)", v);
		} else if (!strcmp(k, "fps")) {
			long n = strtol(v, &end, 10);
			if (end != v)
				c.fps = (int)clampd((double)n, FPS_MIN, FPS_MAX);
		}
	}
	fclose(f);
	return c;
}

static void config_paths(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg == '/')
		snprintf(app.cfg_dir, sizeof app.cfg_dir, "%s/letitsnow", xdg);
	else
		snprintf(app.cfg_dir, sizeof app.cfg_dir, "%s/.config/letitsnow", home ? home : "");
	snprintf(app.cfg_path, sizeof app.cfg_path, "%s/config", app.cfg_dir);
}

/* ------------------------------------------------------------- hyprland ipc */

static bool read_lock(const char *dir, pid_t *pid, char *sock, size_t n)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/hyprland.lock", dir);
	FILE *f = fopen(p, "r");
	if (!f)
		return false;
	char l1[64] = "", l2[256] = "";
	bool ok = fgets(l1, sizeof l1, f) && fgets(l2, sizeof l2, f);
	fclose(f);
	if (!ok)
		return false;
	*pid = (pid_t)atol(l1);
	snprintf(sock, n, "%s", trim(l2));
	return *pid > 0 && *sock;
}

static bool alive(pid_t pid)
{
	return kill(pid, 0) == 0 || errno == EPERM;
}

/* The Hyprland instance whose Wayland socket is `want`; with want NULL, the
 * newest live instance. Writes its directory and socket name. */
static bool find_hyprland(const char *want, char *dir, size_t dn, char *sock, size_t sn)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt || !*rt)
		return false;
	char base[PATH_MAX];
	snprintf(base, sizeof base, "%s/hypr", rt);

	const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	if (sig && *sig && !strchr(sig, '/')) {
		char d[PATH_MAX], s[256];
		pid_t pid;
		snprintf(d, sizeof d, "%s/%s", base, sig);
		if (read_lock(d, &pid, s, sizeof s) && alive(pid) && (!want || !strcmp(s, want))) {
			snprintf(dir, dn, "%s", d);
			snprintf(sock, sn, "%s", s);
			return true;
		}
	}

	DIR *dp = opendir(base);
	if (!dp)
		return false;
	struct dirent *e;
	time_t best = 0;
	bool found = false;
	while ((e = readdir(dp))) {
		if (e->d_name[0] == '.')
			continue;
		char d[PATH_MAX], s[256], lp[PATH_MAX];
		pid_t pid;
		snprintf(d, sizeof d, "%s/%s", base, e->d_name);
		if (!read_lock(d, &pid, s, sizeof s) || !alive(pid))
			continue;
		if (want && strcmp(s, want))
			continue;
		struct stat st;
		snprintf(lp, sizeof lp, "%s/hyprland.lock", d);
		if (stat(lp, &st) || (found && st.st_mtime < best))
			continue;
		best = st.st_mtime;
		found = true;
		snprintf(dir, dn, "%s", d);
		snprintf(sock, sn, "%s", s);
	}
	closedir(dp);
	return found;
}

static int hypr_connect(const char *file)
{
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s/%s", app.hypr_dir, file) >= (int)sizeof sa.sun_path)
		return -1;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* One request on Hyprland's command socket; the reply, malloc'd. */
static char *hypr_request(const char *cmd)
{
	if (!app.hypr_dir[0])
		return NULL;
	int fd = hypr_connect(".socket.sock");
	if (fd < 0)
		return NULL;
	struct timeval tv = {.tv_sec = 1};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
		close(fd);
		return NULL;
	}
	size_t len = 0, cap = 16384;
	char *buf = malloc(cap);
	for (;;) {
		if (len + 4096 + 1 > cap) {
			if (cap > (8u << 20))
				break;
			char *nb = realloc(buf, cap *= 2);
			if (!nb)
				break;
			buf = nb;
		}
		ssize_t r = read(fd, buf + len, cap - len - 1);
		if (r <= 0)
			break;
		len += (size_t)r;
	}
	close(fd);
	buf[len] = 0;
	return buf;
}

/* A small JSON reader: enough for Hyprland's replies, nothing more. */
enum jtype { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct jv {
	enum jtype t;
	double num;
	char *str;
	struct jv *items; /* array elements or object values */
	char **keys;      /* object keys */
	size_t n;
};

struct jp {
	const char *p;
	int depth;
};

static void jclear(struct jv *v)
{
	for (size_t i = 0; i < v->n; i++) {
		jclear(&v->items[i]);
		if (v->keys)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->str);
	memset(v, 0, sizeof *v);
}

static void jws(struct jp *j)
{
	while (*j->p && isspace((unsigned char)*j->p))
		j->p++;
}

static int hexval(char c)
{
	return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

static char *jstring(struct jp *j)
{
	size_t cap = 32, n = 0;
	char *out = malloc(cap);
	j->p++; /* the opening quote */
	while (*j->p && *j->p != '"') {
		char c = *j->p++;
		if (c == '\\' && *j->p) {
			char e = *j->p++;
			switch (e) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u': {
				unsigned v = 0;
				for (int i = 0; i < 4 && isxdigit((unsigned char)*j->p); i++)
					v = v * 16 + (unsigned)hexval(*j->p++);
				c = v < 0x80 ? (char)v : '?'; /* only names are compared; ASCII is enough */
				break;
			}
			default: c = e;
			}
		}
		if (n + 2 > cap) {
			char *nb = realloc(out, cap *= 2);
			if (!nb)
				break;
			out = nb;
		}
		out[n++] = c;
	}
	if (*j->p == '"')
		j->p++;
	out[n] = 0;
	return out;
}

static bool jparse(struct jp *j, struct jv *v)
{
	memset(v, 0, sizeof *v);
	jws(j);
	if (++j->depth > 64)
		return false;
	bool ok = true;
	char c = *j->p;
	if (c == '{' || c == '[') {
		bool obj = c == '{';
		char close = obj ? '}' : ']';
		size_t cap = 0;
		v->t = obj ? J_OBJ : J_ARR;
		j->p++;
		jws(j);
		if (*j->p == close) {
			j->p++;
		} else {
			for (;;) {
				char *key = NULL;
				if (obj) {
					jws(j);
					if (*j->p != '"') {
						ok = false;
						break;
					}
					key = jstring(j);
					jws(j);
					if (*j->p != ':') {
						free(key);
						ok = false;
						break;
					}
					j->p++;
				}
				if (v->n == cap) {
					cap = cap ? cap * 2 : 8;
					struct jv *ni = realloc(v->items, cap * sizeof *ni);
					char **nk = obj ? realloc(v->keys, cap * sizeof *nk) : NULL;
					if (ni)
						v->items = ni;
					if (nk)
						v->keys = nk;
					if (!ni || (obj && !nk)) {
						free(key);
						ok = false;
						break;
					}
				}
				if (obj)
					v->keys[v->n] = key;
				bool item_ok = jparse(j, &v->items[v->n]);
				v->n++;
				if (!item_ok) {
					ok = false;
					break;
				}
				jws(j);
				if (*j->p == ',') {
					j->p++;
					continue;
				}
				if (*j->p == close)
					j->p++;
				else
					ok = false;
				break;
			}
		}
	} else if (c == '"') {
		v->t = J_STR;
		v->str = jstring(j);
	} else if (!strncmp(j->p, "true", 4)) {
		v->t = J_BOOL;
		v->num = 1;
		j->p += 4;
	} else if (!strncmp(j->p, "false", 5)) {
		v->t = J_BOOL;
		j->p += 5;
	} else if (!strncmp(j->p, "null", 4)) {
		j->p += 4;
	} else {
		char *end;
		v->t = J_NUM;
		v->num = strtod(j->p, &end);
		if (end == j->p)
			ok = false;
		else
			j->p = end;
	}
	j->depth--;
	return ok;
}

static struct jv *jget(struct jv *o, const char *key)
{
	if (!o || o->t != J_OBJ)
		return NULL;
	for (size_t i = 0; i < o->n; i++)
		if (o->keys[i] && !strcmp(o->keys[i], key))
			return &o->items[i];
	return NULL;
}

static double jnum(struct jv *v, double def)
{
	return v && (v->t == J_NUM || v->t == J_BOOL) ? v->num : def;
}

/* Ask Hyprland (j/layers, j/monitors, ...) and parse the reply. */
static bool hypr_json(const char *cmd, struct jv *out)
{
	char *s = hypr_request(cmd);
	if (!s)
		return false;
	struct jp j = {s, 0};
	bool ok = jparse(&j, out);
	free(s);
	if (!ok)
		jclear(out);
	return ok;
}

static void restack_snow(struct output *o);
static void schedule(void);

/* Desktop mode sits on the background layer above the wallpaper. A wallpaper
 * mapped after it -- a shell restart, a video wallpaper, swaybg relaunched --
 * lands on top and hides the snow, so when one opens, look at the order and
 * re-create the snow surface (which puts it last, on top) where needed. */
static void check_stacking(void)
{
	if (!on_background(app.cfg.layer))
		return;
	struct jv L;
	if (!hypr_json("j/layers", &L))
		return;
	struct output *o;
	wl_list_for_each(o, &app.outputs, link) {
		struct snow *s = o->snow;
		if (!s || !s->configured || !on_background(s->mode) || !o->name)
			continue;
		struct jv *lv0 = jget(jget(jget(&L, o->name), "levels"), "0");
		if (!lv0 || lv0->t != J_ARR)
			continue;
		/* In Hyprland's order, drawn first to last. */
		bool ours_seen = false, other_after = false;
		for (size_t i = 0; i < lv0->n; i++) {
			struct jv *ns = jget(&lv0->items[i], "namespace");
			if (!ns || ns->t != J_STR)
				continue;
			if (!strcmp(ns->str, NAMESPACE)) {
				ours_seen = true;
				other_after = false;
			} else if (ours_seen) {
				other_after = true;
			}
		}
		if (ours_seen && other_after)
			restack_snow(o);
	}
	jclear(&L);
}

/* Behind a fullscreen window nobody sees the desktop, but Hyprland keeps
 * asking for frames, and each one makes it composite the screen again. So in
 * desktop mode an output showing a fullscreen window gets one empty frame and
 * then nothing until the window leaves. */
static void check_fullscreen(void)
{
	struct jv M = {0}, C = {0};
	bool desktop = on_background(app.cfg.layer);
	if (!hypr_json("j/monitors", &M))
		return;
	if (desktop && !hypr_json("j/clients", &C)) {
		jclear(&M);
		return;
	}
	struct output *o;
	wl_list_for_each(o, &app.outputs, link) {
		bool covered = false;
		for (size_t i = 0; o->name && M.t == J_ARR && i < M.n; i++) {
			struct jv *mon = &M.items[i];
			struct jv *name = jget(mon, "name");
			if (!name || name->t != J_STR || strcmp(name->str, o->name))
				continue;
			/* Strips kept for bars ([left, top, right, bottom]): the veil leaves them alone. */
			struct jv *res = jget(mon, "reserved");
			for (int k = 0; k < 4; k++)
				o->reserved[k] = res && res->t == J_ARR && (size_t)k < res->n ? (int)jnum(&res->items[k], 0) : 0;
			if (!desktop)
				continue;
			double ws = jnum(jget(jget(mon, "activeWorkspace"), "id"), 0);
			double sp = jnum(jget(jget(mon, "specialWorkspace"), "id"), 0);
			for (size_t k = 0; C.t == J_ARR && k < C.n; k++) {
				struct jv *c = &C.items[k];
				double wid = jnum(jget(jget(c, "workspace"), "id"), 0);
				int fs = (int)jnum(jget(c, "fullscreen"), 0);
				bool shown = jnum(jget(c, "mapped"), 1) != 0 && jnum(jget(c, "hidden"), 0) == 0;
				if (shown && (fs & 2) && (wid == ws || (sp != 0 && wid == sp)))
					covered = true;
			}
		}
		if (covered == o->covered)
			continue;
		o->covered = covered;
		if (app.verbose)
			logmsg("%s: %s", o->name ? o->name : "?", covered ? "fullscreen window, pausing" : "resuming");
		struct snow *both[2] = {o->snow, o->veil};
		for (int k = 0; k < 2; k++) {
			if (!both[k])
				continue;
			both[k]->clear_pending = covered;
			if (!both[k]->frame_cb && both[k]->configured)
				both[k]->ready = true;
		}
	}
	jclear(&M);
	jclear(&C);
	schedule();
}

static void arm_ipc_check(bool restack, bool fullscreen, int ms)
{
	app.want_restack |= restack;
	app.want_fullscreen |= fullscreen;
	if (app.ipc_fd < 0)
		return;
	struct itimerspec it = {.it_value = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L}};
	timerfd_settime(app.ipc_fd, 0, &it, NULL);
}

static void ipc_checks(void)
{
	bool r = app.want_restack, f = app.want_fullscreen;
	app.want_restack = app.want_fullscreen = false;
	if (f) {
		check_fullscreen();
		/* A window that just closed can still be listed for the length of its
		 * closing animation, so a check right after the event may see it. */
		if (app.fullscreen_recheck) {
			app.fullscreen_recheck = false;
			arm_ipc_check(false, true, 600);
		}
	}
	if (r)
		check_stacking();
}

static bool starts(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

static void hypr_event(const char *line)
{
	/* A layer opening or closing: a wallpaper over the snow, or a bar that
	 * changed what it keeps (the veil's clip). Our own two are no news. */
	if (starts(line, "openlayer>>") || starts(line, "closelayer>>")) {
		const char *ns = strstr(line, ">>") + 2;
		if (strcmp(ns, NAMESPACE) && strcmp(ns, NAMESPACE_VEIL))
			arm_ipc_check(starts(line, "openlayer>>"), true, 250);
		return;
	}
	static const char *const VIEW[] = {
		"fullscreen>>", "workspace", "focusedmon", "activespecial", "openwindow>>", "closewindow>>",
		"movewindow", "moveworkspace", "monitoradded", "monitorremoved",
	};
	for (size_t i = 0; i < sizeof VIEW / sizeof *VIEW; i++)
		if (starts(line, VIEW[i])) {
			app.fullscreen_recheck = true;
			arm_ipc_check(false, true, 100);
			return;
		}
}

static void hypr_events(void)
{
	for (;;) {
		ssize_t r = read(app.hypr_fd, app.hypr_buf + app.hypr_len, sizeof app.hypr_buf - app.hypr_len - 1);
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		if (r <= 0) {
			close(app.hypr_fd);
			app.hypr_fd = -1;
			return;
		}
		app.hypr_len += (size_t)r;
		app.hypr_buf[app.hypr_len] = 0;
		char *line = app.hypr_buf, *nl;
		while ((nl = strchr(line, '\n'))) {
			*nl = 0;
			hypr_event(line);
			line = nl + 1;
		}
		size_t rest = app.hypr_len - (size_t)(line - app.hypr_buf);
		memmove(app.hypr_buf, line, rest);
		app.hypr_len = rest;
		if (app.hypr_len == sizeof app.hypr_buf - 1)
			app.hypr_len = 0; /* a line longer than the buffer: drop it */
	}
}

/* ------------------------------------------------------------- gl */

static const char *VERT_SRC =
	"attribute vec4 a_flake;\n"   /* center x, y (buffer px, y down), radius, alpha */
	"attribute vec2 a_shape;\n"   /* exponent factor, point size */
	"uniform vec2 u_buf;\n"
	"varying vec2 v_center;\n"
	"varying vec3 v_g;\n"
	"void main() {\n"
	"  vec2 p = a_flake.xy;\n"
	"  gl_Position = vec4(p.x / u_buf.x * 2.0 - 1.0, 1.0 - p.y / u_buf.y * 2.0, 0.0, 1.0);\n"
	"  gl_PointSize = a_shape.y;\n"
	"  v_center = vec2(p.x, u_buf.y - p.y);\n"
	"  v_g = vec3(a_flake.z, a_flake.w, a_shape.x);\n"
	"}\n";

/* The reel's sprite: exp(-d^2/r^2 * coef) at whole-pixel offsets from the
 * flake's pixel, times its alpha level, truncated to 8 bits; color
 * (250, 248, 240). Premultiplied, as Wayland expects. */
static const char *FRAG_SRC =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform float u_scale;\n"
	"uniform vec3 u_rgb;\n"
	"uniform float u_mode;\n"   /* 0 alone, 1 beneath the veil, 2 the veil */
	"uniform float u_frac;\n"   /* the veil's share */
	"varying vec2 v_center;\n"
	"varying vec3 v_g;\n"
	"void main() {\n"
	"  vec2 d = (gl_FragCoord.xy - v_center) / u_scale;\n"
	"  float g = exp(-dot(d, d) / (v_g.x * v_g.x) * v_g.z);\n"
	"  float al = floor(g * 255.0 * v_g.y + 1e-4) / 255.0;\n"
	"  if (u_mode > 0.5) {\n"
	"    float u = floor(g * 255.0 * v_g.y * u_frac + 1e-4) / 255.0;\n"
	/* Beneath the veil: what, with the veil over it, composites to al. */
	"    al = u_mode > 1.5 ? u : (al - u) / (1.0 - u);\n"
	"  }\n"
	"  if (al <= 0.0) discard;\n"
	"  gl_FragColor = vec4(u_rgb * al, al);\n"
	"}\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(sh, sizeof log, NULL, log);
		logmsg("shader: %s", log);
		exit(1);
	}
	return sh;
}

static void gl_init(void)
{
	app.prog = glCreateProgram();
	glAttachShader(app.prog, compile(GL_VERTEX_SHADER, VERT_SRC));
	glAttachShader(app.prog, compile(GL_FRAGMENT_SHADER, FRAG_SRC));
	glBindAttribLocation(app.prog, 0, "a_flake");
	glBindAttribLocation(app.prog, 1, "a_shape");
	glLinkProgram(app.prog);
	GLint ok = 0;
	glGetProgramiv(app.prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		logmsg("shader program did not link");
		exit(1);
	}
	app.loc_flake = 0;
	app.loc_shape = 1;
	app.loc_buf = glGetUniformLocation(app.prog, "u_buf");
	app.loc_scale = glGetUniformLocation(app.prog, "u_scale");
	app.loc_rgb = glGetUniformLocation(app.prog, "u_rgb");
	app.loc_mode = glGetUniformLocation(app.prog, "u_mode");
	app.loc_frac = glGetUniformLocation(app.prog, "u_frac");
	glGenBuffers(1, &app.vbo);
	GLfloat range[2] = {1, 64};
	glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, range);
	app.point_max = range[1];
	app.gl_ready = true;
}

static void egl_init(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	app.egl_dpy = get_platform ? get_platform(EGL_PLATFORM_WAYLAND_KHR, app.display, NULL)
				   : eglGetDisplay((EGLNativeDisplayType)app.display);
	if (app.egl_dpy == EGL_NO_DISPLAY || !eglInitialize(app.egl_dpy, NULL, NULL)) {
		logmsg("EGL is not available on this display");
		exit(1);
	}
	app.create_platform_window =
		(PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLConfig cfgs[64];
	EGLint n = 0;
	if (!eglChooseConfig(app.egl_dpy, attrs, cfgs, 64, &n) || n < 1) {
		logmsg("no EGL config with an alpha channel");
		exit(1);
	}
	app.egl_cfg = cfgs[0];
	for (EGLint i = 0; i < n; i++) { /* exactly 8 bits each: ARGB8888 */
		EGLint r, a, size;
		eglGetConfigAttrib(app.egl_dpy, cfgs[i], EGL_RED_SIZE, &r);
		eglGetConfigAttrib(app.egl_dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
		eglGetConfigAttrib(app.egl_dpy, cfgs[i], EGL_BUFFER_SIZE, &size);
		if (r == 8 && a == 8 && size == 32) {
			app.egl_cfg = cfgs[i];
			break;
		}
	}
	const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	app.egl_ctx = eglCreateContext(app.egl_dpy, app.egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
	if (app.egl_ctx == EGL_NO_CONTEXT) {
		logmsg("could not create a GLES2 context");
		exit(1);
	}
}

/* ------------------------------------------------------------- snow surfaces */

static void snow_destroy(struct snow *s);

static double snow_scale(struct snow *s)
{
	if (s->viewport && s->frac_scale > 0)
		return s->frac_scale;
	return s->out->int_scale > 0 ? s->out->int_scale : 1;
}

static void snow_resize(struct snow *s)
{
	if (s->width <= 0 || s->height <= 0)
		return;
	double sc = snow_scale(s);
	s->buf_w = (int)lround(s->width * sc);
	s->buf_h = (int)lround(s->height * sc);
	if (s->viewport) {
		wp_viewport_set_destination(s->viewport, s->width, s->height);
	} else {
		wl_surface_set_buffer_scale(s->surface, (int32_t)sc);
	}
	if (s->egl_win)
		wl_egl_window_resize(s->egl_win, s->buf_w, s->buf_h, 0, 0);
	else
		s->egl_win = wl_egl_window_create(s->surface, s->buf_w, s->buf_h);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h)
{
	struct snow *s = data;
	zwlr_layer_surface_v1_ack_configure(l, serial);
	if ((int)w != s->width || (int)h != s->height || !s->egl_win) {
		s->width = (int)w;
		s->height = (int)h;
		snow_resize(s);
	}
	s->configured = true;
	if (!s->frame_cb)
		s->ready = true;
	schedule();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
	struct snow *s = data;
	struct output *o = s->out;
	if (s == o->veil)
		o->veil = NULL;
	else
		o->snow = NULL;
	snow_destroy(s);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void frac_preferred(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale120)
{
	struct snow *s = data;
	double sc = scale120 / 120.0;
	if (sc != s->frac_scale) {
		s->frac_scale = sc;
		snow_resize(s);
		if (!s->frame_cb && s->configured)
			s->ready = true;
		schedule();
	}
}

static const struct wp_fractional_scale_v1_listener frac_listener = {
	.preferred_scale = frac_preferred,
};

static uint32_t shell_layer(enum layer_mode m)
{
	return m == MODE_TOP ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
}

static struct snow *snow_create(struct output *o, bool veil)
{
	struct snow *s = calloc(1, sizeof *s);
	s->out = o;
	s->veil = veil;
	s->mode = app.cfg.layer;
	s->surface = wl_compositor_create_surface(app.compositor);
	if (app.frac_mgr && app.viewporter) {
		s->frac = wp_fractional_scale_manager_v1_get_fractional_scale(app.frac_mgr, s->surface);
		wp_fractional_scale_v1_add_listener(s->frac, &frac_listener, s);
		s->viewport = wp_viewporter_get_viewport(app.viewporter, s->surface);
	}
	/* The veil sits on the top layer: above windows, below panels and
	 * notifications (the overlay layer). */
	s->layer = zwlr_layer_shell_v1_get_layer_surface(app.layer_shell, s->surface, o->wl,
							 veil ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : shell_layer(s->mode),
							 veil ? NAMESPACE_VEIL : NAMESPACE);
	zwlr_layer_surface_v1_add_listener(s->layer, &layer_listener, s);
	zwlr_layer_surface_v1_set_size(s->layer, 0, 0);
	zwlr_layer_surface_v1_set_anchor(s->layer,
					 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer, 0);
	struct wl_region *none = wl_compositor_create_region(app.compositor);
	wl_surface_set_input_region(s->surface, none); /* clicks fall through */
	wl_region_destroy(none);
	wl_surface_commit(s->surface);
	return s;
}

/* Dynamic mode has a veil on every output with snow; the others have none. */
static void sync_veil(struct output *o)
{
	bool want = app.cfg.layer == MODE_DYNAMIC && o->snow && app.layer_shell;
	if (want && !o->veil)
		o->veil = snow_create(o, true);
	else if (!want && o->veil) {
		snow_destroy(o->veil);
		o->veil = NULL;
	}
}

static void snow_destroy(struct snow *s)
{
	if (!s)
		return;
	if (s->egl_surf != EGL_NO_SURFACE && s->egl_surf) {
		eglMakeCurrent(app.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(app.egl_dpy, s->egl_surf);
	}
	if (s->egl_win)
		wl_egl_window_destroy(s->egl_win);
	if (s->frame_cb)
		wl_callback_destroy(s->frame_cb);
	if (s->viewport)
		wp_viewport_destroy(s->viewport);
	if (s->frac)
		wp_fractional_scale_v1_destroy(s->frac);
	if (s->layer)
		zwlr_layer_surface_v1_destroy(s->layer);
	if (s->surface)
		wl_surface_destroy(s->surface);
	free(s);
}

static void restack_snow(struct output *o)
{
	if (o->restack_gave_up)
		return;
	/* Rate limit: something re-creating itself on top in a loop must not
	 * turn this into a flicker war. Eight restacks inside a minute is enough
	 * to say so and stop. */
	double t = now_ns() / 1e9;
	double oldest = o->restack_at[o->restack_i];
	if (oldest > 0 && t - oldest < 60) {
		logmsg("%s: another background surface keeps covering the snow; leaving it", o->name);
		o->restack_gave_up = true;
		return;
	}
	o->restack_at[o->restack_i] = t;
	o->restack_i = (o->restack_i + 1) % 8;
	if (app.verbose)
		logmsg("%s: wallpaper mapped above the snow, raising it", o->name);
	snow_destroy(o->snow);
	o->snow = snow_create(o, false);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
	struct snow *s = data;
	wl_callback_destroy(cb);
	s->frame_cb = NULL;
	s->ready = true;
	schedule();
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void ensure_flakes(struct output *s, size_t n)
{
	if (!s->seeded) {
		s->rng = seed_for(s->name);
		s->seeded = true;
	}
	if (n <= s->nflakes)
		return;
	if (n > s->cap) {
		size_t cap = s->cap ? s->cap : 256;
		while (cap < n)
			cap *= 2;
		struct flake *nf = realloc(s->flakes, cap * sizeof *nf);
		if (!nf)
			return;
		s->flakes = nf;
		s->cap = cap;
	}
	/* Appended in a fixed order from the output's own generator, so raising
	 * the density adds flakes and never reshuffles the ones already falling. */
	while (s->nflakes < n)
		s->flakes[s->nflakes++] = make_flake(&s->rng);
}

static double pmod(double a, double m)
{
	double r = fmod(a, m);
	return r < 0 ? r + m : r;
}

static void dump_flakes(struct snow *s, size_t n, double tau, double inten)
{
	FILE *f = fopen(app.dump_path, "a");
	if (!f)
		return;
	fprintf(f, "{\"output\":\"%s\",\"width\":%d,\"height\":%d,\"buf_w\":%d,\"buf_h\":%d,\"tau\":%.9g,"
		   "\"intensity\":%.9g,\"flakes\":[",
		s->out->name ? s->out->name : "", s->width, s->height, s->buf_w, s->buf_h, tau, inten);
	for (size_t i = 0; i < n; i++) {
		struct flake *fl = &s->out->flakes[i];
		fprintf(f, "%s[%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]", i ? "," : "", fl->ux, fl->uy, fl->s,
			fl->amp, fl->f, fl->ph, fl->a);
	}
	fputs("]}\n", f);
	fclose(f);
}

static void snow_render(struct snow *s)
{
	if (!s->egl_win || s->buf_w <= 0)
		return;
	if (!s->egl_surf) {
		s->egl_surf = app.create_platform_window
			? app.create_platform_window(app.egl_dpy, app.egl_cfg, s->egl_win, NULL)
			: eglCreateWindowSurface(app.egl_dpy, app.egl_cfg, (EGLNativeWindowType)s->egl_win, NULL);
		if (s->egl_surf == EGL_NO_SURFACE) {
			logmsg("could not create an EGL surface");
			exit(1);
		}
	}
	if (!eglMakeCurrent(app.egl_dpy, s->egl_surf, s->egl_surf, app.egl_ctx)) {
		logmsg("eglMakeCurrent failed");
		exit(1);
	}
	if (!s->interval_set) {
		eglSwapInterval(app.egl_dpy, 0); /* pacing comes from frame callbacks */
		s->interval_set = true;
	}
	if (!app.gl_ready)
		gl_init();

	double W = s->width, H = s->height, sc = (double)s->buf_w / W;
	double inten = app.cfg.opacity;
	double tau = app.tau;
	struct output *o = s->out;
	size_t n = o->covered ? 0 : (size_t)llround(REF_FLAKES * app.cfg.density * W * H / REF_AREA);
	s->clear_pending = false;
	ensure_flakes(o, n);
	if (n > o->nflakes)
		n = o->nflakes;
	/* 0 alone, 1 beneath a veil that is on screen, 2 the veil itself. */
	float mode = s->veil ? 2.f : (o->veil && o->veil->configured && o->veil->frames > 0) ? 1.f : 0.f;
	if (app.dump_path && !s->dumped && n && !s->veil) {
		dump_flakes(s, n, tau, inten);
		s->dumped = true;
	}

	if (app.vcap < n * 6) {
		size_t cap = n * 6 + 1536;
		float *nv = realloc(app.verts, cap * sizeof *nv);
		if (!nv)
			return;
		app.verts = nv;
		app.vcap = cap;
	}
	size_t m = 0;
	for (size_t i = 0; i < n; i++) {
		struct flake *fl = &o->flakes[i];
		double lv = nearbyint(fl->a * inten * ALPHA_LEVELS);
		if (lv <= 0)
			continue;
		/* Snow.draw: fall and wrap through a box 60 px taller and 40 px
		 * wider than the screen, drifting right at 14 px/s with a sway. */
		double y = fmod(fl->uy * (H + 60) + fl->v * tau, H + 60) - 30;
		double x = pmod(fl->ux * (W + 40) + 14 * tau + fl->amp * sin(2 * M_PI * fl->f * tau + fl->ph), W + 40) - 20;
		/* The reel pastes each sprite on whole pixels: the flake's center is
		 * the center of the pixel it falls in. */
		float *v = app.verts + m * 6;
		v[0] = (float)(nearbyint(x * sc - 0.5) + 0.5);
		v[1] = (float)(nearbyint(y * sc - 0.5) + 0.5);
		v[2] = fl->r;
		v[3] = (float)(lv / ALPHA_LEVELS);
		v[4] = fl->coef;
		float size = 2 * ceilf(fl->half * (float)sc) + 1;
		v[5] = size < app.point_max ? size : app.point_max;
		m++;
	}

	glViewport(0, 0, s->buf_w, s->buf_h);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	if (m) {
		glUseProgram(app.prog);
		glUniform2f(app.loc_buf, (float)s->buf_w, (float)s->buf_h);
		glUniform1f(app.loc_scale, (float)sc);
		glUniform3fv(app.loc_rgb, 1, FLAKE_RGB);
		glUniform1f(app.loc_mode, mode);
		glUniform1f(app.loc_frac, (float)DYNAMIC_VEIL);
		if (s->veil) {
			/* Not over bars: their reserved strips stay as in desktop mode. */
			int x0 = (int)lround(o->reserved[0] * sc), top = (int)lround(o->reserved[1] * sc);
			int x1 = s->buf_w - (int)lround(o->reserved[2] * sc);
			int y1 = s->buf_h - (int)lround(o->reserved[3] * sc);
			glEnable(GL_SCISSOR_TEST);
			glScissor(x0, s->buf_h - y1, x1 > x0 ? x1 - x0 : 0, y1 > top ? y1 - top : 0);
		}
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glBindBuffer(GL_ARRAY_BUFFER, app.vbo);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m * 6 * sizeof(float)), app.verts, GL_STREAM_DRAW);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(4 * sizeof(float)));
		glDrawArrays(GL_POINTS, 0, (GLsizei)m);
		glDisable(GL_SCISSOR_TEST);
	}

	s->frame_cb = wl_surface_frame(s->surface);
	wl_callback_add_listener(s->frame_cb, &frame_listener, s);
	s->ready = false;
	if (!eglSwapBuffers(app.egl_dpy, s->egl_surf)) {
		logmsg("eglSwapBuffers failed (0x%x)", eglGetError());
		app.running = false;
		app.exit_code = 1;
	}
	s->frames++;
}

/* ------------------------------------------------------------- pacing */

static uint64_t interval_ns(void)
{
	return 1000000000ull / (uint64_t)app.cfg.fps;
}

static bool wants_frame(struct snow *s)
{
	return s && s->configured && s->ready && (!s->out->covered || s->clear_pending);
}

/* Draw when a surface's last frame has been shown (its frame callback) and a
 * frame interval has passed. A hidden surface gets no callbacks and costs
 * nothing. */
static void schedule(void)
{
	if (app.timer_armed)
		return;
	bool any = false;
	struct output *o;
	wl_list_for_each(o, &app.outputs, link) if (wants_frame(o->snow) || wants_frame(o->veil)) any = true;
	if (!any)
		return;
	uint64_t now = now_ns();
	uint64_t slack = 2000000; /* 2 ms: render on a vblank-paced callback rather than one frame late */
	uint64_t due = app.last_frame_ns + interval_ns();
	due = due > slack ? due - slack : 0;
	if (due <= now)
		due = now + 1000;
	struct itimerspec it = {.it_value = {.tv_sec = (time_t)(due / 1000000000ull), .tv_nsec = (long)(due % 1000000000ull)}};
	timerfd_settime(app.timer_fd, TFD_TIMER_ABSTIME, &it, NULL);
	app.timer_armed = true;
}

static void tick(void)
{
	uint64_t now = now_ns();
	double dt = app.last_tick_ns ? (now - app.last_tick_ns) / 1e9 : 0;
	if (dt > 0.25)
		dt = 0.25; /* back from hiding: carry on, don't leap */
	app.last_tick_ns = now;
	app.tau = app.freeze_tau >= 0 ? app.freeze_tau : app.tau + dt * app.cfg.speed;
	app.last_frame_ns = now;
	struct output *o;
	wl_list_for_each(o, &app.outputs, link) {
		if (wants_frame(o->snow))
			snow_render(o->snow);
		if (wants_frame(o->veil))
			snow_render(o->veil);
	}
	if (app.verbose && now - app.stats_ns > 10000000000ull) {
		wl_list_for_each(o, &app.outputs, link) if (o->snow) {
			logmsg("%s: %.1f fps", o->name ? o->name : "?", o->snow->frames / ((now - app.stats_ns) / 1e9));
			o->snow->frames = 0;
		}
		app.stats_ns = now;
	}
}

/* ------------------------------------------------------------- settings apply */

static void apply_settings(void)
{
	struct settings c = read_settings();
	bool layer_changed = c.layer != app.cfg.layer;
	app.cfg = c;
	if (app.verbose)
		logmsg("speed %.2f, density %.2f, opacity %.2f, layer %s, %d fps", c.speed, c.density, c.opacity,
		       layer_name(c.layer), c.fps);
	if (!layer_changed)
		return;
	struct output *o;
	wl_list_for_each(o, &app.outputs, link) {
		if (!o->snow)
			continue;
		o->restack_gave_up = false;
		if (app.layer_shell_version >= 2) {
			zwlr_layer_surface_v1_set_layer(o->snow->layer, shell_layer(c.layer));
			o->snow->mode = c.layer;
			wl_surface_commit(o->snow->surface);
		} else {
			snow_destroy(o->snow);
			o->snow = snow_create(o, false);
		}
		sync_veil(o);
	}
	arm_ipc_check(on_background(c.layer), true, 300);
}

/* ------------------------------------------------------------- outputs */

static void output_geometry(void *d, struct wl_output *w, int32_t x, int32_t y, int32_t pw, int32_t ph,
			    int32_t sub, const char *make, const char *model, int32_t tr) {}
static void output_mode(void *d, struct wl_output *w, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {}

static void output_done(void *data, struct wl_output *w)
{
	struct output *o = data;
	o->done = true;
	if (!o->snow && app.layer_shell) {
		o->snow = snow_create(o, false);
		sync_veil(o);
		return;
	}
	struct snow *both[2] = {o->snow, o->veil};
	for (int i = 0; i < 2; i++) {
		struct snow *s = both[i];
		if (!s || s->viewport)
			continue;
		snow_resize(s);
		if (!s->frame_cb && s->configured)
			s->ready = true;
	}
	schedule();
}

static void output_scale(void *data, struct wl_output *w, int32_t factor)
{
	((struct output *)data)->int_scale = factor;
}

static void output_name(void *data, struct wl_output *w, const char *name)
{
	struct output *o = data;
	free(o->name);
	o->name = strdup(name);
}

static void output_description(void *d, struct wl_output *w, const char *desc) {}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void output_free(struct output *o)
{
	snow_destroy(o->veil);
	snow_destroy(o->snow);
	free(o->flakes);
	if (wl_output_get_version(o->wl) >= 3)
		wl_output_release(o->wl);
	else
		wl_output_destroy(o->wl);
	wl_list_remove(&o->link);
	free(o->name);
	free(o);
}

static void registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		app.compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		app.layer_shell_version = version < 4 ? version : 4;
		app.layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, app.layer_shell_version);
	} else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
		app.frac_mgr = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
	} else if (!strcmp(iface, wp_viewporter_interface.name)) {
		app.viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		struct output *o = calloc(1, sizeof *o);
		o->global = name;
		o->int_scale = 1;
		o->wl = wl_registry_bind(r, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(o->wl, &output_listener, o);
		wl_list_insert(app.outputs.prev, &o->link);
	}
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &app.outputs, link) if (o->global == name) output_free(o);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

/* ------------------------------------------------------------- startup */

static void connect_display(void)
{
	const char *env = getenv("WAYLAND_DISPLAY");
	if (env && *env) {
		app.display = wl_display_connect(NULL);
		if (app.display) {
			const char *b = strrchr(env, '/');
			snprintf(app.wl_socket, sizeof app.wl_socket, "%s", b ? b + 1 : env);
			return;
		}
		logmsg("could not connect to WAYLAND_DISPLAY=%s; looking for Hyprland instead", env);
	}
	/* A user service may start without the session's environment (Hyprland
	 * launched without uwsm, or a stale WAYLAND_DISPLAY left in the manager):
	 * Hyprland's lock file names its socket. */
	char dir[PATH_MAX], sock[256];
	if (find_hyprland(NULL, dir, sizeof dir, sock, sizeof sock)) {
		app.display = wl_display_connect(sock);
		if (app.display) {
			snprintf(app.wl_socket, sizeof app.wl_socket, "%s", sock);
			return;
		}
	}
	logmsg("no Wayland display: run this inside a Hyprland session");
	exit(EX_UNAVAILABLE);
}

static int take_lock(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/letitsnow-%s.lock", rt && *rt ? rt : "/tmp", app.wl_socket);
	int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	/* `letitsnow status` tests the lock by taking it for an instant, so a
	 * renderer started during that instant must not take it for another one
	 * running: retry for a moment before saying so. */
	int tries = 0;
	while (flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK && ++tries < 30)
		usleep(50000);
	if (tries >= 30) {
		char pid[32] = "";
		ssize_t r = pread(fd, pid, sizeof pid - 1, 0);
		if (r > 0)
			pid[r] = 0;
		logmsg("already snowing on %s (pid %s)", app.wl_socket, trim(pid));
		exit(0);
	}
	if (ftruncate(fd, 0) == 0) {
		char pid[32];
		int n = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());
		if (pwrite(fd, pid, (size_t)n, 0) < 0) {
			/* the lock holds either way; the pid is only for messages */
		}
	}
	return fd;
}

static void watch_config(void)
{
	mkdir(app.cfg_dir, 0755); /* the CLI normally made it; harmless if it exists */
	app.ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (app.ino_fd >= 0 &&
	    inotify_add_watch(app.ino_fd, app.cfg_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE) < 0) {
		logmsg("not watching %s: %s (send SIGHUP after editing)", app.cfg_dir, strerror(errno));
		close(app.ino_fd);
		app.ino_fd = -1;
	}
}

static void config_events(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool hit = false;
	ssize_t r;
	while ((r = read(app.ino_fd, buf, sizeof buf)) > 0) {
		for (char *p = buf; p < buf + r;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && !strcmp(ev->name, "config"))
				hit = true;
			p += sizeof *ev + ev->len;
		}
	}
	if (hit)
		apply_settings();
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: letitsnowd [-v] [--version]\n"
		"Draws snow on every output of the running Hyprland (any wlr-layer-shell\n"
		"compositor works). Settings: $XDG_CONFIG_HOME/letitsnow/config, applied\n"
		"as soon as the file changes. Use `letitsnow` to change them.\n");
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			app.verbose = true;
		else if (!strcmp(argv[i], "--version")) {
			puts(LETITSNOW_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else {
			usage(stderr);
			return 2;
		}
	}
	setvbuf(stderr, NULL, _IOLBF, 0);

	app.sig_fd = app.ino_fd = app.timer_fd = app.ipc_fd = app.hypr_fd = -1;
	app.freeze_tau = -1;
	const char *freeze = getenv("LETITSNOW_FREEZE");
	if (freeze && *freeze)
		app.freeze_tau = strtod(freeze, NULL);
	app.dump_path = getenv("LETITSNOW_DUMP");
	if (app.dump_path && !*app.dump_path)
		app.dump_path = NULL;

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	signal(SIGPIPE, SIG_IGN);
	app.sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

	config_paths();
	app.cfg = read_settings();
	wl_list_init(&app.outputs);
	connect_display();
	int lock_fd = take_lock();

	char sock[256];
	if (!find_hyprland(app.wl_socket, app.hypr_dir, sizeof app.hypr_dir, sock, sizeof sock))
		app.hypr_dir[0] = 0; /* not Hyprland: no restacking, everything else works */

	/* Before the first roundtrip: configure events arrive there and schedule frames. */
	app.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	app.ipc_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (app.timer_fd < 0 || app.ipc_fd < 0) {
		logmsg("timerfd: %s", strerror(errno));
		return 1;
	}

	app.registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(app.registry, &registry_listener, NULL);
	wl_display_roundtrip(app.display);
	if (!app.compositor || !app.layer_shell) {
		logmsg("this compositor has no wlr-layer-shell; letitsnow needs Hyprland or another wlroots-style compositor");
		return EX_UNAVAILABLE;
	}
	egl_init();
	wl_display_roundtrip(app.display); /* output names and scales; surfaces are made on each output's done */

	watch_config();
	if (app.hypr_dir[0]) {
		app.hypr_fd = hypr_connect(".socket2.sock");
		if (app.hypr_fd >= 0)
			fcntl(app.hypr_fd, F_SETFL, O_NONBLOCK);
		arm_ipc_check(true, true, 800); /* a wallpaper started after us at login; a game already fullscreen */
	}
	if (app.verbose)
		logmsg("snowing on %s (Hyprland IPC %s)", app.wl_socket, app.hypr_fd >= 0 ? "on" : "off");
	app.stats_ns = now_ns();
	app.running = true;

	enum { P_WL, P_SIG, P_INO, P_TIMER, P_IPC, P_HYPR, P_N };
	while (app.running) {
		while (wl_display_prepare_read(app.display) != 0) {
			if (wl_display_dispatch_pending(app.display) < 0)
				goto display_error;
		}
		if (wl_display_flush(app.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(app.display);
			goto display_error;
		}
		struct pollfd p[P_N] = {
			[P_WL] = {.fd = wl_display_get_fd(app.display), .events = POLLIN},
			[P_SIG] = {.fd = app.sig_fd, .events = POLLIN},
			[P_INO] = {.fd = app.ino_fd, .events = POLLIN},
			[P_TIMER] = {.fd = app.timer_fd, .events = POLLIN},
			[P_IPC] = {.fd = app.ipc_fd, .events = POLLIN},
			[P_HYPR] = {.fd = app.hypr_fd, .events = POLLIN},
		};
		if (poll(p, P_N, -1) < 0 && errno != EINTR) {
			wl_display_cancel_read(app.display);
			break;
		}
		if (p[P_WL].revents & POLLIN) {
			if (wl_display_read_events(app.display) < 0)
				goto display_error;
		} else {
			wl_display_cancel_read(app.display);
		}
		if (p[P_WL].revents & (POLLERR | POLLHUP))
			goto display_error;
		if (wl_display_dispatch_pending(app.display) < 0)
			goto display_error;

		if (p[P_SIG].revents & POLLIN) {
			struct signalfd_siginfo si;
			while (read(app.sig_fd, &si, sizeof si) == sizeof si) {
				if (si.ssi_signo == SIGHUP)
					apply_settings();
				else
					app.running = false;
			}
		}
		if (p[P_INO].revents & POLLIN)
			config_events();
		if (p[P_IPC].revents & POLLIN) {
			uint64_t x;
			if (read(app.ipc_fd, &x, sizeof x) > 0)
				ipc_checks();
		}
		if (app.hypr_fd >= 0 && (p[P_HYPR].revents & (POLLIN | POLLHUP | POLLERR)))
			hypr_events();
		if (p[P_TIMER].revents & POLLIN) {
			uint64_t x;
			if (read(app.timer_fd, &x, sizeof x) > 0) {
				app.timer_armed = false;
				tick();
			}
		}
	}
	goto out;

display_error: {
	int err = wl_display_get_error(app.display);
	if (err == EPROTO) {
		logmsg("Wayland protocol error");
		app.exit_code = 1;
	} else {
		/* The compositor went away: the session ended. Nothing to retry. */
		if (app.verbose)
			logmsg("display closed (%s)", strerror(err ? err : errno));
		app.exit_code = 0;
	}
	_exit(app.exit_code); /* the connection is gone; tearing down surfaces would only fail */
}

out: {
	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &app.outputs, link) output_free(o);
	wl_display_roundtrip(app.display); /* let the compositor unmap the snow before we go */
	eglMakeCurrent(app.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(app.egl_dpy);
	wl_display_disconnect(app.display);
	close(lock_fd);
	return app.exit_code;
}
}
