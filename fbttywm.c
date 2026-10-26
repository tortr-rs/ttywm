/* fbttywm - ttywm on the framebuffer. the console with floating windows, no X.
 * every screen is a full screen shell, windows float on top in frames drawn with
 * the console's own font, and every window is a terminal of ours.
 *
 *   gcc -static -O2 fbttywm.c -o /fbttywm
 *
 * super+enter  new window          super+q      close it
 * super+tab    next window         super+f      full screen
 * super+1..9   tty1 to tty9        super+shift+1..9  send the window there
 * super+shift+e  quit
 * drag a title, or super+left drag to move. super+right drag to resize. click [x] to close */
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#define NSCR    9
#define MAXDEV  32
#define LEN(a)  (sizeof(a) / sizeof(a)[0])
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum { Normal, Esc, Csi, Osc, OscEsc, Charset };

typedef struct {
	unsigned c;
	unsigned char fg, bg;
} Cell;

typedef struct {
	int fd, pid, cols, rows;
	Cell *cell, *alt;
	int isalt, x, y, sx, sy, top, bot, wrapnext, hide;
	int fg, bg, bold, rev;
	int st, par[16], np, priv;
	unsigned utf;
	int need;
	char osc[128], title[64];
	int nosc;
} Term;

typedef struct Win Win;
struct Win {
	Term t;
	int x, y, w, h, scr, full, ox, oy, ow, oh;
	Win *next;
};

typedef struct {
	int fd, kbd, ax[2], ay[2];
} Dev;

/* the vga colours, in the order escape codes count them */
static const unsigned pal[16] = {
	0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
	0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

/* us keyboard, by evdev key code */
static const char keys[] =
	"\0" "\033" "1234567890-=" "\177" "\t" "qwertyuiop[]" "\r" "\0" "asdfghjkl;'`" "\0" "\\" "zxcvbnm,./" "\0" "*" "\0" " ";
static const char shiftkeys[] =
	"\0" "\033" "!@#$%^&*()_+" "\177" "\t" "QWERTYUIOP{}" "\r" "\0" "ASDFGHJKL:\"~" "\0" "|" "ZXCVBNM<>?" "\0" "*" "\0" " ";

static unsigned char font[512 * 32 * 4];
static unsigned short uni[65536];
static int fw, fh, fpitch;

static unsigned char *fb;
static int fbw, fbh, fbline, fbbpp;
static struct fb_var_screeninfo var;

static int cols, rows, cur, tty = -1, running = 1, dirty = 1;
static Cell *scr, *old;
static Term shell[NSCR];
static Win *wins, *sel;
static Dev dev[MAXDEV];
static int ndev;
static int mx, my, shift, ctrl, alt, super, caps;
static Win *drag;
static int dragrs, dragx, dragy;
static char *sh;

static void
die(const char *s)
{
	if (tty >= 0)
		ioctl(tty, KDSETMODE, KD_TEXT);
	fprintf(stderr, "fbttywm: %s: %s\n", s, strerror(errno));
	exit(1);
}

/* ---- the font: the one the console uses, from the kernel ---- */
static void
getfont(void)
{
	struct console_font_op op = { KD_FONT_OP_GET, 0, 32, 32, 512, font };
	struct unipair *u;
	struct unimapdesc d;
	int i;

	if (ioctl(tty, KDFONTOP, &op) < 0)
		die("can't read the console's font");
	fw = op.width, fh = op.height, fpitch = (fw + 7) / 8;
	memset(uni, 0xff, sizeof uni);
	u = calloc(16384, sizeof *u);
	d.entry_ct = 16384, d.entries = u;
	if (ioctl(tty, GIO_UNIMAP, &d) == 0) {
		for (i = 0; i < d.entry_ct; i++)
			if (uni[u[i].unicode] == 0xffff)
				uni[u[i].unicode] = u[i].fontpos;
	} else {
		for (i = 32; i < 127; i++)
			uni[i] = i;
	}
	free(u);
}

static int
glyph(unsigned c)
{
	if (c < 65536 && uni[c] != 0xffff)
		return uni[c];
	return uni['?'] != 0xffff ? uni['?'] : '?';
}

/* ---- the screen ---- */
static unsigned
pixel(int i)
{
	unsigned c = pal[i], r = c >> 16, g = c >> 8 & 255, b = c & 255;

	r >>= 8 - var.red.length, g >>= 8 - var.green.length, b >>= 8 - var.blue.length;
	return r << var.red.offset | g << var.green.offset | b << var.blue.offset;
}

static void
drawcell(int x, int y, Cell *c)
{
	unsigned char *g = font + glyph(c->c) * 32 * fpitch;
	unsigned f = pixel(c->fg), b = pixel(c->bg);
	int i, j;

	for (i = 0; i < fh; i++) {
		unsigned char *p = fb + (y * fh + i) * fbline + x * fw * (fbbpp / 8);
		for (j = 0; j < fw; j++) {
			unsigned v = g[i * fpitch + j / 8] & 0x80 >> (j % 8) ? f : b;
			if (fbbpp == 32)
				((unsigned *)p)[j] = v;
			else
				((unsigned short *)p)[j] = v;
		}
	}
}

static void
openfb(void)
{
	struct fb_fix_screeninfo fix;
	int fd = open("/dev/fb0", O_RDWR);

	if (fd < 0)
		die("/dev/fb0");
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0)
		die("/dev/fb0");
	fbw = var.xres, fbh = var.yres, fbline = fix.line_length, fbbpp = var.bits_per_pixel;
	if (fbbpp != 32 && fbbpp != 16) {
		errno = EINVAL;
		die("only 16 and 32 bit framebuffers");
	}
	fb = mmap(NULL, fbline * fbh, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		die("mmap /dev/fb0");
	cols = fbw / fw, rows = fbh / fh;
	scr = calloc(cols * rows, sizeof *scr);
	old = calloc(cols * rows, sizeof *old);
	memset(old, 0xff, cols * rows * sizeof *old);
	memset(fb, 0, fbline * fbh);
}

/* ---- terminals ---- */
static void
clearcells(Term *t, int from, int to)
{
	for (; from < to; from++)
		t->cell[from] = (Cell){ ' ', 7, t->bg };
}

static void
tsize(Term *t)
{
	struct winsize ws = { t->rows, t->cols, t->cols * fw, t->rows * fh };

	ioctl(t->fd, TIOCSWINSZ, &ws);
}

static void
tnew(Term *t, int c, int r)
{
	char *pts;
	int fd;

	memset(t, 0, sizeof *t);
	t->cols = c, t->rows = r, t->bot = r - 1, t->fg = 7;
	t->cell = calloc(c * r, sizeof(Cell));
	t->alt = calloc(c * r, sizeof(Cell));
	clearcells(t, 0, c * r);
	snprintf(t->title, sizeof t->title, "%s", strrchr(sh, '/') ? strrchr(sh, '/') + 1 : sh);
	if ((t->fd = posix_openpt(O_RDWR | O_NOCTTY)) < 0 || grantpt(t->fd) < 0 || unlockpt(t->fd) < 0)
		die("a new terminal");
	pts = ptsname(t->fd);
	tsize(t);
	if ((t->pid = fork()) == 0) {
		setsid();
		if ((fd = open(pts, O_RDWR)) < 0)
			_exit(1);
		ioctl(fd, TIOCSCTTY, 0);
		dup2(fd, 0), dup2(fd, 1), dup2(fd, 2);
		if (fd > 2)
			close(fd);
		setenv("TERM", "linux", 1);
		execl(sh, sh, (char *)NULL);
		_exit(127);
	}
	fcntl(t->fd, F_SETFL, O_NONBLOCK);
}

static void
tfree(Term *t)
{
	if (t->pid > 0)
		kill(t->pid, SIGHUP);
	if (t->fd > 0)
		close(t->fd);
	free(t->cell), free(t->alt);
	t->pid = 0, t->fd = 0, t->cell = t->alt = NULL;
}

static void
tresize(Term *t, int c, int r)
{
	Cell *n[2];
	Cell *o[2] = { t->cell, t->alt };
	int i, y, x;

	if (c == t->cols && r == t->rows)
		return;
	for (i = 0; i < 2; i++) {
		n[i] = calloc(c * r, sizeof(Cell));
		for (y = 0; y < r; y++)
			for (x = 0; x < c; x++)
				n[i][y * c + x] = y < t->rows && x < t->cols ? o[i][y * t->cols + x] : (Cell){ ' ', 7, 0 };
		free(o[i]);
	}
	t->cell = n[0], t->alt = n[1];
	t->cols = c, t->rows = r, t->top = 0, t->bot = r - 1;
	t->x = MIN(t->x, c - 1), t->y = MIN(t->y, r - 1);
	t->wrapnext = 0;
	tsize(t);
}

static void
scrollup(Term *t, int top, int bot, int n)
{
	n = MIN(n, bot - top + 1);
	memmove(t->cell + top * t->cols, t->cell + (top + n) * t->cols, (bot - top + 1 - n) * t->cols * sizeof(Cell));
	clearcells(t, (bot - n + 1) * t->cols, (bot + 1) * t->cols);
}

static void
scrolldown(Term *t, int top, int bot, int n)
{
	n = MIN(n, bot - top + 1);
	memmove(t->cell + (top + n) * t->cols, t->cell + top * t->cols, (bot - top + 1 - n) * t->cols * sizeof(Cell));
	clearcells(t, top * t->cols, (top + n) * t->cols);
}

static void
newline(Term *t)
{
	if (t->y == t->bot)
		scrollup(t, t->top, t->bot, 1);
	else if (t->y < t->rows - 1)
		t->y++;
}

static void
moveto(Term *t, int x, int y)
{
	t->x = MAX(0, MIN(x, t->cols - 1));
	t->y = MAX(0, MIN(y, t->rows - 1));
	t->wrapnext = 0;
}

static void
reply(Term *t, const char *s)
{
	write(t->fd, s, strlen(s));
}

static void
tputc(Term *t, unsigned c)
{
	Cell *p;
	int f = t->fg + (t->bold && t->fg < 8 ? 8 : 0);

	if (t->wrapnext) {
		t->x = 0, t->wrapnext = 0;
		newline(t);
	}
	p = &t->cell[t->y * t->cols + t->x];
	p->c = c;
	p->fg = t->rev ? t->bg : f;
	p->bg = t->rev ? f : t->bg;
	if (t->x == t->cols - 1)
		t->wrapnext = 1;
	else
		t->x++;
}

static void
altscreen(Term *t, int on)
{
	Cell *c;

	if (on == t->isalt)
		return;
	c = t->cell, t->cell = t->alt, t->alt = c;
	t->isalt = on;
	if (on)
		clearcells(t, 0, t->cols * t->rows);
}

static void
sgr(Term *t)
{
	int i, p;

	for (i = 0; i < t->np; i++) {
		p = t->par[i];
		if (p == 0)
			t->fg = 7, t->bg = 0, t->bold = t->rev = 0;
		else if (p == 1)
			t->bold = 1;
		else if (p == 22)
			t->bold = 0;
		else if (p == 7)
			t->rev = 1;
		else if (p == 27)
			t->rev = 0;
		else if (p >= 30 && p <= 37)
			t->fg = p - 30;
		else if (p == 39)
			t->fg = 7;
		else if (p >= 40 && p <= 47)
			t->bg = p - 40;
		else if (p == 49)
			t->bg = 0;
		else if (p >= 90 && p <= 97)
			t->fg = p - 90 + 8;
		else if (p >= 100 && p <= 107)
			t->bg = p - 100 + 8;
		else if ((p == 38 || p == 48) && i + 2 < t->np && t->par[i + 1] == 5) {
			if (t->par[i + 2] < 16)
				*(p == 38 ? &t->fg : &t->bg) = t->par[i + 2];
			i += 2;
		} else if ((p == 38 || p == 48) && t->par[i + 1] == 2)
			i += 4;
	}
}

static void
csi(Term *t, int f)
{
	int n = t->par[0] ? t->par[0] : 1, i, at = t->y * t->cols + t->x;
	char s[32];

	switch (f) {
	case '@':
		n = MIN(n, t->cols - t->x);
		memmove(t->cell + at + n, t->cell + at, (t->cols - t->x - n) * sizeof(Cell));
		clearcells(t, at, at + n);
		break;
	case 'A': moveto(t, t->x, t->y - n); break;
	case 'B': case 'e': moveto(t, t->x, t->y + n); break;
	case 'C': case 'a': moveto(t, t->x + n, t->y); break;
	case 'D': moveto(t, t->x - n, t->y); break;
	case 'E': moveto(t, 0, t->y + n); break;
	case 'F': moveto(t, 0, t->y - n); break;
	case 'G': case '`': moveto(t, n - 1, t->y); break;
	case 'd': moveto(t, t->x, n - 1); break;
	case 'H': case 'f':
		moveto(t, (t->np > 1 && t->par[1] ? t->par[1] : 1) - 1, n - 1);
		break;
	case 'J':
		if (t->par[0] == 0)
			clearcells(t, at, t->cols * t->rows);
		else if (t->par[0] == 1)
			clearcells(t, 0, at + 1);
		else
			clearcells(t, 0, t->cols * t->rows);
		break;
	case 'K':
		if (t->par[0] == 0)
			clearcells(t, at, (t->y + 1) * t->cols);
		else if (t->par[0] == 1)
			clearcells(t, t->y * t->cols, at + 1);
		else
			clearcells(t, t->y * t->cols, (t->y + 1) * t->cols);
		break;
	case 'L':
		if (t->y >= t->top && t->y <= t->bot)
			scrolldown(t, t->y, t->bot, n);
		break;
	case 'M':
		if (t->y >= t->top && t->y <= t->bot)
			scrollup(t, t->y, t->bot, n);
		break;
	case 'P':
		n = MIN(n, t->cols - t->x);
		memmove(t->cell + at, t->cell + at + n, (t->cols - t->x - n) * sizeof(Cell));
		clearcells(t, (t->y + 1) * t->cols - n, (t->y + 1) * t->cols);
		break;
	case 'X':
		clearcells(t, at, at + MIN(n, t->cols - t->x));
		break;
	case 'm':
		sgr(t);
		break;
	case 'r':
		t->top = (t->par[0] ? t->par[0] : 1) - 1;
		t->bot = (t->np > 1 && t->par[1] ? t->par[1] : t->rows) - 1;
		if (t->top >= t->bot || t->bot >= t->rows)
			t->top = 0, t->bot = t->rows - 1;
		moveto(t, 0, 0);
		break;
	case 's': t->sx = t->x, t->sy = t->y; break;
	case 'u': moveto(t, t->sx, t->sy); break;
	case 'h': case 'l':
		for (i = 0; t->priv && i < t->np; i++) {
			if (t->par[i] == 25)
				t->hide = f == 'l';
			else if (t->par[i] == 1049 || t->par[i] == 47 || t->par[i] == 1047) {
				if (f == 'h' && t->par[i] == 1049)
					t->sx = t->x, t->sy = t->y;
				altscreen(t, f == 'h');
				if (f == 'l' && t->par[i] == 1049)
					moveto(t, t->sx, t->sy);
			}
		}
		break;
	case 'n':
		if (t->par[0] == 6) {
			snprintf(s, sizeof s, "\033[%d;%dR", t->y + 1, t->x + 1);
			reply(t, s);
		}
		break;
	case 'c':
		reply(t, "\033[?6c");
		break;
	}
}

static void
esc(Term *t, int c)
{
	t->st = Normal;
	switch (c) {
	case '[':
		t->st = Csi, t->np = 0, t->priv = 0;
		memset(t->par, 0, sizeof t->par);
		break;
	case ']': t->st = Osc, t->nosc = 0; break;
	case '(': case ')': t->st = Charset; break;
	case '7': t->sx = t->x, t->sy = t->y; break;
	case '8': moveto(t, t->sx, t->sy); break;
	case 'D': newline(t); break;
	case 'E': t->x = 0, newline(t); break;
	case 'M':
		if (t->y == t->top)
			scrolldown(t, t->top, t->bot, 1);
		else if (t->y > 0)
			t->y--;
		break;
	case 'c':
		t->fg = 7, t->bg = 0, t->bold = t->rev = t->hide = 0;
		t->top = 0, t->bot = t->rows - 1;
		altscreen(t, 0);
		clearcells(t, 0, t->cols * t->rows);
		moveto(t, 0, 0);
		break;
	}
}

/* a title from \033]0;title\007 or \033]2;title\007 */
static void
oscdone(Term *t)
{
	t->osc[t->nosc] = 0;
	if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';')
		snprintf(t->title, sizeof t->title, "%s", t->osc + 2);
	t->st = Normal;
}

static void
tfeed(Term *t, unsigned char *s, int n)
{
	int b;

	for (; n-- > 0; s++) {
		b = *s;
		if (t->st == Osc) {
			if (b == 7)
				oscdone(t);
			else if (b == 033)
				t->st = OscEsc;
			else if (t->nosc < (int)sizeof t->osc - 1)
				t->osc[t->nosc++] = b;
			continue;
		}
		if (t->st == OscEsc) {
			oscdone(t);
			continue;
		}
		if (t->st == Charset) {
			t->st = Normal;
			continue;
		}
		if (b < 32 || b == 127) {
			switch (b) {
			case '\b': moveto(t, t->x - 1, t->y); break;
			case '\t': moveto(t, (t->x / 8 + 1) * 8, t->y); break;
			case '\n': case '\v': case '\f': t->wrapnext = 0, newline(t); break;
			case '\r': t->x = 0, t->wrapnext = 0; break;
			case 033: t->st = Esc; break;
			case 030: case 032: t->st = Normal; break;
			}
			continue;
		}
		if (t->st == Esc) {
			esc(t, b);
			continue;
		}
		if (t->st == Csi) {
			if (b == '?' || b == '>')
				t->priv = 1;
			else if (b >= '0' && b <= '9')
				t->par[t->np] = MIN(t->par[t->np] * 10 + b - '0', 9999);
			else if (b == ';') {
				if (t->np < (int)LEN(t->par) - 1)
					t->np++;
			} else if (b >= 0x40 && b <= 0x7e) {
				t->np++;
				t->st = Normal;
				csi(t, b);
			}
			continue;
		}
		/* utf-8 */
		if (b < 0x80)
			tputc(t, b), t->need = 0;
		else if ((b & 0xc0) == 0x80 && t->need) {
			t->utf = t->utf << 6 | (b & 0x3f);
			if (--t->need == 0)
				tputc(t, t->utf);
		} else if ((b & 0xe0) == 0xc0)
			t->utf = b & 0x1f, t->need = 1;
		else if ((b & 0xf0) == 0xe0)
			t->utf = b & 0x0f, t->need = 2;
		else if ((b & 0xf8) == 0xf0)
			t->utf = b & 0x07, t->need = 3;
		else
			tputc(t, '?'), t->need = 0;
	}
}

/* ---- windows ---- */
static void
inner(Win *w, int *x, int *y, int *c, int *r)
{
	int f = !w->full;

	*x = w->x + f, *y = w->y + f, *c = w->w - 2 * f, *r = w->h - 2 * f;
}

static Term *
focused(void)
{
	return sel ? &sel->t : &shell[cur];
}

static void
raise_(Win *w)
{
	Win **p;

	for (p = &wins; *p; p = &(*p)->next)
		if (*p == w) {
			*p = w->next;
			break;
		}
	for (p = &wins; *p; p = &(*p)->next)
		;
	*p = w, w->next = NULL;
	sel = w;
}

static Win *
topwin(void)
{
	Win *w, *top = NULL;

	for (w = wins; w; w = w->next)
		if (w->scr == cur)
			top = w;
	return top;
}

static void
newwin(void)
{
	static int n;
	Win *w = calloc(1, sizeof *w);

	w->w = MAX(cols / 2, 20), w->h = MAX((rows - 1) / 2, 6);
	w->x = (cols - w->w) / 2 + n % 5 * 2 - 4, w->y = (rows - 1 - w->h) / 2 + n % 5 - 2;
	w->x = MAX(0, w->x), w->y = MAX(0, w->y);
	w->scr = cur;
	n++;
	tnew(&w->t, w->w - 2, w->h - 2);
	raise_(w);
}

static void
closewin(Win *w)
{
	Win **p;

	for (p = &wins; *p; p = &(*p)->next)
		if (*p == w) {
			*p = w->next;
			break;
		}
	if (drag == w)
		drag = NULL;
	tfree(&w->t);
	free(w);
	sel = topwin();
}

static void
fit(Win *w)
{
	int x, y, c, r;

	inner(w, &x, &y, &c, &r);
	tresize(&w->t, c, r);
}

static void
fullscreen(Win *w)
{
	if ((w->full = !w->full)) {
		w->ox = w->x, w->oy = w->y, w->ow = w->w, w->oh = w->h;
		w->x = 0, w->y = 0, w->w = cols, w->h = rows - 1;
	} else
		w->x = w->ox, w->y = w->oy, w->w = w->ow, w->h = w->oh;
	fit(w);
}

static void
view(int i)
{
	cur = i;
	if (!shell[i].pid)
		tnew(&shell[i], cols, rows - 1);
	sel = topwin();
}

static void
sendwin(int i)
{
	if (!sel)
		return;
	sel->scr = i;
	sel = topwin();
}

static void
nextwin(void)
{
	Win *w;

	for (w = wins; w; w = w->next)
		if (w->scr == cur && w != sel) {
			raise_(w);
			return;
		}
}

/* ---- what goes on the screen ---- */
static void
put(int x, int y, unsigned c, int fg, int bg)
{
	if (x < 0 || y < 0 || x >= cols || y >= rows - 1)
		return;
	scr[y * cols + x] = (Cell){ c, fg, bg };
}

static void
puts_(int x, int y, const char *s, int fg, int bg)
{
	for (; *s; s++, x++)
		put(x, y, (unsigned char)*s, fg, bg);
}

/* ┌─[ name ]───[x]┐ */
static void
frame(Win *w)
{
	int fg = w == sel ? 15 : 7, i, x, n;
	char name[80];

	put(w->x, w->y, 0x250c, fg, 0);
	put(w->x + 1, w->y, 0x2500, fg, 0);
	snprintf(name, sizeof name, "[ %s ]", w->t.title);
	n = MIN((int)strlen(name), w->w - 7);
	name[MAX(n, 0)] = 0;
	puts_(w->x + 2, w->y, name, fg, 0);
	for (x = w->x + 2 + n; x < w->x + w->w - 4; x++)
		put(x, w->y, 0x2500, fg, 0);
	puts_(w->x + w->w - 4, w->y, "[x]", fg, 0);
	put(w->x + w->w - 1, w->y, 0x2510, fg, 0);
	for (i = 1; i < w->h - 1; i++) {
		put(w->x, w->y + i, 0x2502, fg, 0);
		put(w->x + w->w - 1, w->y + i, 0x2502, fg, 0);
	}
	put(w->x, w->y + w->h - 1, 0x2514, fg, 0);
	for (x = w->x + 1; x < w->x + w->w - 1; x++)
		put(x, w->y + w->h - 1, 0x2500, fg, 0);
	put(w->x + w->w - 1, w->y + w->h - 1, 0x2518, fg, 0);
}

static void
bar(void)
{
	char s[128], bat[32] = "";
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	FILE *f;
	int i, n, y = rows - 1;

	if ((f = fopen("/sys/class/power_supply/BAT0/capacity", "r"))) {
		if (fscanf(f, "%d", &n) == 1)
			snprintf(bat, sizeof bat, "bat %d%%  ", n);
		fclose(f);
	}
	n = strftime(s, sizeof s, "%a %d %b  %H:%M", tm);
	snprintf(s + n, sizeof s - n, "  tty%d ", cur + 1);
	for (i = 0; i < cols; i++)
		scr[y * cols + i] = (Cell){ ' ', 7, 0 };
	n = strlen(bat) + strlen(s);
	for (i = 0; bat[i]; i++)
		scr[y * cols + cols - n + i] = (Cell){ (unsigned char)bat[i], 7, 0 };
	for (i = 0; s[i]; i++)
		scr[y * cols + cols - strlen(s) + i] = (Cell){ (unsigned char)s[i], 7, 0 };
}

static void
invert(int x, int y)
{
	Cell *c;
	unsigned char t;

	if (x < 0 || y < 0 || x >= cols || y >= rows)
		return;
	c = &scr[y * cols + x];
	t = c->fg, c->fg = c->bg, c->bg = t;
	if (c->fg == c->bg)
		c->fg = c->bg ? 0 : 7;
}

static void
compose(void)
{
	Term *t = &shell[cur];
	Win *w;
	int x, y, ix, iy, c, r;

	for (y = 0; y < rows - 1; y++)
		for (x = 0; x < cols; x++)
			scr[y * cols + x] = t->cell[y * t->cols + x];
	for (w = wins; w; w = w->next) {
		if (w->scr != cur)
			continue;
		if (!w->full)
			frame(w);
		inner(w, &ix, &iy, &c, &r);
		for (y = 0; y < MIN(r, w->t.rows); y++)
			for (x = 0; x < MIN(c, w->t.cols); x++) {
				Cell *p = &w->t.cell[y * w->t.cols + x];
				put(ix + x, iy + y, p->c, p->fg, p->bg);
			}
	}
	bar();
	t = focused();
	if (!t->hide) {
		ix = iy = 0;
		if (sel)
			inner(sel, &ix, &iy, &c, &r);
		if (iy + t->y < rows - 1)
			invert(ix + t->x, iy + t->y);
	}
	invert(mx / fw, my / fh);
}

static void
draw(void)
{
	int i;

	compose();
	for (i = 0; i < cols * rows; i++)
		if (memcmp(&scr[i], &old[i], sizeof(Cell))) {
			drawcell(i % cols, i / cols, &scr[i]);
			old[i] = scr[i];
		}
	dirty = 0;
}

/* ---- keyboard and mouse ---- */
static void
opendevs(void)
{
	unsigned long ev = 0, key[KEY_MAX / 64 + 1];
	struct input_absinfo a;
	struct dirent *e;
	char path[300];
	DIR *d = opendir("/dev/input");
	int fd;

	if (!d)
		die("/dev/input");
	while ((e = readdir(d)) && ndev < MAXDEV) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0)
			continue;
		memset(key, 0, sizeof key);
		ioctl(fd, EVIOCGBIT(0, sizeof ev), &ev);
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
		if (!(ev & (1 << EV_KEY))) {
			close(fd);
			continue;
		}
		dev[ndev].fd = fd;
		dev[ndev].kbd = !!(key[KEY_A / 64] & 1UL << (KEY_A % 64));
		if (ev & (1 << EV_ABS)) {
			if (ioctl(fd, EVIOCGABS(ABS_X), &a) == 0)
				dev[ndev].ax[0] = a.minimum, dev[ndev].ax[1] = a.maximum;
			if (ioctl(fd, EVIOCGABS(ABS_Y), &a) == 0)
				dev[ndev].ay[0] = a.minimum, dev[ndev].ay[1] = a.maximum;
		}
		ioctl(fd, EVIOCGRAB, 1);
		ndev++;
	}
	closedir(d);
}

static void
type(int code)
{
	static const struct { int code; char *s; } seq[] = {
		{ KEY_UP, "\033[A" }, { KEY_DOWN, "\033[B" }, { KEY_RIGHT, "\033[C" }, { KEY_LEFT, "\033[D" },
		{ KEY_HOME, "\033[1~" }, { KEY_INSERT, "\033[2~" }, { KEY_DELETE, "\033[3~" }, { KEY_END, "\033[4~" },
		{ KEY_PAGEUP, "\033[5~" }, { KEY_PAGEDOWN, "\033[6~" }, { KEY_KPENTER, "\r" },
		{ KEY_F1, "\033[[A" }, { KEY_F2, "\033[[B" }, { KEY_F3, "\033[[C" }, { KEY_F4, "\033[[D" },
		{ KEY_F5, "\033[[E" },
	};
	Term *t = focused();
	char c, buf[2];
	unsigned i;

	for (i = 0; i < LEN(seq); i++)
		if (seq[i].code == code) {
			reply(t, seq[i].s);
			return;
		}
	if (code >= (int)sizeof keys - 1 || !(c = (shift ? shiftkeys : keys)[code]))
		return;
	if (caps && c >= 'a' && c <= 'z')
		c -= 32;
	else if (caps && c >= 'A' && c <= 'Z')
		c += 32;
	if (ctrl && c >= '@' && c <= '~')
		c &= 0x1f;
	else if (ctrl && c == ' ')
		c = 0;
	buf[0] = 033, buf[1] = c;
	write(t->fd, alt ? buf : buf + 1, alt ? 2 : 1);
}

static void
superkey(int code)
{
	if (code >= KEY_1 && code <= KEY_9) {
		if (shift)
			sendwin(code - KEY_1);
		else
			view(code - KEY_1);
	} else if (code == KEY_ENTER)
		newwin();
	else if (code == KEY_Q && sel)
		closewin(sel);
	else if (code == KEY_TAB)
		nextwin();
	else if (code == KEY_F && sel)
		fullscreen(sel);
	else if (code == KEY_E && shift)
		running = 0;
}

static Win *
winat(int x, int y)
{
	Win *w, *hit = NULL;

	for (w = wins; w; w = w->next)
		if (w->scr == cur && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
			hit = w;
	return hit;
}

static void
click(int button)
{
	int x = mx / fw, y = my / fh;
	Win *w = winat(x, y);

	if (!w) {
		sel = NULL;
		return;
	}
	raise_(w);
	if (!w->full && y == w->y && x >= w->x + w->w - 4 && x < w->x + w->w - 1) {
		closewin(w);
		return;
	}
	if (w->full)
		return;
	if ((super && (button == BTN_LEFT || button == BTN_RIGHT)) || y == w->y) {
		drag = w, dragrs = super && button == BTN_RIGHT;
		dragx = x, dragy = y;
	}
}

static void
motion(void)
{
	int x = mx / fw, y = my / fh;

	if (!drag || (x == dragx && y == dragy))
		return;
	if (dragrs) {
		drag->w = MAX(drag->w + x - dragx, 12);
		drag->h = MAX(drag->h + y - dragy, 4);
		fit(drag);
	} else
		drag->x += x - dragx, drag->y += y - dragy;
	dragx = x, dragy = y;
}

static void
input(Dev *d)
{
	struct input_event ev[64];
	int n, i, moved = 0;

	while ((n = read(d->fd, ev, sizeof ev)) > 0) {
		for (i = 0; i < n / (int)sizeof *ev; i++) {
			struct input_event *e = &ev[i];
			if (e->type == EV_REL && e->code == REL_X)
				mx += e->value, moved = 1;
			else if (e->type == EV_REL && e->code == REL_Y)
				my += e->value, moved = 1;
			else if (e->type == EV_ABS && e->code == ABS_X && d->ax[1] > d->ax[0])
				mx = (long)(e->value - d->ax[0]) * fbw / (d->ax[1] - d->ax[0] + 1), moved = 1;
			else if (e->type == EV_ABS && e->code == ABS_Y && d->ay[1] > d->ay[0])
				my = (long)(e->value - d->ay[0]) * fbh / (d->ay[1] - d->ay[0] + 1), moved = 1;
			else if (e->type == EV_KEY && e->code >= BTN_MOUSE && e->code < BTN_JOYSTICK) {
				if (e->value == 1)
					click(e->code);
				else if (e->value == 0)
					drag = NULL;
			} else if (e->type == EV_KEY) {
				int down = e->value != 0;
				switch (e->code) {
				case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift = down; break;
				case KEY_LEFTCTRL: case KEY_RIGHTCTRL: ctrl = down; break;
				case KEY_LEFTALT: case KEY_RIGHTALT: alt = down; break;
				case KEY_LEFTMETA: case KEY_RIGHTMETA: super = down; break;
				case KEY_CAPSLOCK: if (e->value == 1) caps = !caps; break;
				default:
					if (down)
						super ? superkey(e->code) : type(e->code);
				}
			}
			dirty = 1;
		}
		if (moved) {
			mx = MAX(0, MIN(mx, cols * fw - 1));
			my = MAX(0, MIN(my, rows * fh - 1));
			motion();
		}
	}
}

/* ---- the loop ---- */
static void
quit(int sig)
{
	(void)sig;
	running = 0;
}

static int
output(Term *t)
{
	unsigned char buf[8192];
	int n;

	while ((n = read(t->fd, buf, sizeof buf)) > 0)
		tfeed(t, buf, n);
	dirty = 1;
	return n == 0 || (n < 0 && errno != EAGAIN);
}

int
main(void)
{
	struct pollfd pf[MAXDEV + NSCR + 256];
	Term *who[LEN(pf)];
	Win *w, *next;
	int i, n, nd;

	sh = getenv("SHELL");
	if (!sh || access(sh, X_OK))
		sh = access("/sh", X_OK) == 0 ? "/sh" : "/bin/sh";
	/* the console it's started from: that one is yours. /dev/tty0 is root's */
	tty = 0;
	if (ioctl(tty, KDGETMODE, &i) < 0 && (tty = open("/dev/tty0", O_RDWR)) < 0)
		die("not on a console");
	getfont();
	openfb();
	opendevs();
	signal(SIGCHLD, SIG_DFL);
	signal(SIGTERM, quit);
	signal(SIGINT, quit);
	signal(SIGHUP, quit);
	ioctl(tty, KDSETMODE, KD_GRAPHICS);
	mx = fbw / 2, my = fbh / 2;
	view(0);

	while (running) {
		if (dirty)
			draw();
		n = 0;
		for (i = 0; i < ndev; i++)
			pf[n].fd = dev[i].fd, pf[n].events = POLLIN, who[n++] = NULL;
		nd = n;
		for (i = 0; i < NSCR; i++)
			if (shell[i].pid)
				pf[n].fd = shell[i].fd, pf[n].events = POLLIN, who[n++] = &shell[i];
		for (w = wins; w && n < (int)LEN(pf); w = w->next)
			pf[n].fd = w->t.fd, pf[n].events = POLLIN, who[n++] = &w->t;
		if (poll(pf, n, 1000) == 0) {
			dirty = 1;
			continue;
		}
		for (i = 0; i < nd; i++)
			if (pf[i].revents)
				input(&dev[i]);
		for (i = nd; i < n; i++) {
			if (!pf[i].revents || !output(who[i]))
				continue;
			/* its shell has gone: a screen gets a new one, a window closes */
			if (who[i] >= shell && who[i] < shell + NSCR) {
				tfree(who[i]);
				tnew(who[i], cols, rows - 1);
			} else
				for (w = wins; w; w = next) {
					next = w->next;
					if (&w->t == who[i])
						closewin(w);
				}
			/* the pollfd array still points at what's gone: start again */
			break;
		}
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
	}
	for (i = 0; i < NSCR; i++)
		if (shell[i].pid)
			tfree(&shell[i]);
	while (wins)
		closewin(wins);
	ioctl(tty, KDSETMODE, KD_TEXT);
	return 0;
}
