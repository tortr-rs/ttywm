/* ttywm - floating window manager that looks like a tty */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dbus/dbus.h>
#include <security/pam_appl.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>

#define LENGTH(X)       (sizeof(X) / sizeof(X)[0])
#define MAX(A, B)       ((A) > (B) ? (A) : (B))
#define MIN(A, B)       ((A) < (B) ? (A) : (B))
#define CLEANMASK(mask) ((mask) & ~(LockMask|Mod2Mask))
#define NSCR            9
#define SHELLCLASS      "TtywmShell"
#define SHELLCMD        "ttywm-term"

enum { ColBg, ColWindow, ColOther, ColTitle, ColBar, ColLast };
enum { ActRun, ActTerminal, ActMenu, ActClose, ActNext, ActFull, ActQuit, ActView, ActSend, ActVolUp, ActVolDown, ActMute,
       ActLightUp, ActLightDown, ActLeft, ActRight, ActMax, ActRestore, ActLock };
enum { SegNone, SegNet, SegVol, SegLight };

typedef struct {
	unsigned int mod;
	KeySym keysym;
	int act, n;
	char *cmd;
} Key;

typedef struct Notif Notif;
struct Notif {
	unsigned int id;
	char app[64], line[4][128];
	int nlines, w, h;
	time_t until;
	Window win;
	XftDraw *draw;
	Notif *next;
};

typedef struct Client Client;
struct Client {
	char name[256];
	int x, y, w, h, oldx, oldy, oldw, oldh;
	int scr, isfull, ignoreunmap;
	int snapped, sx, sy, sw, sh;	/* where it was before super+arrows */
	Window win, frame;
	XftDraw *draw;
	Client *next;
};

/* settings, from ~/.config/ttywm/config */
static char terminal[256] = "ttywm-term", menu[256] = "rofi -show drun";
static char colname[ColLast][32] = { "black", "white", "grey", "white", "grey" };
static int clock24 = 1, showclock = 1, showdate = 1, showbattery = 1, shownetwork = 1, showvolume = 1, showlight = 1, big;
static Key *keys;
static int nkeys;

static const char defaultconfig[] =
"# ttywm settings\n"
"# change what's after the = and save. it works right away.\n"
"\n"
"# colours: black blue green cyan red magenta brown grey\n"
"#          dark grey, light blue, light green, light cyan, light red, pink, yellow, white\n"
"window colour = white\n"
"other windows colour = grey\n"
"title colour = white\n"
"bottom line colour = grey\n"
"\n"
"# normal or big. big is for small, sharp screens. changes when you log in again\n"
"text size = normal\n"
"\n"
"# what's on the bottom line. on or off\n"
"network = on\n"
"volume = on\n"
"brightness = on\n"
"battery = on\n"
"date = on\n"
"# 24 hour, 12 hour or off\n"
"clock = 24 hour\n"
"\n"
"# the terminal for new windows, and the apps menu\n"
"terminal = ttywm-term\n"
"apps menu = rofi -show drun\n"
"\n"
"# keys. super is the windows key. after the = goes one of:\n"
"#   terminal, apps menu, close window, next window, full screen, log out\n"
"# or any program, like firefox\n"
"super+enter = terminal\n"
"super+d = apps menu\n"
"super+q = close window\n"
"super+tab = next window\n"
"super+f = full screen\n"
"super+shift+e = log out\n"
"# super+1 to super+9 go to tty1 to tty9, super+shift+1 to 9 send a window there\n"
"# super+left and super+right put a window on half the screen, super+up fills it,\n"
"# super+down puts it back. super+l locks the screen\n"
"\n"
"# programs to start when you log in, one per line, like:\n"
"# start = firefox\n";

static Display *dpy;
static Window root, bar, wmcheck;
static int sw, sh, wh, cw, ch;
static int cur, newshell = -1, running = 1;
static Window shell[NSCR];
static Client *clients, *sel;
static XftFont *xfont;
static XftDraw *bardraw;
static XftColor col[ColLast];
static Atom wmprotocols, wmdelete, wmstate, netwmname, netwmstate, netfullscreen, netwmcheck, utf8string;
static char cfgpath[4096];
static time_t cfgmtime;
static int vol = -1, muted;
static void (*handler[LASTEvent])(XEvent *);
static char backlight[256];	/* /sys/class/backlight/..., or empty */
static DBusConnection *bus;
static Notif *notifs;
static unsigned int lastid;
static struct { int x0, x1, seg; } segs[8];
static int nsegs;

static void
die(const char *s)
{
	fprintf(stderr, "ttywm: %s\n", s);
	exit(1);
}

static int
xerror(Display *d, XErrorEvent *ee)
{
	/* windows die while we talk to them */
	if (ee->error_code == BadWindow || ee->error_code == BadMatch || ee->error_code == BadDrawable)
		return 0;
	fprintf(stderr, "ttywm: X error %d, request %d\n", ee->error_code, ee->request_code);
	return 0;
}

static int
xerrorstart(Display *d, XErrorEvent *ee)
{
	die("another window manager is already running");
	return -1;
}

static int
inpath(const char *cmd)
{
	char buf[4096], path[4096], *p, *dir;

	snprintf(buf, sizeof buf, "%s", getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	for (p = buf; (dir = strsep(&p, ":"));) {
		snprintf(path, sizeof path, "%s/%s", *dir ? dir : ".", cmd);
		if (access(path, X_OK) == 0)
			return 1;
	}
	return 0;
}

static void
run(const char *cmd)
{
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(1);
	}
}

/* runs cmd and waits for it, keeping the first line it prints */
static void
runwait(const char *cmd, char *out, size_t n)
{
	FILE *f;

	if (out)
		*out = '\0';
	if (!(f = popen(cmd, "r")))
		return;
	if (out && !fgets(out, n, f))
		*out = '\0';
	while (fgetc(f) != EOF);
	pclose(f);
}

static int
readfile(const char *path, char *buf, size_t n)
{
	FILE *f;
	size_t len;

	if (!(f = fopen(path, "r")))
		return 0;
	len = fread(buf, 1, n - 1, f);
	fclose(f);
	buf[len] = '\0';
	while (len && isspace((unsigned char)buf[len - 1]))
		buf[--len] = '\0';
	return 1;
}

static long
readnum(const char *dir, const char *file)
{
	char path[512], buf[64];

	snprintf(path, sizeof path, "%s/%s", dir, file);
	return readfile(path, buf, sizeof buf) ? atol(buf) : -1;
}

/* ---- settings ---- */

static char *
trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); e--);
	*e = '\0';
	return s;
}

/* "Dark  Grey" -> "darkgrey" */
static void
squash(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	for (; *src && i < n - 1; src++)
		if (!isspace((unsigned char)*src))
			dst[i++] = tolower((unsigned char)*src);
	dst[i] = '\0';
}

static int
yes(const char *v)
{
	char s[16];

	squash(s, v, sizeof s);
	return !strcmp(s, "on") || !strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "1");
}

static const char *
colourhex(const char *name)
{
	static const char *vga[][2] = {
		{ "black", "#000000" }, { "blue", "#0000aa" }, { "green", "#00aa00" }, { "cyan", "#00aaaa" },
		{ "red", "#aa0000" }, { "magenta", "#aa00aa" }, { "brown", "#aa5500" }, { "grey", "#aaaaaa" },
		{ "gray", "#aaaaaa" }, { "darkgrey", "#555555" }, { "darkgray", "#555555" },
		{ "lightblue", "#5555ff" }, { "lightgreen", "#55ff55" }, { "lightcyan", "#55ffff" },
		{ "lightred", "#ff5555" }, { "pink", "#ff55ff" }, { "yellow", "#ffff55" }, { "white", "#ffffff" },
	};
	char s[32];
	unsigned int i;

	squash(s, name, sizeof s);
	for (i = 0; i < LENGTH(vga); i++)
		if (!strcmp(s, vga[i][0]))
			return vga[i][1];
	return name;	/* #rrggbb, or any name X knows */
}

/* "window colour" or "window color" */
static int
iskey(const char *key, const char *want)
{
	char alt[64];
	const char *p;

	if (!strcmp(key, want))
		return 1;
	if (!(p = strstr(want, "colour")))
		return 0;
	snprintf(alt, sizeof alt, "%.*scolor%s", (int)(p - want), want, p + 6);
	return !strcmp(key, alt);
}

static KeySym
keyname(const char *s)
{
	static const char *names[][2] = {
		{ "enter", "Return" }, { "return", "Return" }, { "tab", "Tab" }, { "space", "space" },
		{ "esc", "Escape" }, { "escape", "Escape" }, { "backspace", "BackSpace" }, { "delete", "Delete" },
		{ "up", "Up" }, { "down", "Down" }, { "left", "Left" }, { "right", "Right" },
		{ "home", "Home" }, { "end", "End" }, { "print", "Print" }, { "printscreen", "Print" },
	};
	char buf[32];
	unsigned int i;

	for (i = 0; i < LENGTH(names); i++)
		if (!strcmp(s, names[i][0]))
			return XStringToKeysym(names[i][1]);
	if (s[0] == 'f' && isdigit((unsigned char)s[1])) {
		snprintf(buf, sizeof buf, "F%s", s + 1);
		return XStringToKeysym(buf);
	}
	return XStringToKeysym(s);
}

static void
addkey(unsigned int mod, KeySym sym, int act, int n, const char *cmd)
{
	Key *k;

	if (!sym || !(k = realloc(keys, (nkeys + 1) * sizeof(Key))))
		return;
	keys = k;
	k = &keys[nkeys++];
	k->mod = mod;
	k->keysym = sym;
	k->act = act;
	k->n = n;
	k->cmd = cmd ? strdup(cmd) : NULL;
}

/* super+shift+e = log out */
static int
parsekey(char *name, char *value)
{
	static const struct { const char *word; int act; } acts[] = {
		{ "terminal", ActTerminal }, { "appsmenu", ActMenu }, { "menu", ActMenu },
		{ "closewindow", ActClose }, { "close", ActClose }, { "nextwindow", ActNext },
		{ "fullscreen", ActFull }, { "logout", ActQuit }, { "quit", ActQuit }, { "lock", ActLock },
		{ "lockscreen", ActLock },
	};
	unsigned int mod = 0, i;
	char *tok, *last = NULL, act[64];

	if (!strchr(name, '+'))
		return 0;
	for (tok = strtok(name, "+"); tok; tok = strtok(NULL, "+")) {
		tok = trim(tok);
		if (last) {
			if (!strcmp(last, "super") || !strcmp(last, "win") || !strcmp(last, "windows"))
				mod |= Mod4Mask;
			else if (!strcmp(last, "shift"))
				mod |= ShiftMask;
			else if (!strcmp(last, "ctrl") || !strcmp(last, "control"))
				mod |= ControlMask;
			else if (!strcmp(last, "alt"))
				mod |= Mod1Mask;
			else
				return 0;
		}
		last = tok;
	}
	if (!last)
		return 0;
	squash(act, value, sizeof act);
	for (i = 0; i < LENGTH(acts); i++)
		if (!strcmp(act, acts[i].word)) {
			addkey(mod, keyname(last), acts[i].act, 0, NULL);
			return 1;
		}
	addkey(mod, keyname(last), ActRun, 0, value);
	return 1;
}

static void
loadconfig(int firsttime)
{
	FILE *f;
	char line[1024], *name, *value, *eq, key[64];
	int i;
	struct stat st;

	if (stat(cfgpath, &st) < 0 && (f = fopen(cfgpath, "w"))) {
		fputs(defaultconfig, f);
		fclose(f);
	}
	if (stat(cfgpath, &st) == 0)
		cfgmtime = st.st_mtime;

	for (i = 0; i < nkeys; i++)
		free(keys[i].cmd);
	nkeys = 0;
	f = fopen(cfgpath, "r");
	if (!f) {
		/* no file and nowhere to write one: the defaults, straight from memory */
		f = fmemopen((void *)defaultconfig, sizeof defaultconfig - 1, "r");
		if (!f)
			return;
	}
	while (fgets(line, sizeof line, f)) {
		if ((eq = strchr(line, '#')))
			*eq = '\0';
		if (!(eq = strchr(line, '=')))
			continue;
		*eq = '\0';
		name = trim(line);
		value = trim(eq + 1);
		for (eq = name; *eq; eq++)
			*eq = tolower((unsigned char)*eq);
		if (parsekey(name, value))
			continue;
		squash(key, name, sizeof key);
		if (iskey(key, "windowcolour"))
			snprintf(colname[ColWindow], sizeof colname[0], "%s", value);
		else if (iskey(key, "otherwindowscolour"))
			snprintf(colname[ColOther], sizeof colname[0], "%s", value);
		else if (iskey(key, "titlecolour"))
			snprintf(colname[ColTitle], sizeof colname[0], "%s", value);
		else if (iskey(key, "bottomlinecolour"))
			snprintf(colname[ColBar], sizeof colname[0], "%s", value);
		else if (!strcmp(key, "textsize")) {
			if (firsttime)
				big = !strcasecmp(value, "big");
		}
		else if (!strcmp(key, "network"))
			shownetwork = yes(value);
		else if (!strcmp(key, "volume"))
			showvolume = yes(value);
		else if (!strcmp(key, "brightness"))
			showlight = yes(value);
		else if (!strcmp(key, "battery"))
			showbattery = yes(value);
		else if (!strcmp(key, "date"))
			showdate = yes(value);
		else if (!strcmp(key, "clock")) {
			squash(key, value, sizeof key);
			showclock = strcmp(key, "off") != 0;
			clock24 = strncmp(key, "12", 2) != 0;
		} else if (!strcmp(key, "terminal"))
			snprintf(terminal, sizeof terminal, "%s", value);
		else if (!strcmp(key, "appsmenu") || !strcmp(key, "menu"))
			snprintf(menu, sizeof menu, "%s", value);
		else if (!strcmp(key, "start")) {
			if (firsttime)
				run(value);
		}
		else
			fprintf(stderr, "ttywm: %s: don't know '%s'\n", cfgpath, name);
	}
	fclose(f);

	/* these are always there */
	for (i = 0; i < NSCR; i++) {
		addkey(Mod4Mask, XK_1 + i, ActView, i, NULL);
		addkey(Mod4Mask|ShiftMask, XK_1 + i, ActSend, i, NULL);
	}
	addkey(0, XStringToKeysym("XF86AudioRaiseVolume"), ActVolUp, 0, NULL);
	addkey(0, XStringToKeysym("XF86AudioLowerVolume"), ActVolDown, 0, NULL);
	addkey(0, XStringToKeysym("XF86AudioMute"), ActMute, 0, NULL);
	addkey(0, XStringToKeysym("XF86MonBrightnessUp"), ActLightUp, 0, NULL);
	addkey(0, XStringToKeysym("XF86MonBrightnessDown"), ActLightDown, 0, NULL);
	addkey(Mod4Mask, XK_Left, ActLeft, 0, NULL);
	addkey(Mod4Mask, XK_Right, ActRight, 0, NULL);
	addkey(Mod4Mask, XK_Up, ActMax, 0, NULL);
	addkey(Mod4Mask, XK_Down, ActRestore, 0, NULL);
	addkey(Mod4Mask, XK_l, ActLock, 0, NULL);
}

static void
grabkeys(void)
{
	unsigned int j, mods[] = { 0, LockMask, Mod2Mask, LockMask|Mod2Mask };
	KeyCode code;
	int i;

	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < nkeys; i++)
		if ((code = XKeysymToKeycode(dpy, keys[i].keysym)))
			for (j = 0; j < LENGTH(mods); j++)
				XGrabKey(dpy, code, keys[i].mod | mods[j], root, True, GrabModeAsync, GrabModeAsync);
}

static void
loadcolours(void)
{
	static int loaded;
	Visual *vis = DefaultVisual(dpy, DefaultScreen(dpy));
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
	int i;

	for (i = 0; i < ColLast; i++) {
		if (loaded)
			XftColorFree(dpy, vis, cmap, &col[i]);
		if (!XftColorAllocName(dpy, vis, cmap, colourhex(colname[i]), &col[i])
		&& !XftColorAllocName(dpy, vis, cmap, i == ColBg ? "#000000" : "#aaaaaa", &col[i]))
			die("cannot allocate color");
	}
	loaded = 1;
}

/* ---- drawing ---- */

static Client *
wintoclient(Window w)
{
	Client *c;

	for (c = clients; c; c = c->next)
		if (c->win == w || c->frame == w)
			return c;
	return NULL;
}

static int
shellof(Window w)
{
	int i;

	for (i = 0; i < NSCR; i++)
		if (shell[i] && shell[i] == w)
			return i;
	return -1;
}

static void
text(XftDraw *d, int c, int x, int y, const char *s)
{
	XftDrawStringUtf8(d, &col[c], xfont, x, y + xfont->ascent, (const FcChar8 *)s, strlen(s));
}

/* ┌─[ name ]───[x]┐ */
static void
drawframe(Client *c)
{
	char buf[2048], *p;
	int cols = c->w / cw + 2, rows = c->h / ch;
	int bc = c == sel ? ColWindow : ColOther;
	int n = MIN((int)strlen(c->name), MAX(cols - 10, 0));
	int i;

	if (c->isfull)
		return;
	XftDrawRect(c->draw, &col[ColBg], 0, 0, c->w + 2 * cw, c->h + 2 * ch);

	text(c->draw, bc, 0, 0, "┌─[");
	snprintf(buf, sizeof buf, " %.*s ", n, c->name);
	text(c->draw, ColTitle, 3 * cw, 0, buf);
	p = buf + sprintf(buf, "]");
	for (i = n + 6; i < cols - 4; i++)
		p += sprintf(p, "─");
	sprintf(p, "[x]┐");
	text(c->draw, bc, (n + 5) * cw, 0, buf);

	for (i = 1; i <= rows; i++) {
		text(c->draw, bc, 0, i * ch, "│");
		text(c->draw, bc, (cols - 1) * cw, i * ch, "│");
	}

	p = buf + sprintf(buf, "└");
	for (i = 2; i < cols; i++)
		p += sprintf(p, "─");
	sprintf(p, "┘");
	text(c->draw, bc, 0, (rows + 1) * ch, buf);
}

/* ---- the bottom line ---- */

static void
readvolume(void)
{
	char buf[128];
	float v;

	vol = -1;
	runwait("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", buf, sizeof buf);
	if (sscanf(buf, "Volume: %f", &v) == 1) {
		vol = v * 100 + 0.5;
		muted = strstr(buf, "MUTED") != NULL;
	}
}

/* the first backlight there is. the video group may write it */
static void
findbacklight(void)
{
	DIR *d;
	struct dirent *e;

	backlight[0] = '\0';
	if (!(d = opendir("/sys/class/backlight")))
		return;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.') {
			snprintf(backlight, sizeof backlight, "/sys/class/backlight/%s", e->d_name);
			break;
		}
	closedir(d);
}

static int
light(void)
{
	long now, max;

	if (!backlight[0] || (now = readnum(backlight, "brightness")) < 0 || (max = readnum(backlight, "max_brightness")) <= 0)
		return -1;
	return (now * 100 + max / 2) / max;
}

static void
setlight(int step)
{
	char path[300];
	long max;
	int pct = light();
	FILE *f;

	if (pct < 0 || (max = readnum(backlight, "max_brightness")) <= 0)
		return;
	/* 5% steps, but never all the way to black */
	pct = MIN(MAX(pct + step, 1), 100);
	snprintf(path, sizeof path, "%s/brightness", backlight);
	if ((f = fopen(path, "w"))) {
		fprintf(f, "%ld\n", MAX(pct * max / 100, 1));
		fclose(f);
	} else
		fprintf(stderr, "ttywm: %s: can't write it, are you in the video group?\n", path);
}

static void
network(char *s, size_t n)
{
	DIR *d;
	struct dirent *e;
	char path[512], state[32];
	int wired = 0, wifi = 0;

	if (!(d = opendir("/sys/class/net")))
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo"))
			continue;
		snprintf(path, sizeof path, "/sys/class/net/%s/operstate", e->d_name);
		if (!readfile(path, state, sizeof state) || strcmp(state, "up"))
			continue;
		snprintf(path, sizeof path, "/sys/class/net/%s/wireless", e->d_name);
		if (access(path, F_OK) == 0)
			wifi = 1;
		else
			wired = 1;
	}
	closedir(d);
	snprintf(s, n, "%s", wired ? "wired" : wifi ? "wifi" : "offline");
}

/* all batteries together: a T480 has two */
static int
battery(char *s, size_t n)
{
	DIR *d;
	struct dirent *e;
	char dir[512], path[600], buf[32];
	long now = 0, full = 0, a, b;
	int charging = 0;

	if (!(d = opendir("/sys/class/power_supply")))
		return 0;
	while ((e = readdir(d))) {
		snprintf(dir, sizeof dir, "/sys/class/power_supply/%s", e->d_name);
		snprintf(path, sizeof path, "%s/type", dir);
		if (!readfile(path, buf, sizeof buf) || strcmp(buf, "Battery"))
			continue;
		snprintf(path, sizeof path, "%s/status", dir);
		if (readfile(path, buf, sizeof buf) && !strcmp(buf, "Charging"))
			charging = 1;
		if ((a = readnum(dir, "energy_now")) < 0 || (b = readnum(dir, "energy_full")) <= 0) {
			a = readnum(dir, "charge_now");
			b = readnum(dir, "charge_full");
		}
		if (a >= 0 && b > 0) {
			now += a;
			full += b;
		}
	}
	closedir(d);
	if (!full)
		return 0;
	snprintf(s, n, "bat %ld%%%s", now * 100 / full, charging ? "+" : "");
	return 1;
}

static void
drawbar(void)
{
	char seg[8][64];
	int kind[8], n = 0, i, x, len = 0;
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);

	if (shownetwork) {
		network(seg[n], sizeof seg[n]);
		kind[n++] = SegNet;
	}
	if (showvolume && vol >= 0) {
		snprintf(seg[n], sizeof seg[n], muted ? "vol muted" : "vol %d%%", vol);
		kind[n++] = SegVol;
	}
	if (showlight && light() >= 0) {
		snprintf(seg[n], sizeof seg[n], "light %d%%", light());
		kind[n++] = SegLight;
	}
	if (showbattery && battery(seg[n], sizeof seg[n]))
		kind[n++] = SegNone;
	if (showdate) {
		strftime(seg[n], sizeof seg[n], "%a %d %b", tm);
		kind[n++] = SegNone;
	}
	if (showclock) {
		strftime(seg[n], sizeof seg[n], clock24 ? "%H:%M" : "%I:%M %p", tm);
		kind[n++] = SegNone;
	}
	snprintf(seg[n], sizeof seg[n], "tty%d", cur + 1);
	kind[n++] = SegNone;

	for (i = 0; i < n; i++)
		len += strlen(seg[i]) + 2;
	XftDrawRect(bardraw, &col[ColBg], 0, 0, sw, ch);
	x = sw - len * cw;
	nsegs = 0;
	for (i = 0; i < n; i++) {
		text(bardraw, ColBar, x, 0, seg[i]);
		segs[nsegs].x0 = x;
		segs[nsegs].x1 = x + strlen(seg[i]) * cw;
		segs[nsegs++].seg = kind[i];
		x += (strlen(seg[i]) + 2) * cw;
	}
}

static void
setvolume(const char *how)
{
	char cmd[128];

	if (!strcmp(how, "mute"))
		snprintf(cmd, sizeof cmd, "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
	else
		snprintf(cmd, sizeof cmd, "wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ %s", how);
	runwait(cmd, NULL, 0);
	readvolume();
	drawbar();
}

/* ---- notifications: ttywm is org.freedesktop.Notifications ---- */

#define NOTIFCOLS 48

/* characters, not bytes */
static int
ulen(const char *s)
{
	int n = 0;

	for (; *s; s++)
		n += (*s & 0xc0) != 0x80;
	return n;
}

/* the first n characters of s */
static void
ucut(char *dst, const char *s, int n, size_t size)
{
	size_t i = 0;

	for (; *s && i < size - 1; s++) {
		if ((*s & 0xc0) != 0x80 && n-- <= 0)
			break;
		dst[i++] = *s;
	}
	dst[i] = '\0';
}

/* <b>bold</b> &amp; co. as plain text */
static void
plain(char *dst, const char *s, size_t size)
{
	static const char *ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" } };
	size_t i = 0;
	unsigned int e;

	while (*s && i < size - 1) {
		if (*s == '<' && strchr(s, '>')) {
			s = strchr(s, '>') + 1;
			continue;
		}
		for (e = 0; e < LENGTH(ent); e++)
			if (!strncmp(s, ent[e][0], strlen(ent[e][0])))
				break;
		if (e < LENGTH(ent)) {
			dst[i++] = ent[e][1][0];
			s += strlen(ent[e][0]);
		} else
			dst[i++] = *s == '\n' || *s == '\t' ? ' ' : *s, s++;
	}
	dst[i] = '\0';
}

static void
drawnotif(Notif *n)
{
	char buf[1024], *p;
	int cols = n->w / cw, i;

	XftDrawRect(n->draw, &col[ColBg], 0, 0, n->w, n->h);
	text(n->draw, ColWindow, 0, 0, "┌─[");
	snprintf(buf, sizeof buf, " %s ", n->app);
	text(n->draw, ColTitle, 3 * cw, 0, buf);
	p = buf + sprintf(buf, "]");
	for (i = ulen(n->app) + 6; i < cols - 1; i++)
		p += sprintf(p, "─");
	sprintf(p, "┐");
	text(n->draw, ColWindow, (ulen(n->app) + 5) * cw, 0, buf);
	for (i = 0; i < n->nlines; i++) {
		text(n->draw, ColWindow, 0, (i + 1) * ch, "│");
		text(n->draw, i ? ColWindow : ColTitle, 2 * cw, (i + 1) * ch, n->line[i]);
		text(n->draw, ColWindow, (cols - 1) * cw, (i + 1) * ch, "│");
	}
	p = buf + sprintf(buf, "└");
	for (i = 2; i < cols; i++)
		p += sprintf(p, "─");
	sprintf(p, "┘");
	text(n->draw, ColWindow, 0, (n->nlines + 1) * ch, buf);
}

/* stacked down from the top right corner, newest last */
static void
placenotifs(void)
{
	Notif *n;
	int y = 0;

	for (n = notifs; n; n = n->next) {
		XMoveResizeWindow(dpy, n->win, sw / cw * cw - n->w, y, n->w, n->h);
		XRaiseWindow(dpy, n->win);
		drawnotif(n);
		y += n->h;
	}
}

static void
closenotif(Notif *n, unsigned int why)
{
	Notif **pn;
	DBusMessage *m;
	dbus_uint32_t id = n->id, reason = why;

	for (pn = &notifs; *pn != n; pn = &(*pn)->next);
	*pn = n->next;
	XftDrawDestroy(n->draw);
	XDestroyWindow(dpy, n->win);
	free(n);
	placenotifs();
	if (bus && (m = dbus_message_new_signal("/org/freedesktop/Notifications",
	    "org.freedesktop.Notifications", "NotificationClosed"))) {
		dbus_message_append_args(m, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
		dbus_connection_send(bus, m, NULL);
		dbus_message_unref(m);
	}
}

static unsigned int
notify(const char *app, unsigned int replaces, const char *summary, const char *body, int timeout)
{
	XSetWindowAttributes wa;
	Notif *n, **pn;
	char buf[1024], word[256], *p, *w;
	int i, len;

	for (n = notifs; n && (!replaces || n->id != replaces); n = n->next);
	if (!n) {
		if (!(n = calloc(1, sizeof *n)))
			return 0;
		n->id = ++lastid;
		wa.override_redirect = True;
		wa.background_pixel = col[ColBg].pixel;
		wa.event_mask = ExposureMask|ButtonPressMask;
		n->win = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, CopyFromParent, CopyFromParent, CopyFromParent,
		                       CWOverrideRedirect|CWBackPixel|CWEventMask, &wa);
		n->draw = XftDrawCreate(dpy, n->win, DefaultVisual(dpy, DefaultScreen(dpy)),
		                        DefaultColormap(dpy, DefaultScreen(dpy)));
		for (pn = &notifs; *pn; pn = &(*pn)->next);
		*pn = n;
		XMapRaised(dpy, n->win);
	}
	ucut(n->app, *app ? app : "notification", NOTIFCOLS - 10, sizeof n->app);
	plain(buf, summary, sizeof buf);
	ucut(n->line[0], buf, NOTIFCOLS - 4, sizeof n->line[0]);
	n->nlines = 1;

	/* the body, a word at a time, in up to 3 lines under the summary */
	plain(buf, body, sizeof buf);
	for (p = buf; (w = strsep(&p, " "));) {
		if (!*w)
			continue;
		ucut(word, w, NOTIFCOLS - 4, sizeof word);
		i = n->nlines - 1;
		if (i > 0 && ulen(n->line[i]) + 1 + ulen(word) <= NOTIFCOLS - 4) {
			len = strlen(n->line[i]);
			snprintf(n->line[i] + len, sizeof n->line[i] - len, " %s", word);
		} else if (n->nlines < 4)
			snprintf(n->line[n->nlines++], sizeof n->line[0], "%s", word);
		else
			break;
	}
	n->w = 0;
	for (i = 0; i < n->nlines; i++)
		n->w = MAX(n->w, ulen(n->line[i]) + 4);
	n->w = MAX(n->w, ulen(n->app) + 10) * cw;
	n->h = (n->nlines + 2) * ch;
	/* -1: the usual 5 seconds. 0: until it's clicked */
	n->until = timeout == 0 ? 0 : time(NULL) + (timeout < 0 ? 5 : (timeout + 999) / 1000);
	placenotifs();
	return n->id;
}

static void
expirenotifs(void)
{
	Notif *n, *next;
	time_t now = time(NULL);

	for (n = notifs; n; n = next) {
		next = n->next;
		if (n->until && n->until <= now)
			closenotif(n, 1);
	}
}

static Notif *
wintonotif(Window w)
{
	Notif *n;

	for (n = notifs; n && n->win != w; n = n->next);
	return n;
}

static void
arg(DBusMessageIter *it, int type, void *v)
{
	if (dbus_message_iter_get_arg_type(it) == type)
		dbus_message_iter_get_basic(it, v);
	dbus_message_iter_next(it);
}

static void
dbusmessage(DBusMessage *m)
{
	static const char *iface = "org.freedesktop.Notifications";
	static const char *caps[] = { "body" }, **cp = caps;
	static const char *info[] = { "ttywm", "Chamesle", VERSION, "1.2" };
	const char *app = "", *icon = "", *summary = "", *body = "";
	dbus_uint32_t replaces = 0, id;
	dbus_int32_t timeout = -1;
	DBusMessageIter it;
	DBusMessage *r;
	Notif *n;

	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return;
	if (dbus_message_is_method_call(m, iface, "Notify")) {
		dbus_message_iter_init(m, &it);
		arg(&it, DBUS_TYPE_STRING, &app);
		arg(&it, DBUS_TYPE_UINT32, &replaces);
		arg(&it, DBUS_TYPE_STRING, &icon);
		arg(&it, DBUS_TYPE_STRING, &summary);
		arg(&it, DBUS_TYPE_STRING, &body);
		dbus_message_iter_next(&it);	/* actions: not here */
		dbus_message_iter_next(&it);	/* hints */
		arg(&it, DBUS_TYPE_INT32, &timeout);
		id = notify(app, replaces, summary, body, timeout);
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
	} else if (dbus_message_is_method_call(m, iface, "CloseNotification")) {
		dbus_message_iter_init(m, &it);
		id = 0;
		arg(&it, DBUS_TYPE_UINT32, &id);
		for (n = notifs; n && n->id != id; n = n->next);
		if (n)
			closenotif(n, 3);
		r = dbus_message_new_method_return(m);
	} else if (dbus_message_is_method_call(m, iface, "GetCapabilities")) {
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &cp, 1, DBUS_TYPE_INVALID);
	} else if (dbus_message_is_method_call(m, iface, "GetServerInformation")) {
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_STRING, &info[0], DBUS_TYPE_STRING, &info[1],
		                         DBUS_TYPE_STRING, &info[2], DBUS_TYPE_STRING, &info[3], DBUS_TYPE_INVALID);
	} else
		r = dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_METHOD, "ttywm only shows notifications");
	if (r) {
		dbus_connection_send(bus, r, NULL);
		dbus_message_unref(r);
	}
}

/* no session bus, or another program shows notifications already (like dunst): then not us */
static void
setupbus(void)
{
	DBusError err;

	dbus_error_init(&err);
	if (!(bus = dbus_bus_get(DBUS_BUS_SESSION, &err))) {
		dbus_error_free(&err);
		return;
	}
	dbus_connection_set_exit_on_disconnect(bus, FALSE);
	if (dbus_bus_request_name(bus, "org.freedesktop.Notifications", DBUS_NAME_FLAG_DO_NOT_QUEUE, &err)
	    != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		dbus_error_free(&err);
		bus = NULL;
	}
}

static void
readbus(void)
{
	DBusMessage *m;

	if (!bus)
		return;
	dbus_connection_read_write(bus, 0);
	while ((m = dbus_connection_pop_message(bus))) {
		dbusmessage(m);
		dbus_message_unref(m);
	}
	dbus_connection_flush(bus);
}

/* ---- windows ---- */

static Client *
topclient(void)
{
	Client *c;

	for (c = clients; c && c->scr != cur; c = c->next);
	return c;
}

/* focus(NULL) gives the keyboard to the shell */
static void
focus(Client *c)
{
	Client *old = sel;

	sel = c;
	if (old && old != c)
		drawframe(old);
	if (!c) {
		XSetInputFocus(dpy, shell[cur] ? shell[cur] : root, RevertToPointerRoot, CurrentTime);
		return;
	}
	XRaiseWindow(dpy, c->frame);
	XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
	drawframe(c);
}

static void
updatetitle(Client *c)
{
	XTextProperty tp;

	c->name[0] = '\0';
	if (!XGetTextProperty(dpy, c->win, &tp, netwmname) || !tp.nitems)
		if (!XGetTextProperty(dpy, c->win, &tp, XA_WM_NAME) || !tp.nitems)
			return;
	strncpy(c->name, (char *)tp.value, sizeof c->name - 1);
	XFree(tp.value);
}

static void
setclientstate(Window w, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, w, wmstate, wmstate, 32, PropModeReplace, (unsigned char *)data, 2);
}

/* sizes are whole character cells, like a terminal */
static void
resize(Client *c, int w, int h)
{
	w = MIN(MAX(w / cw, 10), (sw - c->x) / cw - 2) * cw;
	h = MIN(MAX(h / ch, 3), (wh - c->y) / ch - 2) * ch;
	if (w == c->w && h == c->h)
		return;
	c->w = w;
	c->h = h;
	XResizeWindow(dpy, c->frame, c->w + 2 * cw, c->h + 2 * ch);
	XResizeWindow(dpy, c->win, c->w, c->h);
	drawframe(c);
}

static void
move(Client *c, int x, int y)
{
	if (x == c->x && y == c->y)
		return;
	c->x = x;
	c->y = y;
	XMoveWindow(dpy, c->frame, x, y);
}

static void
spawnshell(int s)
{
	char cmd[64];

	newshell = s;
	snprintf(cmd, sizeof cmd, "exec %s -class %s", SHELLCMD, SHELLCLASS);
	run(cmd);
}

static void
manage(Window w, XWindowAttributes *wa)
{
	XClassHint hint = { NULL, NULL };
	Client *c;
	int isshell = 0;

	if (XGetClassHint(dpy, w, &hint)) {
		isshell = hint.res_class && !strcmp(hint.res_class, SHELLCLASS);
		XFree(hint.res_class);
		XFree(hint.res_name);
	}
	if (isshell) {
		int s = newshell >= 0 && !shell[newshell] ? newshell : cur;

		newshell = -1;
		shell[s] = w;
		XSelectInput(dpy, w, StructureNotifyMask);
		XGrabButton(dpy, AnyButton, AnyModifier, w, False, ButtonPressMask,
		            GrabModeSync, GrabModeAsync, None, None);
		XSetWindowBorderWidth(dpy, w, 0);
		XMoveResizeWindow(dpy, w, 0, 0, sw, wh);
		if (s == cur) {
			XMapWindow(dpy, w);
			XLowerWindow(dpy, w);
			if (!sel)
				focus(NULL);
		}
		return;
	}

	if (!(c = calloc(1, sizeof(Client))))
		die("calloc failed");
	c->win = w;
	c->scr = cur;
	c->x = c->y = -1;
	updatetitle(c);
	c->frame = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, col[ColBg].pixel);
	XSelectInput(dpy, c->frame, SubstructureRedirectMask|SubstructureNotifyMask|ExposureMask|ButtonPressMask);
	c->draw = XftDrawCreate(dpy, c->frame, DefaultVisual(dpy, DefaultScreen(dpy)),
	                        DefaultColormap(dpy, DefaultScreen(dpy)));
	move(c, MAX(0, (sw - wa->width) / 2 - cw) / cw * cw, MAX(0, (wh - wa->height) / 2 - ch) / ch * ch);

	/* the frame is the window's parent, so they move together and clicks on the border come to us.
	 * the save set gives the window back to root if we crash */
	XAddToSaveSet(dpy, w);
	XSetWindowBorderWidth(dpy, w, 0);
	if (wa->map_state == IsViewable)
		c->ignoreunmap++;	/* reparenting unmaps it once */
	XReparentWindow(dpy, w, c->frame, cw, ch);
	XSelectInput(dpy, w, PropertyChangeMask|StructureNotifyMask);
	XGrabButton(dpy, AnyButton, AnyModifier, w, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);

	c->next = clients;
	clients = c;
	resize(c, wa->width, wa->height);
	XMapWindow(dpy, w);
	XMapWindow(dpy, c->frame);
	setclientstate(w, NormalState);
	focus(c);
}

static void
unmanage(Client *c, int destroyed)
{
	Client **tc;

	for (tc = &clients; *tc != c; tc = &(*tc)->next);
	*tc = c->next;
	if (!destroyed) {
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		XReparentWindow(dpy, c->win, root, c->x + cw, c->y + ch);
		XRemoveFromSaveSet(dpy, c->win);
		setclientstate(c->win, WithdrawnState);
	}
	XftDrawDestroy(c->draw);
	XDestroyWindow(dpy, c->frame);
	if (sel == c) {
		sel = NULL;
		focus(topclient());
	}
	free(c);
}

/* ICCCM: ask with WM_DELETE_WINDOW if the window understands it, so it can save its work.
 * only kill it if it doesn't */
static void
closeclient(Client *c)
{
	Atom *p;
	int n, ok = 0;
	XEvent ev;

	if (XGetWMProtocols(dpy, c->win, &p, &n)) {
		while (!ok && n--)
			ok = p[n] == wmdelete;
		XFree(p);
	}
	if (!ok) {
		XKillClient(dpy, c->win);
		return;
	}
	memset(&ev, 0, sizeof ev);
	ev.type = ClientMessage;
	ev.xclient.window = c->win;
	ev.xclient.message_type = wmprotocols;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = wmdelete;
	ev.xclient.data.l[1] = CurrentTime;
	XSendEvent(dpy, c->win, False, NoEventMask, &ev);
}

static void
setfullscreen(Client *c, int full)
{
	if (full == c->isfull)
		return;
	c->isfull = full;
	if (full) {
		c->oldx = c->x; c->oldy = c->y; c->oldw = c->w; c->oldh = c->h;
		XMoveResizeWindow(dpy, c->frame, 0, 0, sw, sh);
		XMoveResizeWindow(dpy, c->win, 0, 0, sw, sh);
		XChangeProperty(dpy, c->win, netwmstate, XA_ATOM, 32, PropModeReplace,
		                (unsigned char *)&netfullscreen, 1);
	} else {
		XMoveWindow(dpy, c->win, cw, ch);
		c->x = c->y = -1;
		c->w = c->h = 0;
		move(c, c->oldx, c->oldy);
		resize(c, c->oldw, c->oldh);
		XChangeProperty(dpy, c->win, netwmstate, XA_ATOM, 32, PropModeReplace, NULL, 0);
	}
	XRaiseWindow(dpy, c->frame);
}

/* super+arrows: half the screen, all of it, or back where it was */
static void
snap(Client *c, int how)
{
	int cols = sw / cw, rows = wh / ch, half = cols / 2;

	if (c->isfull)
		return;
	if (how == ActRestore) {
		if (c->snapped) {
			move(c, c->sx, c->sy);
			resize(c, c->sw, c->sh);
		}
		c->snapped = 0;
		return;
	}
	if (!c->snapped) {
		c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
		c->snapped = 1;
	}
	if (how == ActLeft) {
		move(c, 0, 0);
		resize(c, (half - 2) * cw, (rows - 2) * ch);
	} else if (how == ActRight) {
		move(c, half * cw, 0);
		resize(c, (cols - half - 2) * cw, (rows - 2) * ch);
	} else {
		move(c, 0, 0);
		resize(c, (cols - 2) * cw, (rows - 2) * ch);
	}
}

static void
mousedrag(Client *c, int resizing)
{
	XEvent ev;
	Window dummy;
	int px, py, di, ox = c->x, oy = c->y, ow = c->w, oh = c->h;
	unsigned int dui;
	Time last = 0;

	if (c->isfull || XGrabPointer(dpy, root, False, ButtonReleaseMask|PointerMotionMask,
	    GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
		return;
	XQueryPointer(dpy, root, &dummy, &dummy, &px, &py, &di, &di, &dui);
	do {
		XMaskEvent(dpy, ButtonReleaseMask|PointerMotionMask, &ev);
		if (ev.type != MotionNotify)
			continue;
		/* only the newest position, and no more than 60 times a second */
		while (XCheckTypedEvent(dpy, MotionNotify, &ev));
		if (ev.xmotion.time - last < 16)
			continue;
		last = ev.xmotion.time;
		if (resizing)
			resize(c, ow + ev.xmotion.x_root - px, oh + ev.xmotion.y_root - py);
		else
			move(c, ox + (ev.xmotion.x_root - px) / cw * cw, oy + (ev.xmotion.y_root - py) / ch * ch);
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
}

/* ---- the lock screen: a login prompt, checked by pam like a real one ---- */

static int
conv(int n, const struct pam_message **msg, struct pam_response **resp, void *pw)
{
	int i;

	if (!(*resp = calloc(n, sizeof **resp)))
		return PAM_BUF_ERR;
	for (i = 0; i < n; i++)
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
			(*resp)[i].resp = strdup(pw);
	return PAM_SUCCESS;
}

static int
checkpassword(const char *user, const char *pw)
{
	struct pam_conv pc = { conv, (void *)pw };
	pam_handle_t *ph;
	int r;

	if (pam_start("system-local-login", user, &pc, &ph) != PAM_SUCCESS)
		return 0;
	r = pam_authenticate(ph, 0);
	pam_end(ph, r);
	return r == PAM_SUCCESS;
}

static void
lock(void)
{
	XSetWindowAttributes wa;
	XftDraw *d;
	XEvent ev;
	KeySym sym;
	char host[64] = "", line[256], buf[8], pw[256] = "", *user = getenv("USER");
	int n = 0, wrong = 0, i, len;
	Window w;

	gethostname(host, sizeof host);
	wa.override_redirect = True;
	wa.background_pixel = col[ColBg].pixel;
	wa.event_mask = ExposureMask|KeyPressMask;
	w = XCreateWindow(dpy, root, 0, 0, sw, sh, 0, CopyFromParent, CopyFromParent, CopyFromParent,
	                         CWOverrideRedirect|CWBackPixel|CWEventMask, &wa);
	d = XftDrawCreate(dpy, w, DefaultVisual(dpy, DefaultScreen(dpy)), DefaultColormap(dpy, DefaultScreen(dpy)));
	XMapRaised(dpy, w);
	/* the keyboard can be busy for a moment, like when the key that locked is still down */
	for (i = 0; i < 100; i++) {
		if (XGrabKeyboard(dpy, w, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
			break;
		usleep(10000);
	}
	XGrabPointer(dpy, w, False, 0, GrabModeAsync, GrabModeAsync, w, None, CurrentTime);
	for (;;) {
		XftDrawRect(d, &col[ColBg], 0, 0, sw, sh);
		snprintf(line, sizeof line, "Chamesle Linux (tty%d)", cur + 1);
		text(d, ColBar, 0, 0, line);
		snprintf(line, sizeof line, "%s login: %s", host, user ? user : "");
		text(d, ColBar, 0, 2 * ch, line);
		text(d, ColBar, 0, 3 * ch, "Password: ");
		if (wrong)
			text(d, ColBar, 0, 5 * ch, "Login incorrect");
		XRaiseWindow(dpy, w);
		XNextEvent(dpy, &ev);
		if (ev.type == Expose || ev.xany.window != w) {
			/* the rest of the world goes on underneath */
			if (ev.xany.window != w && handler[ev.type] && ev.type != KeyPress && ev.type != ButtonPress)
				handler[ev.type](&ev);
			continue;
		}
		if (ev.type != KeyPress)
			continue;
		len = XLookupString(&ev.xkey, buf, sizeof buf, &sym, NULL);
		if (sym == XK_Return || sym == XK_KP_Enter) {
			pw[n] = '\0';
			if (user && checkpassword(user, pw))
				break;
			wrong = 1;
			n = 0;
		} else if (sym == XK_BackSpace && n)
			n--;
		else if (sym == XK_Escape)
			n = 0;
		else if (len > 0 && !iscntrl((unsigned char)buf[0]) && n + len < (int)sizeof pw - 1) {
			memcpy(pw + n, buf, len);
			n += len;
		}
	}
	memset(pw, 0, sizeof pw);
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XftDrawDestroy(d);
	XDestroyWindow(dpy, w);
	focus(sel);
}

/* ---- what keys and clicks do ---- */

static void
view(int n)
{
	Client *c;

	if (n == cur)
		return;
	for (c = clients; c; c = c->next)
		if (c->scr == cur)
			XUnmapWindow(dpy, c->frame);
	if (shell[cur])
		XUnmapWindow(dpy, shell[cur]);
	cur = n;
	sel = NULL;
	if (shell[cur])
		XMapWindow(dpy, shell[cur]);
	else
		spawnshell(cur);
	for (c = clients; c; c = c->next)
		if (c->scr == cur)
			XMapWindow(dpy, c->frame);
	if (shell[cur])
		XLowerWindow(dpy, shell[cur]);
	drawbar();
	focus(topclient());
}

static void
sendto(int n)
{
	if (!sel || n == cur)
		return;
	sel->scr = n;
	XUnmapWindow(dpy, sel->frame);
	sel = NULL;
	focus(topclient());
}

static void
focusnext(void)
{
	Client *c = sel ? sel->next : clients;

	for (; c && c->scr != cur; c = c->next);
	focus(c);
}

static void
doaction(Key *k)
{
	switch (k->act) {
	case ActRun:      run(k->cmd); break;
	case ActTerminal: run(terminal); break;
	case ActMenu:     run(menu); break;
	case ActClose:    if (sel) closeclient(sel); break;
	case ActNext:     focusnext(); break;
	case ActFull:     if (sel) setfullscreen(sel, !sel->isfull); break;
	case ActQuit:     running = 0; break;
	case ActView:     view(k->n); break;
	case ActSend:     sendto(k->n); break;
	case ActVolUp:    setvolume("5%+"); break;
	case ActVolDown:  setvolume("5%-"); break;
	case ActMute:     setvolume("mute"); break;
	case ActLightUp:  setlight(5); drawbar(); break;
	case ActLightDown: setlight(-5); drawbar(); break;
	case ActLeft: case ActRight: case ActMax: case ActRestore:
		if (sel)
			snap(sel, k->act);
		break;
	case ActLock:     lock(); break;
	}
}

static void
barclick(XButtonPressedEvent *ev)
{
	char cmd[512];
	int i;

	for (i = 0; i < nsegs; i++) {
		if (ev->x < segs[i].x0 || ev->x >= segs[i].x1)
			continue;
		if (segs[i].seg == SegVol) {
			if (ev->button == Button4)
				setvolume("5%+");
			else if (ev->button == Button5)
				setvolume("5%-");
			else if (ev->button == Button1)
				setvolume("mute");
		} else if (segs[i].seg == SegLight && (ev->button == Button4 || ev->button == Button5)) {
			setlight(ev->button == Button4 ? 5 : -5);
			drawbar();
		} else if (segs[i].seg == SegNet && ev->button == Button1) {
			snprintf(cmd, sizeof cmd, "exec %s -T network -e nmtui", SHELLCMD);
			run(cmd);
		}
	}
}

/* ---- events ---- */

static void
buttonpress(XEvent *e)
{
	XButtonPressedEvent *ev = &e->xbutton;
	Client *c;
	Notif *n;
	int cols;

	if (ev->window == bar) {
		barclick(ev);
		return;
	}
	if ((n = wintonotif(ev->window))) {
		closenotif(n, 2);
		return;
	}
	if (!(c = wintoclient(ev->window))) {
		if (shellof(ev->window) >= 0 && sel)
			focus(NULL);
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		return;
	}
	focus(c);
	if (ev->window == c->win) {
		if (CLEANMASK(ev->state) == Mod4Mask && (ev->button == Button1 || ev->button == Button3)) {
			XAllowEvents(dpy, AsyncPointer, CurrentTime);
			mousedrag(c, ev->button == Button3);
		} else
			XAllowEvents(dpy, ReplayPointer, CurrentTime);
		return;
	}
	cols = c->w / cw + 2;
	if (ev->y < ch && ev->x >= (cols - 4) * cw)
		closeclient(c);
	else if (ev->y < ch)
		mousedrag(c, 0);
	else if (ev->y >= c->h + ch && ev->x >= (cols - 2) * cw)
		mousedrag(c, 1);
}

static void
clientmessage(XEvent *e)
{
	XClientMessageEvent *cme = &e->xclient;
	Client *c = wintoclient(cme->window);

	if (c && cme->message_type == netwmstate
	&& ((Atom)cme->data.l[1] == netfullscreen || (Atom)cme->data.l[2] == netfullscreen))
		setfullscreen(c, cme->data.l[0] == 1 || (cme->data.l[0] == 2 && !c->isfull));
}

static void
configurerequest(XEvent *e)
{
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;
	Client *c;

	if (shellof(ev->window) >= 0)
		return;
	if ((c = wintoclient(ev->window))) {
		if (!c->isfull)
			resize(c, ev->value_mask & CWWidth ? ev->width : c->w,
			          ev->value_mask & CWHeight ? ev->height : c->h);
		return;
	}
	wc.x = ev->x;
	wc.y = ev->y;
	wc.width = ev->width;
	wc.height = ev->height;
	wc.border_width = ev->border_width;
	wc.sibling = ev->above;
	wc.stack_mode = ev->detail;
	XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
}

static void
destroynotify(XEvent *e)
{
	Window w = e->xdestroywindow.window;
	Client *c;
	int s;

	if ((c = wintoclient(w)) && c->win == w)
		unmanage(c, 1);
	else if ((s = shellof(w)) >= 0) {
		/* respawn, like a getty */
		shell[s] = 0;
		if (s == cur && running)
			spawnshell(s);
	}
}

static void
expose(XEvent *e)
{
	Client *c;

	if (e->xexpose.count)
		return;
	if (e->xexpose.window == bar)
		drawbar();
	else if (wintonotif(e->xexpose.window))
		drawnotif(wintonotif(e->xexpose.window));
	else if ((c = wintoclient(e->xexpose.window)))
		drawframe(c);
}

static void
keypress(XEvent *e)
{
	KeySym keysym = XkbKeycodeToKeysym(dpy, (KeyCode)e->xkey.keycode, 0, 0);
	int i;

	for (i = 0; i < nkeys; i++)
		if (keysym == keys[i].keysym && CLEANMASK(keys[i].mod) == CLEANMASK(e->xkey.state))
			doaction(&keys[i]);
}

static void
maprequest(XEvent *e)
{
	XWindowAttributes wa;
	Window w = e->xmaprequest.window;

	if (XGetWindowAttributes(dpy, w, &wa) && !wa.override_redirect && !wintoclient(w) && shellof(w) < 0)
		manage(w, &wa);
}

static void
propertynotify(XEvent *e)
{
	Client *c;

	if ((c = wintoclient(e->xproperty.window))
	&& (e->xproperty.atom == XA_WM_NAME || e->xproperty.atom == netwmname)) {
		updatetitle(c);
		drawframe(c);
	}
}

static void
unmapnotify(XEvent *e)
{
	Client *c;

	if (!(c = wintoclient(e->xunmap.window)) || c->win != e->xunmap.window)
		return;
	if (c->ignoreunmap)
		c->ignoreunmap--;
	else
		unmanage(c, 0);
}

static void (*handler[LASTEvent])(XEvent *) = {
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureRequest] = configurerequest,
	[DestroyNotify] = destroynotify,
	[Expose] = expose,
	[KeyPress] = keypress,
	[MapRequest] = maprequest,
	[PropertyNotify] = propertynotify,
	[UnmapNotify] = unmapnotify
};

/* once a second: the clock, the battery, and the settings file if it changed */
static void
tick(void)
{
	static time_t last, lastvol;
	time_t now = time(NULL);
	struct stat st;
	Client *c;

	if (now == last)
		return;
	expirenotifs();
	if (stat(cfgpath, &st) == 0 && st.st_mtime != cfgmtime) {
		loadconfig(0);
		grabkeys();
		loadcolours();
		for (c = clients; c; c = c->next)
			drawframe(c);
		last = 0;
	}
	if (now - lastvol >= 10) {
		readvolume();
		lastvol = now;
	}
	if (now / 5 != last / 5)
		drawbar();
	last = now;
}

static void
setup(void)
{
	XSetWindowAttributes wa;
	int screen = DefaultScreen(dpy);
	char font[128], *home, *xdg;

	signal(SIGCHLD, SIG_IGN);	/* no zombies */
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	root = RootWindow(dpy, screen);

	if ((xdg = getenv("XDG_CONFIG_HOME")) && *xdg)
		snprintf(cfgpath, sizeof cfgpath, "%s", xdg);
	else
		snprintf(cfgpath, sizeof cfgpath, "%s/.config", (home = getenv("HOME")) ? home : ".");
	mkdir(cfgpath, 0755);
	strncat(cfgpath, "/ttywm", sizeof cfgpath - strlen(cfgpath) - 1);
	mkdir(cfgpath, 0755);
	strncat(cfgpath, "/config", sizeof cfgpath - strlen(cfgpath) - 1);
	loadconfig(1);

	setenv("TTYWM_SIZE", big ? "32" : "16", 1);
	snprintf(font, sizeof font, "VGA:pixelsize=%d:antialias=false:hinting=false", big ? 32 : 16);
	if (!(xfont = XftFontOpenName(dpy, screen, font)))
		die("cannot load font");
	cw = xfont->max_advance_width;
	ch = xfont->ascent + xfont->descent;
	wh = sh - ch;	/* the last line is the bottom line */
	loadcolours();

	wmprotocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmdelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmstate = XInternAtom(dpy, "WM_STATE", False);
	netwmname = XInternAtom(dpy, "_NET_WM_NAME", False);
	netwmstate = XInternAtom(dpy, "_NET_WM_STATE", False);
	netfullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	netwmcheck = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);

	/* EWMH check window, so programs can tell which wm is running */
	wmcheck = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheck, netwmcheck, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wmcheck, 1);
	XChangeProperty(dpy, wmcheck, netwmname, utf8string, 8, PropModeReplace, (unsigned char *)"ttywm", 5);
	XChangeProperty(dpy, root, netwmcheck, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wmcheck, 1);

	wa.override_redirect = True;
	wa.background_pixel = col[ColBg].pixel;
	wa.event_mask = ExposureMask|ButtonPressMask;
	bar = XCreateWindow(dpy, root, 0, wh, sw, ch, 0, CopyFromParent, CopyFromParent,
	                    CopyFromParent, CWOverrideRedirect|CWBackPixel|CWEventMask, &wa);
	bardraw = XftDrawCreate(dpy, bar, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));
	XMapWindow(dpy, bar);

	XSetWindowBackground(dpy, root, col[ColBg].pixel);
	XClearWindow(dpy, root);
	XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
	XSelectInput(dpy, root, SubstructureRedirectMask|SubstructureNotifyMask);
	grabkeys();
	readvolume();
	findbacklight();
	setupbus();
}

int
main(int argc, char *argv[])
{
	XEvent ev;
	fd_set fds;
	struct timeval tv;
	int fd, bfd = -1;

	if (argc == 2 && !strcmp("-v", argv[1]))
		die("ttywm-" VERSION);
	else if (argc != 1)
		die("usage: ttywm [-v]");
	setlocale(LC_CTYPE, "");
	/* no shell means a black screen you can't leave: better to go back to the login screen */
	if (!inpath(SHELLCMD))
		die("cannot find " SHELLCMD);
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");

	/* only one client can select SubstructureRedirect on root */
	XSetErrorHandler(xerrorstart);
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);

	setup();
	spawnshell(cur);
	drawbar();

	/* events, notifications, and a tick every second for the bottom line */
	fd = ConnectionNumber(dpy);
	if (bus)
		dbus_connection_get_unix_fd(bus, &bfd);
	while (running) {
		if (!XPending(dpy)) {
			FD_ZERO(&fds);
			FD_SET(fd, &fds);
			if (bfd >= 0)
				FD_SET(bfd, &fds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			if (select(MAX(fd, bfd) + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR)
				break;
		}
		readbus();
		while (running && XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (handler[ev.type])
				handler[ev.type](&ev);
		}
		tick();
	}

	while (clients)
		unmanage(clients, 0);
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}
