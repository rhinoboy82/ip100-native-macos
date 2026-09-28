/*
 * rastertoip100 - native CUPS filter for the Canon PIXMA iP100.
 *
 * Reads 8-bit RGB or grayscale CUPS raster and writes the Canon command stream that
 * Canon's own (Intel-only) Mac driver produces. Command sequence, page geometry and
 * the RGB -> ink tables were derived from that driver's output; see docs/ and tools/.
 * Plain paper (standard, draft, fast, super fine), envelopes and photo papers, with
 * borderless printing on photo papers. On plain paper, pages with no colour are printed
 * with black ink only, as Canon's driver does.
 * Also reports ink levels from the printer's status replies (after each job, and for the
 * CUPS "ReportLevels" command).
 *
 * Usage (as a CUPS filter): rastertoip100 job user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/sidechannel.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ip100_plain_lut.h"
#include "ip100_draft_lut.h"
#include "ip100_superfine_lut.h"
#include "ip100_envelope_lut.h"
#include "ip100_glossy_lut.h"
#include "ip100_glossy2_lut.h"
#include "ip100_pro_lut.h"
#include "ip100_matte_lut.h"

#define IP100_VERSION   "1.2.0"
#define DPI             600
#define LINES_PER_BLOCK 16 /* 600 dpi; draft (300 dpi) uses 8 */
#define MAX_WIDTH_DOTS  4800 /* 8 inch print head sweep */

/* Page geometry per paper size, in 600 dpi dots, exactly as Canon's driver sends it. */
typedef struct {
  const char *name;
  int paper_w, paper_h;   /* physical paper size */
  int left, top;          /* printable area origin */
  int area_w, area_h;     /* printable area size */
  int flag;               /* 7 for Letter/Legal (area narrower than paper), else 0 */
  int borderless;         /* image extends past the paper edges (photo papers only) */
} geometry_t;

static const geometry_t geometries[] = {
  { "Letter", 5100, 6600, 151, 70, 4800, 6412, 7, 0 },
  { "Legal",  5100, 8400, 151, 70, 4800, 8212, 7, 0 },
  { "A4",     4961, 7016,  80, 70, 4800, 6827, 0, 0 },
  { "A5",     3497, 4961,  80, 70, 3336, 4772, 0, 0 },
  { "B5",     4300, 6071,  80, 70, 4139, 5882, 0, 0 },
  { "4x6",    2400, 3600,  80, 70, 2240, 3412, 0, 0 },
  { "5x7",    3000, 4200,  80, 70, 2840, 4012, 0, 0 },
  { "8x10",   4800, 6000,  80, 70, 4640, 5812, 0, 0 },
  { "Env10",  2475, 5700,  80, 70, 2315, 5004, 0, 0 },
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

/* ---------- printer status (USB back-channel) -> ink levels in macOS ---------- */

static char status_buf[4096];
static size_t status_len = 0;
static int have_backchannel = 0; /* fd 3 open at startup (always true under CUPS) */

/* Pass the printer's ink levels (CIR:CL=nnn,BK=nnn) and low-ink flags (CTK) to CUPS. */
static int report_levels(void)
{
  const char *cir = NULL, *p = status_buf;
  int cl = -1, bk = -1, low_cl = 0, low_bk = 0;
  const char *q;

  while ((q = strstr(p, "CIR:")) != NULL) { /* use the most recent reply */
    cir = q;
    p = q + 4;
  }
  if (!cir || sscanf(cir, "CIR:CL=%d,BK=%d", &cl, &bk) != 2)
    return 0;
  if ((q = strstr(status_buf + (cir - status_buf > 600 ? cir - status_buf - 600 : 0), "CTK:")) != NULL) {
    const char *end = strchr(q, ';');
    size_t n = end ? (size_t)(end - q) : strlen(q);
    char ctk[80];
    snprintf(ctk, sizeof(ctk), "%.*s", (int)(n < sizeof(ctk) - 1 ? n : sizeof(ctk) - 1), q);
    /* e.g. "CTK:CL,IO,/,BK,LOW" or "CTK:CL,SET,/,BK,SET" */
    low_cl = strstr(ctk, "CL,LOW") != NULL || strstr(ctk, "CL,IO") != NULL;
    low_bk = strstr(ctk, "BK,LOW") != NULL || strstr(ctk, "BK,IO") != NULL;
  }
  fputs("ATTR: marker-names=Black,Color\n", stderr);
  fputs("ATTR: marker-colors=#000000,#00FFFF#FF00FF#FFFF00\n", stderr);
  fputs("ATTR: marker-types=ink-cartridge,ink-cartridge\n", stderr);
  fprintf(stderr, "ATTR: marker-levels=%d,%d\n", bk, cl);
  fputs("ATTR: marker-low-levels=10,10\n", stderr);
  fprintf(stderr, "STATE: %cmarker-supply-low-warning\n", (low_cl || low_bk) ? '+' : '-');
  fprintf(stderr, "DEBUG: Ink levels: black %d%%, colour %d%%\n", bk, cl);
  return 1;
}

/* Collect back-channel data (printer replies on fd 3) for up to `timeout` seconds; 0 = only
   what is already there. While a job is open the printer streams status continuously, so stop
   as soon as a complete ink-level field (CIR:...;) has arrived. Uses poll() directly: the libcups
   read helper does not honour its timeout on an idle channel. */
static void read_status(double timeout)
{
  char chunk[1024];
  struct timeval start, now;
  if (!have_backchannel)
    return;
  gettimeofday(&start, NULL);
  for (;;) {
    struct pollfd pfd = { 3, POLLIN, 0 };
    int elapsed_ms, wait_ms, rc;
    ssize_t n;
    gettimeofday(&now, NULL);
    elapsed_ms = (int)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000);
    wait_ms = (int)(timeout * 1000) - elapsed_ms;
    if (wait_ms < 0)
      wait_ms = 0;
    rc = poll(&pfd, 1, wait_ms);
    if (rc <= 0 || (pfd.revents & (POLLNVAL | POLLERR)) || !(pfd.revents & (POLLIN | POLLHUP)))
      break; /* time up, or no back-channel */
    n = read(3, chunk, sizeof(chunk));
    if (n <= 0)
      break;
    if (status_len + (size_t)n >= sizeof(status_buf)) { /* keep the newest data */
      size_t keep = sizeof(status_buf) / 2;
      memmove(status_buf, status_buf + status_len - keep, keep);
      status_len = keep;
    }
    memcpy(status_buf + status_len, chunk, (size_t)n);
    status_len += (size_t)n;
    status_buf[status_len] = '\0';
    {
      const char *cir = strstr(status_buf, "CIR:");
      if (cir && strchr(cir, ';'))
        break;
    }
  }
}

/* ---------- maintenance commands (CUPS command files) ---------- */

/* Handles CUPS command files; only "ReportLevels" is recognised, and it is ignored (see below). */
static int run_commands(int fd)
{
  FILE *f = fdopen(fd, "r");
  char line[256];
  int ok = 1;
  if (!f) {
    perror("ERROR: Unable to read command file");
    return 1;
  }
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (!line[0] || line[0] == '#')
      continue;
    fprintf(stderr, "DEBUG: Command \"%s\"\n", line);
    /* Nozzle check and cleaning are not supported yet: the command format the printer
       expects is unknown, and guessed ones left it waiting for data. Use the printer's
       RESUME/CANCEL button instead. */
    if (!strncasecmp(line, "ReportLevels", 12)) {
      /* Deliberately a no-op: a status request outside a print job leaves the iP100 waiting
         for a job (power light flashing) until it is switched off. Ink levels are reported
         at the end of every print job instead. (PPDs from 1.1/1.2 advertised this command.) */
      fputs("DEBUG: ReportLevels ignored; ink levels are updated after each print job\n", stderr);
    } else {
      fprintf(stderr, "WARNING: Unsupported command \"%s\"\n", line);
      ok = 0;
    }
  }
  fclose(f);
  return ok ? 0 : 1;
}

/* ---------- print modes ---------- */

/* How one ESC (L data channel is derived from a logical ink's per-dot level. */
enum { P_BIT = 1, P_2BIT = 2, P_TERN = 3 }; /* 8, 4 or 5 dots per byte */

typedef struct {
  unsigned char code; /* ESC (L channel byte */
  int src;            /* logical ink: 0 C, 1 M, 2 Y, 3 K/k */
  int kind;
  unsigned char map[6]; /* level -> value written for this channel */
} plane_t;

typedef struct {
  const char *name;
  int sc;                       /* 1 = 600 dpi, 2 = 300 dpi */
  unsigned char t[15];          /* ESC (t (first 15 bytes; rest zero) */
  unsigned char c2;             /* ESC (c 30 <media> <c2> */
  int maxlevel[4];              /* per logical ink C, M, Y, K/k */
  int nplanes;
  plane_t planes[9];
  const unsigned short (*lut)[4];
  int mono_pages;               /* send pages without colour as black only (plain paper) */
} printmode_t;

/* Canon's per-dot level codes for split channels (see docs/PROTOCOL.md). */
#define CM_MAIN  { 0, 0, 0, 1, 2, 3 }
#define CM_40    { 0, 0, 1, 1, 1, 1 }
#define CM_80    { 0, 1, 1, 0, 0, 0 }
#define ID       { 0, 1, 2, 3, 4, 5 }

static const printmode_t mode_standard = { "standard", 1,
  { 0x80, 0x80, 0x01, 0x22, 0x00, 0x03, 0x22, 0x00, 0x03, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02 }, 0x03,
  { 2, 2, 1, 1 }, 4,
  { { 'C', 0, P_TERN, ID }, { 'M', 1, P_TERN, ID }, { 'Y', 2, P_BIT, ID }, { 'K', 3, P_BIT, ID } },
  ip100_plain_lut, 1 };

static const printmode_t mode_draft = { "draft", 2,
  { 0x80, 0x00, 0x01, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02 }, 0x01,
  { 1, 1, 1, 1 }, 4,
  { { 'C', 0, P_BIT, ID }, { 'M', 1, P_BIT, ID }, { 'Y', 2, P_BIT, ID }, { 'K', 3, P_BIT, ID } },
  ip100_draft_lut, 1 };

/* Super Fine plain paper and envelopes: C/M six levels, Y four, pigment black 1 bit. */
#define SF_T { 0x80, 0x80, 0x01, 0x02, 0x00, 0x04, 0x02, 0x00, 0x04, 0x02, 0x00, 0x04, 0x01, 0x00, 0x02 }
#define SF_PLANES { { 'C', 0, P_2BIT, CM_MAIN }, { 'M', 1, P_2BIT, CM_MAIN }, { 'Y', 2, P_2BIT, ID }, \
    { 'K', 3, P_BIT, ID }, { 0x83, 0, P_BIT, CM_40 }, { 0x8d, 1, P_BIT, CM_40 }, \
    { 0xc3, 0, P_BIT, CM_80 }, { 0xcd, 1, P_BIT, CM_80 } }
static const printmode_t mode_superfine = { "super fine", 1, SF_T, 0x04, { 5, 5, 3, 1 }, 8, SF_PLANES, ip100_superfine_lut, 0 };
static const printmode_t mode_envelope = { "envelope", 1, SF_T, 0x03, { 5, 5, 3, 1 }, 8, SF_PLANES, ip100_envelope_lut, 0 };

/* Photo papers: C/M six levels, Y four, dye black (k) four. */
#define PH_T { 0x80, 0x80, 0x01, 0x02, 0x00, 0x04, 0x02, 0x00, 0x04, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00 }
#define PH_PLANES { { 'C', 0, P_2BIT, CM_MAIN }, { 'M', 1, P_2BIT, CM_MAIN }, { 'Y', 2, P_2BIT, ID }, \
    { 'k', 3, P_TERN, { 0, 0, 1, 2 } }, { 0x83, 0, P_BIT, CM_40 }, { 0x8d, 1, P_BIT, CM_40 }, \
    { 0xab, 3, P_BIT, { 0, 1, 1, 1 } }, { 0xc3, 0, P_BIT, CM_80 }, { 0xcd, 1, P_BIT, CM_80 } }
static const printmode_t mode_glossy = { "photo glossy", 1, PH_T, 0x03, { 5, 5, 3, 3 }, 9, PH_PLANES, ip100_glossy_lut, 0 };
static const printmode_t mode_glossy2 = { "photo glossy II", 1, PH_T, 0x03, { 5, 5, 3, 3 }, 9, PH_PLANES, ip100_glossy2_lut, 0 };
static const printmode_t mode_pro = { "photo pro", 1, PH_T, 0x03, { 5, 5, 3, 3 }, 9, PH_PLANES, ip100_pro_lut, 0 };
static const printmode_t mode_matte = { "photo matte", 1, PH_T, 0x03, { 5, 5, 3, 3 }, 9, PH_PLANES, ip100_matte_lut, 0 };

/* The rest of the photo-mode ESC (t (bytes 15..35). */
static const unsigned char photo_t_tail[21] = { 0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x22, 0x00, 0x03,
                                                0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02 };
static const unsigned char sf_t_tail[21] = { 0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00 };

/* Media types (PPD MediaType) with Canon's ESC (c and ESC (l codes. */
typedef struct {
  const char *name;
  unsigned char c_media, l_media;
  const printmode_t *mode;
} media_t;

static const media_t media_types[] = {
  { "Plain",              0x00, 0x00, &mode_standard },
  { "Envelope",           0x08, 0x08, &mode_envelope },
  { "PhotoPro",           0x09, 0x0d, &mode_pro },
  { "PhotoPlusGlossyII",  0x1d, 0x23, &mode_glossy2 },
  { "PhotoPlusGlossy",    0x0b, 0x11, &mode_glossy },
  { "PhotoPlusSemiGloss", 0x1a, 0x1f, &mode_glossy },
  { "GlossyPhoto",        0x05, 0x05, &mode_glossy },
  { "Matte",              0x0a, 0x10, &mode_matte },
  { "HighRes",            0x07, 0x07, &mode_matte },
};

typedef enum { Q_STANDARD, Q_DRAFT, Q_FAST, Q_HIGH } quality_t;

/* Returns the offset of the ESC (s last-page flag's value inside the page buffer. */
static size_t page_header(buf_t *b, const geometry_t *g, int page_no, const media_t *md, const printmode_t *m, quality_t q)
{
  unsigned char t[36] = { 0 };
  unsigned char p[46] = { 0 };
  unsigned char d[4] = { 0x02, 0x58, 0x02, 0x58 };
  unsigned char c[3] = { 0x30, md->c_media, m->c2 };
  unsigned char l[2] = { 0x34, md->l_media };
  unsigned char dollar[5] = { 0x01, 0, 0, 0, 0 };
  size_t last_flag;

  memcpy(t, m->t, 15);
  if (m->planes[3].code == 'k')
    memcpy(t + 15, photo_t_tail, 21);
  else if (m->nplanes == 8)
    memcpy(t + 15, sf_t_tail, 21);
  if (m == &mode_draft) {
    d[0] = d[2] = 0x01; /* 300 dpi */
    d[1] = d[3] = 0x2c;
    c[2] = q == Q_FAST ? 0x00 : 0x01;
  }

  if (page_no == 1)
    canon_cmd1(b, 'b', 0x01);
  canon_cmd(b, 'd', d, 4);
  canon_cmd(b, 't', t, 36);
  canon_cmd(b, 'c', c, 3);

  if (!g->borderless) {
    put_be16(p + 0, (g->area_h + 9) / 10);
    put_be16(p + 4, (g->area_w + 9) / 10);
    put_be16(p + 6, g->flag);
  }
  put_be16(p + 12, DPI);
  put_be32(p + 14, (unsigned)g->left);
  put_be32(p + 18, (unsigned)g->top);
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
  canon_cmd1(b, 'J', LINES_PER_BLOCK / m->sc);
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

static const geometry_t *find_geometry(const cups_page_header2_t *h, int allow_borderless)
{
  static geometry_t custom;
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
  if (allow_borderless && strstr(h->cupsPageSizeName, "FullBleed")) {
    /* Borderless: Canon prints an area 47 dots past the top/left edges, 72 past the right and
       119 past the bottom, scaling the page image up to fill it. */
    custom = *best;
    if (best_err > 60) {
      custom.paper_w = w;
      custom.paper_h = hh;
    }
    custom.name = "Borderless";
    custom.left = custom.top = -47;
    custom.area_w = custom.paper_w + 119;
    custom.area_h = custom.paper_h + 166;
    custom.flag = 0;
    custom.borderless = 1;
    return &custom;
  }
  if (best_err > 60) /* > 0.1 inch off: unknown size, derive Canon-style margins */
  {
    custom.name = "Custom";
    custom.paper_w = w;
    custom.paper_h = hh;
    custom.left = 80;
    custom.top = 70;
    custom.area_w = w - 160 > MAX_WIDTH_DOTS ? MAX_WIDTH_DOTS : w - 160;
    custom.area_h = hh - 188;
    custom.flag = 0;
    custom.borderless = 0;
    if (w - 160 > MAX_WIDTH_DOTS) { /* wider than the print head sweep: centre it, like Letter */
      custom.left = (w - MAX_WIDTH_DOTS + 1) / 2;
      custom.flag = 7;
    }
    fprintf(stderr, "DEBUG: No exact paper match for %dx%d dots; using custom geometry\n", w, hh);
    return &custom;
  }
  return best;
}

/* ---------- band encoder: multi-level error diffusion, channel packing, bands ---------- */

#define MAX_INK 4

typedef struct {
  const printmode_t *m;
  int nplanes;
  const plane_t *planes;
  plane_t mono_plane;       /* black-only encoder: one K channel */
  int width, lines_per_block;
  int *err_cur[MAX_INK], *err_next[MAX_INK];
  unsigned char *level[MAX_INK]; /* quantized level per dot, current line */
  unsigned char *packed;    /* scratch for one packed line */
  buf_t band[9];            /* current band, per channel */
  int band_has_ink[9];
  int band_lines, skip, sent_inks;
  int row;                  /* rows encoded so far (serpentine direction) */
  unsigned rng;             /* threshold noise */
  buf_t body;               /* encoded bands for this page */
} enc_t;

/* mono != 0: a black-only encoder (one 1-bit K channel) for pages with no colour. */
static void enc_init(enc_t *e, const printmode_t *m, int width, int mono)
{
  int c;
  memset(e, 0, sizeof(*e));
  e->m = m;
  if (mono) {
    plane_t k = { 'K', 3, P_BIT, ID };
    e->mono_plane = k;
    e->planes = &e->mono_plane;
    e->nplanes = 1;
  } else {
    e->planes = m->planes;
    e->nplanes = m->nplanes;
  }
  e->width = width;
  e->lines_per_block = LINES_PER_BLOCK / m->sc;
  e->rng = 0x1234567u;
  for (c = 0; c < MAX_INK; c++) {
    e->err_cur[c] = calloc(width + 2, sizeof(int));
    e->err_next[c] = calloc(width + 2, sizeof(int));
    e->level[c] = calloc(width, 1);
  }
  e->packed = malloc(width / 4 + 8);
}

static void enc_free(enc_t *e)
{
  int c;
  for (c = 0; c < MAX_INK; c++) {
    free(e->err_cur[c]);
    free(e->err_next[c]);
    free(e->level[c]);
  }
  for (c = 0; c < 9; c++)
    free(e->band[c].data);
  free(e->packed);
  free(e->body.data);
}

static void enc_flush_band(enc_t *e)
{
  int c, any = 0;
  for (c = 0; c < e->nplanes; c++)
    any |= e->band_has_ink[c];
  if (any) {
    if (e->skip) {
      canon_skip(&e->body, e->skip);
      e->skip = 0;
    }
    if (!e->sent_inks) {
      unsigned char inks[9];
      for (c = 0; c < e->nplanes; c++)
        inks[c] = e->planes[c].code;
      canon_cmd(&e->body, 'L', inks, e->nplanes);
      e->sent_inks = 1;
    }
    for (c = 0; c < e->nplanes; c++) {
      /* A channel with no ink in this band is sent as an empty block, like Canon does. */
      if (e->band_has_ink[c])
        canon_cmd(&e->body, 'F', e->band[c].data, (unsigned)e->band[c].len);
      else
        canon_cmd(&e->body, 'F', NULL, 0);
    }
  } else
    e->skip++;
  for (c = 0; c < e->nplanes; c++) {
    e->band[c].len = 0;
    e->band_has_ink[c] = 0;
  }
  e->band_lines = 0;
}

/* Quantize one logical ink to levels 0..maxlevel: serpentine Floyd-Steinberg with a little
   threshold noise (plain FS shows streaks and "worm" patterns in light tints).
   amount: ink per dot in 1/1000 level. */
static void quantize(enc_t *e, int c, const int *amount, int maxlevel)
{
  int *ec = e->err_cur[c], *en = e->err_next[c], *tmp;
  unsigned char *lv = e->level[c];
  int i;
  memset(en, 0, (e->width + 2) * sizeof(int));
  for (i = 0; i < e->width; i++) {
    int x = (e->row & 1) ? e->width - 1 - i : i;
    int fwd = (e->row & 1) ? -1 : 1;
    int v = amount[x] + (ec[x + 1] >> 4), q, err, jitter;
    e->rng = e->rng * 1103515245u + 12345u;
    jitter = (int)((e->rng >> 16) % 301) - 150;
    q = (v + 500 - jitter) / 1000;
    if (v + 500 - jitter < 0)
      q = 0;
    if (q > maxlevel)
      q = maxlevel;
    if (amount[x] == 0 && v < 500)
      q = 0; /* never put ink where the page is white */
    lv[x] = (unsigned char)q;
    err = v - q * 1000;
    ec[x + 1 + fwd] += err * 7;
    en[x + 1 - fwd] += err * 3;
    en[x + 1] += err * 5;
    en[x + 1 + fwd] += err;
  }
  tmp = e->err_cur[c];
  e->err_cur[c] = e->err_next[c];
  e->err_next[c] = tmp;
}

/* amount[ink][x] for logical inks C, M, Y, K/k (only [3] is used by a black-only encoder). */
static void enc_row(enc_t *e, int *const amount[MAX_INK])
{
  int c, x, p;
  int used[MAX_INK] = { 0 };
  for (p = 0; p < e->nplanes; p++)
    used[e->planes[p].src] = 1;
  for (c = 0; c < MAX_INK; c++)
    if (used[c])
      quantize(e, c, amount[c], e->nplanes == 1 ? 1 : e->m->maxlevel[c]);

  for (p = 0; p < e->nplanes; p++) {
    const plane_t *pl = &e->planes[p];
    const unsigned char *lv = e->level[pl->src];
    unsigned char *out = e->packed;
    int per = pl->kind == P_BIT ? 8 : pl->kind == P_2BIT ? 4 : 5;
    int nbytes = (e->width + per - 1) / per, n;
    memset(out, 0, nbytes);
    for (x = 0; x < e->width; x++) {
      int v = pl->map[lv[x]];
      if (!v)
        continue;
      if (pl->kind == P_BIT)
        out[x >> 3] |= 0x80 >> (x & 7);
      else if (pl->kind == P_2BIT)
        out[x >> 2] |= (unsigned char)(v << (6 - 2 * (x & 3)));
      else {
        static const int place[5] = { 81, 27, 9, 3, 1 };
        out[x / 5] += (unsigned char)(v * place[x % 5]);
      }
    }
    n = nbytes;
    while (n > 0 && out[n - 1] == 0)
      n--;
    packbits_line(&e->band[p], out, n);
    e->band_has_ink[p] |= n > 0;
  }
  e->row++;
  if (++e->band_lines == e->lines_per_block)
    enc_flush_band(e);
}

static void enc_finish(enc_t *e)
{
  int c, any = 0;
  if (!e->band_lines)
    return;
  for (c = 0; c < e->nplanes; c++)
    any |= e->band_has_ink[c];
  if (!any)
    return; /* trailing blank lines are simply not sent */
  while (e->band_lines < e->lines_per_block) {
    for (c = 0; c < e->nplanes; c++)
      if (e->band_has_ink[c])
        buf_byte(&e->band[c], 0x80);
    e->band_lines++;
  }
  enc_flush_band(e);
}

/* ---------- colour conversion (tables built from Canon's output) ---------- */

static void lut_ink(const unsigned short (*lut)[4], int r, int g, int b, int out[4])
{
  const int N1 = IP100_LUT_N - 1;
  int fr = r * N1, fg = g * N1, fb = b * N1;
  int ri = fr / 255, gi = fg / 255, bi = fb / 255, c;
  int tr, tg, tb;
  if (ri >= N1) ri = N1 - 1;
  if (gi >= N1) gi = N1 - 1;
  if (bi >= N1) bi = N1 - 1;
  tr = fr - ri * 255; tg = fg - gi * 255; tb = fb - bi * 255; /* 0..255 */
#define L(i, j, k) lut[((ri + (i)) * IP100_LUT_N + (gi + (j))) * IP100_LUT_N + (bi + (k))]
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
  char quality_opt[32] = ""; /* PrintQuality from the job options (PPDs before 1.2 set no OutputType) */
  int num_options;
  cups_option_t *options = NULL;
  const char *val;

  if (argc < 6 || argc > 7) {
    fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
    return 1;
  }
  /* Check for the back-channel before opening the input, which could otherwise become fd 3. */
  have_backchannel = fcntl(3, F_GETFD) != -1;
  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
    perror("ERROR: Unable to open raster file");
    return 1;
  }
  signal(SIGTERM, on_term);

  if ((val = getenv("CONTENT_TYPE")) != NULL && !strcmp(val, "application/vnd.cups-command"))
    return run_commands(fd);

  num_options = cupsParseOptions(argv[5], 0, &options);
  if ((val = cupsGetOption("InkDensity", num_options, options)) != NULL && atoi(val) >= 50 && atoi(val) <= 100)
    density = atoi(val);
  if ((val = cupsGetOption("ColorModel", num_options, options)) != NULL && !strcasecmp(val, "Gray"))
    force_mono = 1;
  if ((val = cupsGetOption("PrintQuality", num_options, options)) != NULL)
    snprintf(quality_opt, sizeof(quality_opt), "%s", val);
  cupsFreeOptions(num_options, options);
  fprintf(stderr, "DEBUG: rastertoip100 %s, ink density %d%%%s\n", IP100_VERSION, density, force_mono ? ", black only" : "");

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  job_start();

  while (!canceled && cupsRasterReadHeader2(ras, &h)) {
    const geometry_t *g;
    const media_t *md = &media_types[0];
    const char *qname;
    const printmode_t *m;
    unsigned bpp = h.cupsBitsPerPixel / 8;
    int x0, y0, width, height, y, x, c, sc, black1000, use_mono;
    int color_input, neutral = 1;
    int *xmap = NULL, last_sy = -1;
    quality_t q;
    size_t i;
    unsigned char *in;
    int *amt[MAX_INK], *kamt[MAX_INK] = { 0 }; /* colour inks; black-only K in [3] */
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
      read_status(0.0);
    }

    /* Media type and quality come from the page header (set by the PPD options). */
    for (i = 0; i < sizeof(media_types) / sizeof(media_types[0]); i++)
      if (!strcasecmp(h.MediaType, media_types[i].name))
        md = &media_types[i];
    qname = h.OutputType[0] ? h.OutputType : quality_opt;
    q = h.HWResolution[0] == 300 ? (!strcasecmp(qname, "Fast") ? Q_FAST : Q_DRAFT)
        : !strcasecmp(qname, "High") ? Q_HIGH : Q_STANDARD;
    m = md->mode;
    if (h.HWResolution[0] == 300 && m->sc == 1) {
      if (m != &mode_standard)
        fprintf(stderr, "WARNING: Draft quality is for plain paper; printing in plain-paper draft mode\n");
      md = &media_types[0];
      m = &mode_draft;
    } else if (m == &mode_standard && q == Q_HIGH)
      m = &mode_superfine;
    if (h.HWResolution[0] != (m->sc == 1 ? 600u : 300u)) {
      fprintf(stderr, "ERROR: Unsupported raster resolution %u dpi\n", h.HWResolution[0]);
      return 1;
    }
    /* Borderless only on photo papers, as in Canon's driver; otherwise use normal margins. */
    g = find_geometry(&h, !m->mono_pages && m != &mode_envelope);
    if (strstr(h.cupsPageSizeName, "FullBleed") && !g->borderless)
      fprintf(stderr, "WARNING: Borderless printing needs a photo paper type; printing with margins\n");
    sc = m->sc;
    /* Black-only darkness scale: Canon caps black at ~95% in standard mode, 100% in draft. */
    black1000 = sc == 1 ? density * 10 : (density * 1000 / 95 > 1000 ? 1000 : density * 1000 / 95);

    page_no++;
    fprintf(stderr, "PAGE: %d 1\n", page_no);
    fprintf(stderr, "DEBUG: Page %d: %s, %s, %s mode, raster %ux%u @ %ux%u dpi, %u bpp\n", page_no, g->name, md->name,
            m->name, h.cupsWidth, h.cupsHeight, h.HWResolution[0], h.HWResolution[1], h.cupsBitsPerPixel);

    width = g->area_w / sc;
    height = g->area_h / sc;
    if (g->borderless) {
      /* Scale the full-page raster up to the (larger) borderless print area. */
      x0 = y0 = 0;
      xmap = malloc(width * sizeof(int));
      for (x = 0; x < width; x++)
        xmap[x] = (int)((long)x * h.cupsWidth / width);
    } else {
      /* Raster normally covers the printable area; if it covers the whole sheet, crop to it. */
      x0 = (int)h.cupsWidth >= g->paper_w / sc - 2 ? g->left / sc : 0;
      y0 = (int)h.cupsHeight >= g->paper_h / sc - 2 ? g->top / sc : 0;
    }
    color_input = bpp == 3 && !(force_mono && m->mono_pages);

    in = malloc(h.cupsBytesPerLine);
    for (c = 0; c < MAX_INK; c++)
      amt[c] = calloc(width, sizeof(int));
    kamt[3] = calloc(width, sizeof(int));
    if (m->mono_pages)
      enc_init(&mono, m, width, 1);
    if (color_input)
      enc_init(&color, m, width, 0);

    last_flag = page_header(&page, g, page_no, md, m, q);

    for (y = 0; y < height; y++) {
      int sy = g->borderless ? (int)((long)y * h.cupsHeight / height) : y + y0;
      int lr = -1, lg = -1, lb = -1, ink[4] = { 0 };

      if (sy >= (int)h.cupsHeight)
        break;
      while (last_sy < sy) { /* read forward to raster line sy (repeats lines when scaling up) */
        if (cupsRasterReadPixels(ras, in, h.cupsBytesPerLine) == 0)
          break;
        last_sy++;
      }
      if (last_sy < sy)
        break;

      for (x = 0; x < width; x++) {
        int sx = xmap ? xmap[x] : x + x0, r, gg, b, v;
        if (sx >= (int)h.cupsWidth)
          r = gg = b = 255;
        else if (bpp == 1)
          r = gg = b = h.cupsColorSpace == CUPS_CSPACE_K ? 255 - in[sx] : in[sx];
        else {
          r = in[sx * 3];
          gg = in[sx * 3 + 1];
          b = in[sx * 3 + 2];
        }

        if (m->mono_pages) { /* black-only encoding: darkness scaled by ink density */
          v = (r * 30 + gg * 59 + b * 11) / 100;
          kamt[3][x] = ((255 - v) * black1000) / 255;
        }
        if (color_input) {
          if (r != gg || gg != b)
            neutral = 0;
          if (r != lr || gg != lg || b != lb) {
            if (r == 255 && gg == 255 && b == 255)
              ink[0] = ink[1] = ink[2] = ink[3] = 0;
            else
              lut_ink(m->lut, r, gg, b, ink);
            lr = r; lg = gg; lb = b;
          }
          for (c = 0; c < 4; c++)
            amt[c][x] = ink[c] * density / 95;
        }
      }
      if (m->mono_pages)
        enc_row(&mono, kamt);
      if (color_input)
        enc_row(&color, amt);
    }
    fprintf(stderr, "DEBUG: Page %d: used raster lines 0-%d of %u\n", page_no, last_sy, h.cupsHeight);
    /* Consume any raster lines not needed (e.g. below the printable area). */
    while (last_sy + 1 < (int)h.cupsHeight && cupsRasterReadPixels(ras, in, h.cupsBytesPerLine))
      last_sy++;

    if (m->mono_pages)
      enc_finish(&mono);
    if (color_input)
      enc_finish(&color);

    /* On plain paper, pages with no colour go out black-only, like Canon's driver. */
    use_mono = m->mono_pages && (!color_input || neutral);
    fprintf(stderr, "DEBUG: Page %d printed %s\n", page_no, use_mono ? "black only" : "in colour");
    if (use_mono)
      buf_put(&page, mono.body.data, mono.body.len);
    else
      buf_put(&page, color.body.data, color.body.len);
    canon_skip(&page, 1);
    buf_byte(&page, 0x0c); /* form feed: eject */
    have_page = 1;

    if (m->mono_pages)
      enc_free(&mono);
    if (color_input)
      enc_free(&color);
    for (c = 0; c < MAX_INK; c++)
      free(amt[c]);
    free(kamt[3]);
    free(xmap);
    free(in);
  }

  if (have_page) {
    page.data[last_flag] = 0x01; /* ESC (s 1 marks the final page */
    buf_put(&page, "\x1b(b\x01\x00\x00", 6);
    /* ESC @ (reset), as Canon's driver sends at the end of every job. Without it the iP100
       can stay in "receiving job" state afterwards, power light flashing. */
    buf_put(&page, "\x1b@", 2);
    write_all(page.data, page.len);
  }
  job_end();
  read_status(3.0);
  report_levels();

  cupsRasterClose(ras);
  if (fd)
    close(fd);
  free(page.data);
  return page_no ? 0 : 1;
}
