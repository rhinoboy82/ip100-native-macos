/*
 * rastertoip100 - native CUPS filter for the Canon PIXMA iP100 (plain paper, 600 dpi).
 *
 * Reads 8-bit RGB or grayscale CUPS raster and writes the Canon command stream that
 * Canon's own (Intel-only) Mac driver produces. Command sequence, page geometry and
 * the RGB -> CMYK ink table were derived from that driver's output; see ref/ and tools/.
 * Pages with no colour are printed with black ink only, as Canon's driver does.
 *
 * Usage (as a CUPS filter): rastertoip100 job user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ip100_plain_lut.h"

#define IP100_VERSION   "1.0.0"
#define DPI             600
#define LINES_PER_BLOCK 16
#define MAX_WIDTH_DOTS  4800 /* 8 inch print head sweep */

/* Page geometry per paper size, in 600 dpi dots, exactly as Canon's driver sends it. */
typedef struct {
  const char *name;
  int paper_w, paper_h;   /* physical paper size */
  int left, top;          /* printable area origin */
  int area_w, area_h;     /* printable area size */
  int flag;               /* 7 for Letter/Legal (area narrower than paper), else 0 */
} geometry_t;

static const geometry_t geometries[] = {
  { "Letter", 5100, 6600, 151, 70, 4800, 6412, 7 },
  { "Legal",  5100, 8400, 151, 70, 4800, 8212, 7 },
  { "A4",     4961, 7016,  80, 70, 4800, 6827, 0 },
  { "A5",     3497, 4961,  80, 70, 3336, 4772, 0 },
  { "B5",     4300, 6071,  80, 70, 4139, 5882, 0 },
  { "4x6",    2400, 3600,  80, 70, 2240, 3412, 0 },
  { "5x7",    3000, 4200,  80, 70, 2840, 4012, 0 },
  { "8x10",   4800, 6000,  80, 70, 4640, 5812, 0 },
  { "Env10",  2475, 5700,  80, 70, 2315, 5004, 0 },
};

/* ---------- growable output buffer (one page is buffered so the last-page flag can be set) ---------- */

typedef struct {
  unsigned char *data;
  size_t len, cap;
} buf_t;

static void buf_put(buf_t *b, const void *p, size_t n)
{
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 65536;
    while (cap < b->len + n)
      cap *= 2;
    b->data = realloc(b->data, cap);
    if (!b->data) {
      fputs("ERROR: Out of memory\n", stderr);
      exit(1);
    }
    b->cap = cap;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
}

static void buf_byte(buf_t *b, unsigned char c) { buf_put(b, &c, 1); }

static void put_be16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void put_be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* ESC ( c <len16le> body */
static void canon_cmd(buf_t *b, char c, const unsigned char *body, unsigned len)
{
  unsigned char hdr[5] = { 0x1b, '(', (unsigned char)c, len & 0xff, len >> 8 };
  buf_put(b, hdr, 5);
  buf_put(b, body, len);
}

static void canon_cmd1(buf_t *b, char c, unsigned char v) { canon_cmd(b, c, &v, 1); }

static void canon_skip(buf_t *b, unsigned blocks)
{
  unsigned char body[2];
  put_be16(body, blocks);
  canon_cmd(b, 'e', body, 2);
}

static void write_all(const void *p, size_t n)
{
  const unsigned char *c = p;
  while (n > 0) {
    ssize_t w = write(1, c, n);
    if (w < 0) {
      perror("ERROR: Unable to write print data");
      exit(1);
    }
    c += w;
    n -= (size_t)w;
  }
}

/* Job preamble: status request, then BJL blocks (ESC [K with a 2-byte body, followed by free text). */
static void job_start(void)
{
  static const char status_req[] = "\x00\x1e\x00" "BSSR=DJS,DBS,DWS,DOC,DSC,BST,PID,CHD,OPT,LVR,CIR,CTK,AOF,HRI,MSI;";
  buf_t b = { 0 };
  char text[160];
  time_t now = time(NULL);
  struct tm tm;
  unsigned char hdr[5] = { 0x1b, '[', 'K', 0, 0 };

  buf_byte(&b, 0x00);

  hdr[3] = sizeof(status_req) - 1;
  buf_put(&b, hdr, 5);
  buf_put(&b, status_req, sizeof(status_req) - 1);

  localtime_r(&now, &tm);
  snprintf(text, sizeof(text), "BJLSTART\nControlmode = Common\nSetTime = %04d%02d%02d%02d%02d%02d\nBJLEND\n",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  hdr[3] = 2;
  buf_put(&b, hdr, 5);
  buf_put(&b, "\x00\x1f", 2);
  buf_put(&b, text, strlen(text));

  buf_put(&b, hdr, 5);
  buf_put(&b, "\x00\x1f", 2);
  strcpy(text, "BJLSTART\nCONTROLMODE=COMMON\nSETSILENT=OFF\nBJLEND\n");
  buf_put(&b, text, strlen(text));

  buf_put(&b, hdr, 5);
  buf_put(&b, "\x00\x0f", 2);

  write_all(b.data, b.len);
  free(b.data);
}

static void job_end(void)
{
  static const unsigned char end[] = { 0x1b, '[', 'K', 11, 0, 0x00, 0x1e, 0x00, 0x09, 'S', 'S', 'R', '=', 'D', 'F', ';' };
  write_all(end, sizeof(end));
}

/* Offset of the ESC (s last-page flag's value inside a page buffer. */
static size_t page_header(buf_t *b, const geometry_t *g, int page_no)
{
  static const unsigned char ink_setup[36] = {
    0x80, 0x80, 0x01, 0x22, 0x00, 0x03, 0x22, 0x00, 0x03, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02
  };
  unsigned char p[46] = { 0 };
  unsigned char d[4] = { 0x02, 0x58, 0x02, 0x58 };
  unsigned char c[3] = { 0x30, 0x00, 0x03 };
  unsigned char l[2] = { 0x34, 0x00 };
  unsigned char dollar[5] = { 0x01, 0, 0, 0, 0 };
  size_t last_flag;

  if (page_no == 1)
    canon_cmd1(b, 'b', 0x01);
  canon_cmd(b, 'd', d, 4);
  canon_cmd(b, 't', ink_setup, sizeof(ink_setup));
  canon_cmd(b, 'c', c, 3);

  put_be16(p + 0, (g->area_h + 9) / 10);
  put_be16(p + 4, (g->area_w + 9) / 10);
  put_be16(p + 6, g->flag);
  put_be16(p + 12, DPI);
  put_be32(p + 14, g->left);
  put_be32(p + 18, g->top);
  put_be32(p + 22, g->area_w);
  put_be32(p + 26, g->area_h);
  put_be32(p + 38, g->paper_w);
  put_be32(p + 42, g->paper_h);
  canon_cmd(b, 'p', p, sizeof(p));

  canon_cmd(b, 'l', l, 2);
  canon_cmd1(b, 'u', 0x00);
  canon_cmd1(b, 's', 0x00);
  last_flag = b->len - 1;
  canon_cmd1(b, 'q', (unsigned char)page_no);
  canon_cmd(b, '$', dollar, 5);
  canon_cmd1(b, 'b', 0x01);
  canon_cmd1(b, 'I', 0x01);
  canon_cmd1(b, 'J', LINES_PER_BLOCK);
  return last_flag;
}

/* PackBits one line (trailing zero bytes already trimmed), then Canon's 0x80 line terminator. */
static void packbits_line(buf_t *b, const unsigned char *row, int n)
{
  int i = 0;
  while (i < n) {
    int run = 1;
    while (i + run < n && run < 128 && row[i + run] == row[i])
      run++;
    if (run >= 2) {
      buf_byte(b, (unsigned char)(257 - run));
      buf_byte(b, row[i]);
      i += run;
    } else {
      int start = i;
      i++;
      while (i < n && i - start < 128 && !(i + 1 < n && row[i + 1] == row[i]))
        i++;
      buf_byte(b, (unsigned char)(i - start - 1));
      buf_put(b, row + start, i - start);
    }
  }
  buf_byte(b, 0x80);
}

static const geometry_t *find_geometry(const cups_page_header2_t *h)
{
  int w = (int)((h->PageSize[0] * DPI + 36) / 72), hh = (int)((h->PageSize[1] * DPI + 36) / 72);
  const geometry_t *best = &geometries[0];
  int best_err = 1 << 30;
  size_t i;

  for (i = 0; i < sizeof(geometries) / sizeof(geometries[0]); i++) {
    int err = abs(geometries[i].paper_w - w) + abs(geometries[i].paper_h - hh);
    if (err < best_err) {
      best_err = err;
      best = &geometries[i];
    }
  }
  if (best_err > 60) /* > 0.1 inch off: unknown size, derive Canon-style margins */
  {
    static geometry_t custom;
    custom.name = "Custom";
    custom.paper_w = w;
    custom.paper_h = hh;
    custom.left = 80;
    custom.top = 70;
    custom.area_w = w - 160 > MAX_WIDTH_DOTS ? MAX_WIDTH_DOTS : w - 160;
    custom.area_h = hh - 188;
    custom.flag = 0;
    if (w - 160 > MAX_WIDTH_DOTS)
      custom.left = (w - MAX_WIDTH_DOTS) / 2;
    fprintf(stderr, "DEBUG: No exact paper match for %dx%d dots; using custom geometry\n", w, hh);
    return &custom;
  }
  return best;
}


/* ---------- band encoder: error diffusion, dot packing, 16-line blocks ---------- */

#define MAX_CH 4

typedef struct {
  int nch;                  /* 1 (K) or 4 (C,M,Y,K) */
  const char *inks;         /* ESC (L argument */
  int levels[MAX_CH];       /* 3 = ternary (0/1/2 drops, 5 dots per byte), 2 = 1 bit */
  int width;
  int *err_cur[MAX_CH], *err_next[MAX_CH];
  unsigned char *packed;    /* scratch for one packed line */
  buf_t band[MAX_CH];       /* current band, per channel */
  int band_has_ink[MAX_CH];
  int band_lines, skip, sent_inks;
  int row;                  /* rows encoded so far (serpentine direction) */
  unsigned rng;             /* threshold noise */
  buf_t body;               /* encoded bands for this page */
} enc_t;

static void enc_init(enc_t *e, const char *inks, int width)
{
  int c;
  memset(e, 0, sizeof(*e));
  e->nch = (int)strlen(inks);
  e->inks = inks;
  e->width = width;
  e->rng = 0x1234567u;
  for (c = 0; c < e->nch; c++) {
    e->levels[c] = (inks[c] == 'C' || inks[c] == 'M') ? 3 : 2;
    e->err_cur[c] = calloc(width + 2, sizeof(int));
    e->err_next[c] = calloc(width + 2, sizeof(int));
  }
  e->packed = malloc(width / 5 + 8 > width / 8 + 8 ? width / 5 + 8 : width / 8 + 8);
}

static void enc_free(enc_t *e)
{
  int c;
  for (c = 0; c < e->nch; c++) {
    free(e->err_cur[c]);
    free(e->err_next[c]);
    free(e->band[c].data);
  }
  free(e->packed);
  free(e->body.data);
}

static void enc_flush_band(enc_t *e)
{
  int c, any = 0;
  for (c = 0; c < e->nch; c++)
    any |= e->band_has_ink[c];
  if (any) {
    if (e->skip) {
      canon_skip(&e->body, e->skip);
      e->skip = 0;
    }
    if (!e->sent_inks) {
      canon_cmd(&e->body, 'L', (const unsigned char *)e->inks, e->nch);
      e->sent_inks = 1;
    }
    for (c = 0; c < e->nch; c++) {
      /* A channel with no ink in this band is sent as an empty block, like Canon does. */
      if (e->band_has_ink[c])
        canon_cmd(&e->body, 'F', e->band[c].data, (unsigned)e->band[c].len);
      else
        canon_cmd(&e->body, 'F', NULL, 0);
    }
  } else
    e->skip++;
  for (c = 0; c < e->nch; c++) {
    e->band[c].len = 0;
    e->band_has_ink[c] = 0;
  }
  e->band_lines = 0;
}

/* amount[c][x]: ink per dot in 1/1000 drop (0..2000 for ternary channels, 0..1000 otherwise). */
static void enc_row(enc_t *e, int *const amount[MAX_CH])
{
  int c, i, n;
  for (c = 0; c < e->nch; c++) {
    int *ec = e->err_cur[c], *en = e->err_next[c], *tmp;
    const int *a = amount[c];
    int ternary = e->levels[c] == 3;
    unsigned char *out = e->packed;
    int nbytes = ternary ? (e->width + 4) / 5 : (e->width + 7) / 8;

    memset(out, 0, nbytes);
    memset(en, 0, (e->width + 2) * sizeof(int));
    /* Floyd-Steinberg, serpentine (alternate direction each line) with a little threshold
       noise: without these, light tints show streaks and "worm" patterns. */
    for (i = 0; i < e->width; i++) {
      int x = (e->row & 1) ? e->width - 1 - i : i;
      int fwd = (e->row & 1) ? -1 : 1;
      int v = a[x] + (ec[x + 1] >> 4), q, err, jitter;
      e->rng = e->rng * 1103515245u + 12345u;
      jitter = (int)((e->rng >> 16) % 301) - 150;
      if (ternary)
        q = v < 500 + jitter ? 0 : v < 1500 + jitter ? 1 : 2;
      else
        q = v >= 500 + jitter;
      if (a[x] == 0 && v < 500)
        q = 0; /* never put ink where the page is white */
      err = v - q * 1000;
      if (q) {
        if (ternary) {
          static const int place[5] = { 81, 27, 9, 3, 1 };
          out[x / 5] += (unsigned char)(q * place[x % 5]);
        } else
          out[x >> 3] |= 0x80 >> (x & 7);
      }
      ec[x + 1 + fwd] += err * 7;
      en[x + 1 - fwd] += err * 3;
      en[x + 1] += err * 5;
      en[x + 1 + fwd] += err;
    }
    tmp = e->err_cur[c];
    e->err_cur[c] = e->err_next[c];
    e->err_next[c] = tmp;

    n = nbytes;
    while (n > 0 && out[n - 1] == 0)
      n--;
    packbits_line(&e->band[c], out, n);
    e->band_has_ink[c] |= n > 0;
  }
  e->row++;
  if (++e->band_lines == LINES_PER_BLOCK)
    enc_flush_band(e);
}

static void enc_finish(enc_t *e)
{
  int c, any = 0;
  if (!e->band_lines)
    return;
  for (c = 0; c < e->nch; c++)
    any |= e->band_has_ink[c];
  if (!any)
    return; /* trailing blank lines are simply not sent */
  while (e->band_lines < LINES_PER_BLOCK) {
    for (c = 0; c < e->nch; c++)
      if (e->band_has_ink[c])
        buf_byte(&e->band[c], 0x80);
    e->band_lines++;
  }
  e->band_lines = LINES_PER_BLOCK;
  enc_flush_band(e);
}

/* ---------- colour conversion (table built from Canon's plain-paper output) ---------- */

static void lut_ink(int r, int g, int b, int out[4])
{
  const int N1 = IP100_LUT_N - 1;
  int fr = r * N1, fg = g * N1, fb = b * N1;
  int ri = fr / 255, gi = fg / 255, bi = fb / 255, c;
  int tr, tg, tb;
  if (ri >= N1) ri = N1 - 1;
  if (gi >= N1) gi = N1 - 1;
  if (bi >= N1) bi = N1 - 1;
  tr = fr - ri * 255; tg = fg - gi * 255; tb = fb - bi * 255; /* 0..255 */
#define L(i, j, k) ip100_plain_lut[((ri + (i)) * IP100_LUT_N + (gi + (j))) * IP100_LUT_N + (bi + (k))]
  for (c = 0; c < 4; c++) {
    int c00 = L(0,0,0)[c] * (255 - tb) + L(0,0,1)[c] * tb;
    int c01 = L(0,1,0)[c] * (255 - tb) + L(0,1,1)[c] * tb;
    int c10 = L(1,0,0)[c] * (255 - tb) + L(1,0,1)[c] * tb;
    int c11 = L(1,1,0)[c] * (255 - tb) + L(1,1,1)[c] * tb;
    int c0 = (c00 / 255) * (255 - tg) + (c01 / 255) * tg;
    int c1 = (c10 / 255) * (255 - tg) + (c11 / 255) * tg;
    out[c] = ((c0 / 255) * (255 - tr) + (c1 / 255) * tr) / 255;
  }
#undef L
  if (r == 0 && g == 0 && b == 0) { /* pure black: black ink only, like Canon's text */
    out[0] = out[1] = out[2] = 0;
    out[3] = 950;
  }
}

static volatile sig_atomic_t canceled = 0;
static void on_term(int sig) { (void)sig; canceled = 1; }

int main(int argc, char *argv[])
{
  int fd = 0, page_no = 0;
  cups_raster_t *ras;
  cups_page_header2_t h;
  buf_t page = { 0 };
  size_t last_flag = 0;
  int have_page = 0;
  int density = 95; /* max black coverage in percent; Canon's driver uses about 95% */
  int force_mono = 0;
  int num_options;
  cups_option_t *options = NULL;
  const char *val;

  if (argc < 6 || argc > 7) {
    fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
    return 1;
  }
  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
    perror("ERROR: Unable to open raster file");
    return 1;
  }
  signal(SIGTERM, on_term);

  num_options = cupsParseOptions(argv[5], 0, &options);
  if ((val = cupsGetOption("InkDensity", num_options, options)) != NULL && atoi(val) >= 50 && atoi(val) <= 100)
    density = atoi(val);
  if ((val = cupsGetOption("ColorModel", num_options, options)) != NULL && !strcasecmp(val, "Gray"))
    force_mono = 1;
  cupsFreeOptions(num_options, options);
  fprintf(stderr, "DEBUG: rastertoip100 %s, ink density %d%%%s\n", IP100_VERSION, density, force_mono ? ", black only" : "");

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  job_start();

  while (!canceled && cupsRasterReadHeader2(ras, &h)) {
    const geometry_t *g = find_geometry(&h);
    unsigned bpp = h.cupsBitsPerPixel / 8;
    int x0, y0, width, y, x, c;
    int color_input, neutral = 1;
    unsigned char *in;
    int *amt[MAX_CH], *kamt[MAX_CH] = { 0 }; /* colour inks C,M,Y,K; black-only K */
    enc_t mono, color;

    if (h.cupsBitsPerColor != 8 || (bpp != 1 && bpp != 3)) {
      fprintf(stderr, "ERROR: Unsupported raster format (%u bits/color, %u bits/pixel)\n",
              h.cupsBitsPerColor, h.cupsBitsPerPixel);
      return 1;
    }

    /* Flush the previous page now that we know it isn't the last one. */
    if (have_page) {
      write_all(page.data, page.len);
      page.len = 0;
    }

    page_no++;
    fprintf(stderr, "PAGE: %d 1\n", page_no);
    fprintf(stderr, "DEBUG: Page %d: %s, raster %ux%u @ %ux%u dpi, %u bpp\n", page_no, g->name,
            h.cupsWidth, h.cupsHeight, h.HWResolution[0], h.HWResolution[1], h.cupsBitsPerPixel);

    /* Raster normally covers the printable area; if it covers the whole sheet, crop to it. */
    x0 = (int)h.cupsWidth >= g->paper_w - 2 ? g->left : 0;
    y0 = (int)h.cupsHeight >= g->paper_h - 2 ? g->top : 0;
    width = g->area_w;
    color_input = bpp == 3 && !force_mono;

    in = malloc(h.cupsBytesPerLine);
    for (c = 0; c < MAX_CH; c++)
      amt[c] = calloc(width, sizeof(int));
    kamt[0] = calloc(width, sizeof(int));
    enc_init(&mono, "K", width);
    if (color_input)
      enc_init(&color, "CMYK", width);

    last_flag = page_header(&page, g, page_no);

    for (y = 0; y < (int)h.cupsHeight; y++) {
      int sy = y - y0;
      int lr = -1, lg = -1, lb = -1, ink[4] = { 0 };

      if (cupsRasterReadPixels(ras, in, h.cupsBytesPerLine) == 0)
        break;
      if (sy < 0 || sy >= g->area_h)
        continue;

      for (x = 0; x < width; x++) {
        int sx = x + x0, r, gg, b, v;
        if (sx >= (int)h.cupsWidth)
          r = gg = b = 255;
        else if (bpp == 1)
          r = gg = b = h.cupsColorSpace == CUPS_CSPACE_K ? 255 - in[sx] : in[sx];
        else {
          r = in[sx * 3];
          gg = in[sx * 3 + 1];
          b = in[sx * 3 + 2];
        }

        /* Black-only encoding: darkness scaled by ink density. */
        v = (r * 30 + gg * 59 + b * 11) / 100;
        kamt[0][x] = ((255 - v) * density * 1000) / (100 * 255);

        if (color_input) {
          if (r != gg || gg != b)
            neutral = 0;
          if (r != lr || gg != lg || b != lb) {
            if (r == 255 && gg == 255 && b == 255)
              ink[0] = ink[1] = ink[2] = ink[3] = 0;
            else
              lut_ink(r, gg, b, ink);
            lr = r; lg = gg; lb = b;
          }
          for (c = 0; c < 4; c++)
            amt[c][x] = ink[c] * density / 95;
        }
      }
      enc_row(&mono, kamt);
      if (color_input)
        enc_row(&color, amt);
    }
    enc_finish(&mono);
    if (color_input)
      enc_finish(&color);

    if (color_input && !neutral) {
      fprintf(stderr, "DEBUG: Page %d printed in colour\n", page_no);
      buf_put(&page, color.body.data, color.body.len);
    } else {
      fprintf(stderr, "DEBUG: Page %d printed black only\n", page_no);
      buf_put(&page, mono.body.data, mono.body.len);
    }
    canon_skip(&page, 1);
    buf_byte(&page, 0x0c); /* form feed: eject */
    have_page = 1;

    enc_free(&mono);
    if (color_input)
      enc_free(&color);
    for (c = 0; c < MAX_CH; c++)
      free(amt[c]);
    free(kamt[0]);
    free(in);
  }

  if (have_page) {
    page.data[last_flag] = 0x01; /* ESC (s 1 marks the final page */
    buf_put(&page, "\x1b(b\x01\x00\x00", 6);
    write_all(page.data, page.len);
  }
  job_end();

  cupsRasterClose(ras);
  if (fd)
    close(fd);
  free(page.data);
  return page_no ? 0 : 1;
}
