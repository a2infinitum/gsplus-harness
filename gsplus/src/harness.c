/**********************************************************************/
/*                    GSplus - Apple //gs Emulator                    */
/*                    Based on KEGS by Kent Dickey                    */
/*                    Copyright 2002-2023 Kent Dickey                 */
/*                    Copyright 2025-2026 GSplus Contributors         */
/*                                                                    */
/*      This code is covered by the GNU GPL v3                        */
/*      See the file COPYING.txt or https://www.gnu.org/licenses/     */
/**********************************************************************/

/* Headless test-harness instrumentation.
 *
 * Flags (any of them activates harness mode):
 *   -hdir <dir>       artifact output directory (default ".")
 *   -hsecs <secs>     stop after <secs> EMULATED seconds (60 VBLs = 1s),
 *                     exit 0 -- deterministic, unlike wall-clock -timeout
 *   -hshot <secs>     save a screenshot every <secs> emulated seconds
 *   -hbp <addr>       code breakpoint, hex, "bb/aaaa" or "bbaaaa" form
 *                     (repeatable); a hit dumps state and exits 2
 *   -hbpw <addr>      memory-write breakpoint, same form (repeatable)
 *   -hdump <addr:len> memory region included in every state dump, and
 *                     also written raw as dump-<addr>.bin (repeatable)
 *   -hbrk             halt on the BRK opcode (native or emulation mode)
 *   -hstall <secs>    if the screen stops changing for <secs> emulated
 *                     seconds, dump state and exit 3
 *
 * Exit codes: 0 = -hsecs limit reached, 2 = CPU halted (breakpoint/BRK/
 * halt_printf), 3 = stalled screen.  Artifacts: shot-*.png (periodic),
 * final-<reason>.png, state-<reason>.txt, dump-*.bin, result.txt
 * ("<reason> exit=<code> t=<secs>" -- one line, script-friendly).
 *
 * Screenshots are rendered from the emulator's own framebuffer
 * (g_mainwin_kimage), not the SDL renderer, so they work under
 * SDL_VIDEODRIVER=dummy where SDL_RenderReadPixels has nothing to read.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include "defc.h"
#include "harness.h"

extern Engine_reg engine;
extern int g_halt_sim;
extern word32 g_vbl_count;
extern Kimage g_mainwin_kimage;

void set_bp(word32 addr, word32 end_addr, word32 acc_type);
int write_png_rgba(const char *path, const unsigned char *rgba, int w, int h);
char *do_dis(word32 kpc, int accsize, int xsize, int op_provided, word32 instr,
							int *size_ptr);

int	g_harness_trap_brk = 0;
int	g_harness_turbo = 0;

#define HARNESS_MAX_DUMPS	16

typedef struct {
	word32	addr;
	word32	len;
} Harness_dump;

static int	g_h_active = 0;
static char	g_h_dir[1024] = ".";
static double	g_h_limit_secs = 0.0;
static double	g_h_shot_secs = 0.0;
static double	g_h_stall_secs = 0.0;
static Harness_dump g_h_dumps[HARNESS_MAX_DUMPS];
static int	g_h_num_dumps = 0;

static int	g_h_exit_code = 0;
static int	g_h_done = 0;
static double	g_h_next_shot = 0.0;
static word32	g_h_fb_hash = 0;
static double	g_h_fb_same_since = 0.0;
static int	g_h_shot_count = 0;

/* Parse "bb/aaaa", "bbaaaa" or "aaaa" hex into a 24-bit address;
 * returns (word32)-1 on garbage. */
static word32
harness_parse_addr(const char *str)
{
	word32	val;
	char	*end;
	const char *slash;

	slash = strchr(str, '/');
	if(slash) {
		word32 bank = (word32)strtoul(str, &end, 16);
		if(end != slash) {
			return (word32)-1;
		}
		val = (word32)strtoul(slash + 1, &end, 16);
		if(*end || (val > 0xffff) || (bank > 0xff)) {
			return (word32)-1;
		}
		return (bank << 16) | val;
	}
	val = (word32)strtoul(str, &end, 16);
	if(*end || (val > 0xffffff)) {
		return (word32)-1;
	}
	return val;
}

void
harness_usage(void)
{
	printf("  -hdir <dir>      Test harness: artifact output directory\n");
	printf("  -hsecs <secs>    Test harness: quit 0 after emulated secs\n");
	printf("  -hshot <secs>    Test harness: screenshot every emulated "
								"secs\n");
	printf("  -hbp <bb/aaaa>   Test harness: code breakpoint -> dump, "
								"exit 2\n");
	printf("  -hbpw <bb/aaaa>  Test harness: write breakpoint -> dump, "
								"exit 2\n");
	printf("  -hdump <adr:len> Test harness: memory region in state "
								"dumps\n");
	printf("  -hbrk            Test harness: halt on BRK opcode\n");
	printf("  -hstall <secs>   Test harness: exit 3 if screen frozen "
								"that long\n");
	printf("  -hturbo          Test harness: no frame pacing, run flat "
								"out\n");
}

/* Called from parse_argv().  Returns 1 and advances *i_ptr past any value
 * argument when the flag was ours, 0 when it was not, -1 on a bad value
 * (caller exits). */
int
harness_parse_argv(int argc, char **argv, int *i_ptr)
{
	const char *arg, *val;
	word32	addr;
	int	i;

	i = *i_ptr;
	arg = argv[i];
	if(strncmp(arg, "-h", 2) != 0) {
		return 0;
	}
	/* Flags with no value first */
	if(!strcmp(arg, "-hbrk")) {
		g_harness_trap_brk = 1;
		g_h_active = 1;
		return 1;
	}
	if(!strcmp(arg, "-hturbo")) {
		g_harness_turbo = 1;
		g_h_active = 1;
		return 1;
	}
	if(strcmp(arg, "-hdir") && strcmp(arg, "-hsecs") &&
			strcmp(arg, "-hshot") && strcmp(arg, "-hbp") &&
			strcmp(arg, "-hbpw") && strcmp(arg, "-hdump") &&
			strcmp(arg, "-hstall")) {
		return 0;		/* not a harness flag */
	}
	if(i >= (argc - 1)) {
		printf("%s needs an argument\n", arg);
		return -1;
	}
	val = argv[i + 1];
	*i_ptr = i + 1;
	if(!strcmp(arg, "-hdir")) {
		snprintf(g_h_dir, sizeof(g_h_dir), "%s", val);
	} else if(!strcmp(arg, "-hsecs")) {
		g_h_limit_secs = strtod(val, 0);
	} else if(!strcmp(arg, "-hshot")) {
		g_h_shot_secs = strtod(val, 0);
		g_h_next_shot = g_h_shot_secs;
	} else if(!strcmp(arg, "-hstall")) {
		g_h_stall_secs = strtod(val, 0);
	} else if(!strcmp(arg, "-hbp") || !strcmp(arg, "-hbpw")) {
		addr = harness_parse_addr(val);
		if(addr == (word32)-1) {
			printf("%s: bad address %s\n", arg, val);
			return -1;
		}
		set_bp(addr, addr, !strcmp(arg, "-hbpw") ? 2 : 4);
	} else if(!strcmp(arg, "-hdump")) {
		char	buf[64];
		char	*colon, *end;
		word32	len;

		snprintf(buf, sizeof(buf), "%s", val);
		colon = strchr(buf, ':');
		if(!colon) {
			printf("-hdump wants addr:len, got %s\n", val);
			return -1;
		}
		*colon = 0;
		addr = harness_parse_addr(buf);
		len = (word32)strtoul(colon + 1, &end, 16);
		if((addr == (word32)-1) || *end || (len == 0) ||
							(len > 0x10000)) {
			printf("-hdump: bad region %s\n", val);
			return -1;
		}
		if(g_h_num_dumps >= HARNESS_MAX_DUMPS) {
			printf("-hdump: too many regions\n");
			return -1;
		}
		g_h_dumps[g_h_num_dumps].addr = addr;
		g_h_dumps[g_h_num_dumps].len = len;
		g_h_num_dumps++;
	}
	g_h_active = 1;
	return 1;
}

static FILE *
harness_fopen(const char *name, const char *mode)
{
	char	path[1200];
	FILE	*f;

	snprintf(path, sizeof(path), "%s/%s", g_h_dir, name);
	f = fopen(path, mode);
	if(!f) {
		printf("Harness: cannot open %s: %s\n", path, strerror(errno));
	}
	return f;
}

/* PNG of the emulator's internal framebuffer -- works headless. */
static void
harness_screenshot(const char *name)
{
	unsigned char *rgba, *dst;
	word32	*src;
	word32	pix;
	char	path[1200];
	int	w, h, x, y, rc;

	w = g_mainwin_kimage.a2_width_full;
	h = g_mainwin_kimage.a2_height_full;
	src = g_mainwin_kimage.wptr;
	if(!src || (w <= 0) || (h <= 0)) {
		return;
	}
	rgba = malloc((size_t)w * h * 4);
	if(!rgba) {
		return;
	}
	dst = rgba;
	for(y = 0; y < h; y++) {
		for(x = 0; x < w; x++) {
			pix = src[(size_t)y * w + x];	/* ARGB8888 */
			*dst++ = (pix >> 16) & 0xff;
			*dst++ = (pix >> 8) & 0xff;
			*dst++ = pix & 0xff;
			*dst++ = 0xff;
		}
	}
	snprintf(path, sizeof(path), "%s/%s", g_h_dir, name);
	rc = write_png_rgba(path, rgba, w, h);
	free(rgba);
	if(rc == 0) {
		printf("Harness: screenshot %s\n", path);
	}
}

/* Read one byte for a state dump without tripping I/O softswitches: a
 * get_memory_c() of $C000-$C0FF has side effects (reading $C0EC pops a
 * disk byte), so the I/O page reads as 0xee filler. */
static word32
harness_peek(word32 addr)
{
	word32	bank;

	addr = addr & 0xffffff;
	bank = addr >> 16;
	if(((bank == 0x00) || (bank == 0x01) || (bank == 0xe0) ||
			(bank == 0xe1)) &&
			((addr & 0xff00) == 0xc000)) {
		return 0xee;
	}
	return get_memory_c(addr) & 0xff;
}

static void
harness_hexdump(FILE *f, word32 addr, word32 len)
{
	word32	i;

	for(i = 0; i < len; i++) {
		if((i & 0xf) == 0) {
			fprintf(f, "%02x/%04x:", ((addr + i) >> 16) & 0xff,
							(addr + i) & 0xffff);
		}
		fprintf(f, " %02x", harness_peek(addr + i));
		if(((i & 0xf) == 0xf) || (i == len - 1)) {
			fprintf(f, "\n");
		}
	}
}

/* The text page as currently displayed (40 or 80 column per the live video
 * mode) -- cfg_text_screen_str() is the emulator's own decoder, also behind
 * the F4 config panel's "Dump text screen to file". */
static void
harness_text_screen(FILE *f)
{
	fprintf(f, "\n== text screen (as displayed) ==\n");
	fputs(cfg_text_screen_str(), f);
}

static void
harness_state_dump(const char *reason, double esecs)
{
	FILE	*f;
	char	name[128];
	word32	kpc, psr, stack;
	int	accsize, xsize, size, i, d;

	snprintf(name, sizeof(name), "state-%s.txt", reason);
	f = harness_fopen(name, "w");
	if(!f) {
		return;
	}

	kpc = engine.kpc;
	psr = engine.psr;
	stack = engine.stack;
	fprintf(f, "reason: %s\n", reason);
	fprintf(f, "emulated_secs: %.2f (vbl %u)\n", esecs, g_vbl_count);
	fprintf(f, "\n== registers ==\n");
	fprintf(f, "K/PC: %02x/%04x  A:%04x X:%04x Y:%04x S:%04x D:%04x "
		"B:%02x  P:%03x (%c%c%c%c%c%c%c%c e:%d)\n",
		kpc >> 16, kpc & 0xffff, engine.acc, engine.xreg, engine.yreg,
		stack, engine.direct, engine.dbank, psr,
		(psr & 0x80) ? 'N' : 'n', (psr & 0x40) ? 'V' : 'v',
		(psr & 0x20) ? 'M' : 'm', (psr & 0x10) ? 'X' : 'x',
		(psr & 0x08) ? 'D' : 'd', (psr & 0x04) ? 'I' : 'i',
		(psr & 0x02) ? 'Z' : 'z', (psr & 0x01) ? 'C' : 'c',
		(psr >> 8) & 1);

	accsize = ((psr & 0x120) != 0) ? 1 : 2;
	xsize = ((psr & 0x110) != 0) ? 1 : 2;
	fprintf(f, "\n== disassembly from K/PC ==\n");
	for(i = 0; i < 16; i++) {
		fprintf(f, "%s\n", do_dis(kpc, accsize, xsize, 0, 0, &size));
		kpc = (kpc & 0xff0000) | ((kpc + size) & 0xffff);
	}

	fprintf(f, "\n== stack (S upward) ==\n");
	harness_hexdump(f, stack & 0xffff, 48);
	fprintf(f, "\n== direct page (D) ==\n");
	harness_hexdump(f, engine.direct & 0xffff, 64);

	for(d = 0; d < g_h_num_dumps; d++) {
		word32	addr = g_h_dumps[d].addr;
		word32	len = g_h_dumps[d].len;
		FILE	*bin;
		word32	j;

		fprintf(f, "\n== dump %02x/%04x len %x ==\n",
				addr >> 16, addr & 0xffff, len);
		harness_hexdump(f, addr, len);
		snprintf(name, sizeof(name), "dump-%06x.bin", addr);
		bin = harness_fopen(name, "wb");
		if(bin) {
			for(j = 0; j < len; j++) {
				fputc(harness_peek(addr + j), bin);
			}
			fclose(bin);
		}
	}

	harness_text_screen(f);
	fclose(f);
	printf("Harness: state dump %s/state-%s.txt\n", g_h_dir, reason);
}

/* Final artifacts + one-line verdict; arms the exit path. */
static int
harness_finish(const char *reason, int code, double esecs)
{
	FILE	*f;
	char	name[128];

	g_h_done = 1;
	g_h_exit_code = code;
	harness_state_dump(reason, esecs);
	snprintf(name, sizeof(name), "final-%s.png", reason);
	harness_screenshot(name);
	f = harness_fopen("result.txt", "w");
	if(f) {
		fprintf(f, "%s exit=%d t=%.2f\n", reason, code, esecs);
		fclose(f);
	}
	printf("Harness: %s at t=%.2fs, exiting %d\n", reason, esecs, code);
	fflush(stdout);
	return 0x200;		/* nonzero: main loop quits */
}

static word32
harness_fb_hash(void)
{
	word32	*src;
	word32	hash;
	int	w, h, i, n;

	src = g_mainwin_kimage.wptr;
	w = g_mainwin_kimage.a2_width_full;
	h = g_mainwin_kimage.a2_height_full;
	if(!src) {
		return 0;
	}
	/* The status lines at the bottom redraw every second (sim MHz etc.),
	 * so a "frozen" screen would never hash equal -- exclude them. */
	if(h > (MAX_STATUS_LINES * 16 + 2)) {
		h -= MAX_STATUS_LINES * 16 + 2;
	}
	hash = 2166136261u;
	n = w * h;
	for(i = 0; i < n; i++) {
		hash = (hash ^ src[i]) * 16777619u;
	}
	return hash;
}

/* Called once per VBL from run_16ms(). */
int
harness_tick(void)
{
	double	esecs;
	word32	hash;

	if(!g_h_active || g_h_done) {
		return 0;
	}
	esecs = g_vbl_count / 60.0;

	if(g_halt_sim) {
		return harness_finish("halt", 2, esecs);
	}
	if((g_h_shot_secs > 0.0) && (esecs >= g_h_next_shot)) {
		char	name[128];

		snprintf(name, sizeof(name), "shot-%03d-t%07.2fs.png",
						g_h_shot_count++, esecs);
		harness_screenshot(name);
		g_h_next_shot += g_h_shot_secs;
	}
	if((g_h_stall_secs > 0.0) && ((g_vbl_count % 30) == 0)) {
		hash = harness_fb_hash();
		if(hash != g_h_fb_hash) {
			g_h_fb_hash = hash;
			g_h_fb_same_since = esecs;
		} else if((esecs - g_h_fb_same_since) >= g_h_stall_secs) {
			return harness_finish("stall", 3, esecs);
		}
	}
	if((g_h_limit_secs > 0.0) && (esecs >= g_h_limit_secs)) {
		return harness_finish("timeout", 0, esecs);
	}
	return 0;
}

int
harness_exit_code(void)
{
	return g_h_exit_code;
}
