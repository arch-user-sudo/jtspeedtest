/*
 * jtspeedtest — a small, zen-styled network speed test client.
 *
 * Wayland + cairo + Pango. No X11, no GTK, no Qt.
 *
 *   make
 *   ./jtspeedtest
 *   ./jtspeedtest '#378484'   (optional accent override)
 *   ./jtspeedtestctl accent '#378484'   (change accent while running)
 *
 * Controls:
 *   Space / Enter   start or stop a test
 *   Esc / q         quit
 *   Click gauge     start a test (when idle)
 *   Drag header     move the window
 *
 * Measurements use Cloudflare's public speed endpoints:
 *   https://speed.cloudflare.com/__down?bytes=N
 *   https://speed.cloudflare.com/__up
 * and their plain-text trace endpoint for the server location.
 * A full test transfers ~25 MB down and ~10 MB up.
 *
 * Palette from ~/JTLab/config.h:
 *   bg #030303, fg #FFFFFF, borders #404040,
 *   accent #E8A03C (overridable), success #66B366, font Liberation.
 */

#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "cursor-shape-client-protocol.h"

#include <curl/curl.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Look & feel                                                        */
/* ------------------------------------------------------------------ */

#define WIN_W 540.0
#define WIN_H 640.0

#define FONT_FAMILY "Liberation"

#define C_BG        0x030303
#define C_BG_TOP    0x070708
#define C_FG        0xFFFFFF
#define C_DIM       0x8C8C8C
#define C_FAINT     0x5C5C5C
#define C_BORDER    0x404040
#define C_CARD      0x0F1011
#define C_TRACK     0x1A1A1C
#define C_GREEN     0x66B366

/* The accent can be overridden at launch.  These aliases keep the drawing
 * code unchanged while allowing the values to be replaced at runtime. */
static unsigned accent_rgb = 0xE8A03C;
static unsigned accent_hi_rgb = 0xF2B25C;
static unsigned accent_lo_rgb = 0xCE8A2F;
static unsigned on_accent_rgb = 0x14100A;
#define C_ACCENT    accent_rgb
#define C_ACCENT_HI accent_hi_rgb
#define C_ACCENT_LO accent_lo_rgb
#define C_ON_ACCENT on_accent_rgb

#define PI 3.14159265358979323846

#define DL_BYTES 25000000
#define UL_BYTES 10000000
#define PING_PROBES 6
#define TEST_UA "jtspeedtest/1.0"

#define R_(h) ((((h) >> 16) & 0xFF) / 255.0)
#define G_(h) ((((h) >> 8) & 0xFF) / 255.0)
#define B_(h) (((h) & 0xFF) / 255.0)

static void rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, R_(hex), G_(hex), B_(hex), a);
}

static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double r)
{
    double d = 2.0 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    r = d / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2.0, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
}

static double mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int accent_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int accent_parse(const char *s, unsigned *out)
{
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (strlen(s) != 6)
        return 0;

    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = accent_hex_nibble(s[i]);
        if (nibble < 0)
            return 0;
        value = (value << 4) | (unsigned)nibble;
    }
    *out = value;
    return 1;
}

static double accent_linear_channel(unsigned channel)
{
    double v = (double)channel / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double accent_luminance(unsigned rgb)
{
    return 0.2126 * accent_linear_channel((rgb >> 16) & 0xFF) +
           0.7152 * accent_linear_channel((rgb >> 8) & 0xFF) +
           0.0722 * accent_linear_channel(rgb & 0xFF);
}

static double accent_contrast(double a, double b)
{
    if (a < b) {
        double t = a;
        a = b;
        b = t;
    }
    return (a + 0.05) / (b + 0.05);
}

static unsigned accent_ink(unsigned rgb)
{
    const unsigned dark = 0x14100A;
    const unsigned light = 0xF7F5F0;
    double background = accent_luminance(rgb);
    double dark_contrast = accent_contrast(background,
                                           accent_luminance(dark));
    double light_contrast = accent_contrast(background,
                                            accent_luminance(light));
    return light_contrast > dark_contrast ? light : dark;
}

static unsigned accent_mix(unsigned rgb, unsigned toward, double amount)
{
    int r1 = (int)((rgb >> 16) & 0xFF);
    int g1 = (int)((rgb >> 8) & 0xFF);
    int b1 = (int)(rgb & 0xFF);
    int r2 = (int)((toward >> 16) & 0xFF);
    int g2 = (int)((toward >> 8) & 0xFF);
    int b2 = (int)(toward & 0xFF);
    int r = (int)lround(r1 + (r2 - r1) * amount);
    int g = (int)lround(g1 + (g2 - g1) * amount);
    int b = (int)lround(b1 + (b2 - b1) * amount);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static void accent_set(unsigned rgb)
{
    accent_rgb = rgb;
    accent_hi_rgb = accent_mix(rgb, 0xFFFFFF, 0.16);
    accent_lo_rgb = accent_mix(rgb, 0x000000, 0.14);
    on_accent_rgb = accent_ink(rgb);
}

/* Remove a leading '#RRGGBB' (or '#RRGGBB' with quotes removed by the shell),
 * a bare leading RRGGBB, or an explicit --accent COLOR from argv. */
static int accent_cli(int *argc, char **argv)
{
    for (int i = 1; i < *argc; i++) {
        const char *value = NULL;
        int consumed = 0;

        if (!strcmp(argv[i], "--accent")) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "%s: --accent requires a color\n", argv[0]);
                return 0;
            }
            value = argv[i + 1];
            consumed = 2;
        } else if (!strncmp(argv[i], "--accent=", 9)) {
            value = argv[i] + 9;
            consumed = 1;
        } else if (argv[i][0] == '#') {
            value = argv[i];
            consumed = 1;
        } else if (i == 1) {
            unsigned candidate;
            if (accent_parse(argv[i], &candidate)) {
                value = argv[i];
                consumed = 1;
            }
        }

        if (!value)
            continue;

        unsigned rgb;
        if (!accent_parse(value, &rgb)) {
            fprintf(stderr,
                    "%s: invalid accent color '%s' (expected #RRGGBB)\n",
                    argv[0], value);
            return 0;
        }
        accent_set(rgb);

        for (int j = i; j + consumed < *argc; j++)
            argv[j] = argv[j + consumed];
        *argc -= consumed;
        i--;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

typedef struct {
    int key;
    PangoFontDescription *fd;
} FontEnt;

#define FONT_CACHE 256
static FontEnt g_fonts[FONT_CACHE];
static int g_font_count;

static PangoFontDescription *font_get(double size, int weight)
{
    int key = (int)lround(size * 10.0) * 10000 + weight;

    for (int i = 0; i < g_font_count; i++)
        if (g_fonts[i].key == key)
            return g_fonts[i].fd;

    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);

    if (g_font_count < FONT_CACHE) {
        g_fonts[g_font_count].key = key;
        g_fonts[g_font_count].fd = fd;
        g_font_count++;
    }
    return fd;
}

static PangoLayout *layout_make(cairo_t *cr, const char *text,
                                double size, int weight, double spacing)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, font_get(size, weight));
    pango_layout_set_text(l, text, -1);

    if (spacing > 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_letter_spacing_new((int)lround(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static double text_width(cairo_t *cr, const char *text, double size,
                         int weight, double spacing)
{
    if (!text || !*text)
        return 0.0;
    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    g_object_unref(l);
    return w;
}

static double text_draw(cairo_t *cr, const char *text, double size, int weight,
                        unsigned hex, double alpha, double x, double y,
                        int align, double spacing)
{
    if (!text || !*text)
        return 0.0;

    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);

    double w = logical.width;
    if (align == ALIGN_CENTER)
        x -= w / 2.0;
    else if (align == ALIGN_RIGHT)
        x -= w;

    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

/* ------------------------------------------------------------------ */
/*  Network test                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    PH_IDLE,
    PH_CONNECT,
    PH_PING,
    PH_DOWNLOAD,
    PH_UPLOAD,
    PH_DONE,
    PH_ERROR
} Phase;

typedef struct {
    pthread_mutex_t lock;
    Phase phase;
    int running;
    double ping, jitter, dl, ul, live, progress;
    char status[192];
    char server[128];
    char ip[64];
} Net;

typedef struct {
    Phase phase;
    int running;
    double ping, jitter, dl, ul, live, progress;
    char status[192];
    char server[128];
    char ip[64];
} Snap;

static _Atomic int g_cancel;
static volatile sig_atomic_t g_signal_quit;

static void net_snapshot(Net *n, Snap *s)
{
    pthread_mutex_lock(&n->lock);
    s->phase = n->phase;
    s->running = n->running;
    s->ping = n->ping;
    s->jitter = n->jitter;
    s->dl = n->dl;
    s->ul = n->ul;
    s->live = n->live;
    s->progress = n->progress;
    snprintf(s->status, sizeof s->status, "%s", n->status);
    snprintf(s->server, sizeof s->server, "%s", n->server);
    snprintf(s->ip, sizeof s->ip, "%s", n->ip);
    pthread_mutex_unlock(&n->lock);
}

static int net_running(Net *n)
{
    pthread_mutex_lock(&n->lock);
    int r = n->running;
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void net_phase(Net *n, Phase phase, const char *status)
{
    pthread_mutex_lock(&n->lock);
    n->phase = phase;
    n->progress = 0.0;
    if (phase != PH_DOWNLOAD && phase != PH_UPLOAD)
        n->live = 0.0;
    if (status)
        snprintf(n->status, sizeof n->status, "%s", status);
    pthread_mutex_unlock(&n->lock);
}

static void net_error(Net *n, const char *msg)
{
    pthread_mutex_lock(&n->lock);
    n->phase = PH_ERROR;
    n->running = 0;
    n->live = 0.0;
    n->progress = 0.0;
    snprintf(n->status, sizeof n->status, "%s", msg);
    pthread_mutex_unlock(&n->lock);
}

static void net_set_server(Net *n, const char *server, const char *ip)
{
    pthread_mutex_lock(&n->lock);
    snprintf(n->server, sizeof n->server, "%s", server);
    snprintf(n->ip, sizeof n->ip, "%s", ip ? ip : "");
    pthread_mutex_unlock(&n->lock);
}

static int cancelled(void)
{
    return atomic_load_explicit(&g_cancel, memory_order_relaxed) != 0;
}

typedef struct {
    Net *net;
    int upload;
    curl_off_t expected;
    curl_off_t bytes;
    curl_off_t last_bytes;
    double last_time;
    double smooth;
    int primed;
} Xfer;

static size_t sink_write(char *ptr, size_t size, size_t nmemb, void *user)
{
    (void)ptr;
    (void)user;
    return size * nmemb;
}

static size_t count_write(char *ptr, size_t size, size_t nmemb, void *user)
{
    (void)ptr;
    Xfer *x = user;
    x->bytes += (curl_off_t)(size * nmemb);
    return size * nmemb;
}

static int xfer_progress(void *user, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)ultotal;
    Xfer *x = user;

    if (cancelled())
        return 1;

    curl_off_t done = x->upload ? ulnow : dlnow;
    double now = mono_sec();

    if (!x->primed) {
        x->primed = 1;
        x->last_time = now;
        x->last_bytes = done;
        return 0;
    }

    double dt = now - x->last_time;
    if (dt < 0.12)
        return 0;

    double inst = (double)(done - x->last_bytes) * 8.0 / dt / 1e6;
    if (inst < 0.0)
        inst = 0.0;
    x->smooth = x->smooth * 0.55 + inst * 0.45;
    if (x->smooth < 0.0)
        x->smooth = 0.0;

    double prog = x->expected > 0
                ? (double)done / (double)x->expected : 0.0;
    if (prog > 1.0) prog = 1.0;
    if (prog < 0.0) prog = 0.0;

    pthread_mutex_lock(&x->net->lock);
    x->net->live = x->smooth;
    x->net->progress = prog;
    pthread_mutex_unlock(&x->net->lock);

    x->last_time = now;
    x->last_bytes = done;
    return 0;
}

static void curl_common(CURL *c)
{
    curl_easy_setopt(c, CURLOPT_USERAGENT, TEST_UA);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
}

typedef struct {
    char data[4096];
    size_t len;
} MemBuf;

static size_t mem_write(char *ptr, size_t size, size_t nmemb, void *user)
{
    MemBuf *m = user;
    size_t add = size * nmemb;

    if (m->len + add > sizeof m->data - 1)
        add = sizeof m->data - 1 - m->len;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return size * nmemb;
}

static void fetch_trace(Net *n, CURL *c)
{
    MemBuf m = { .len = 0 };

    curl_easy_reset(c);
    curl_common(c);
    curl_easy_setopt(c, CURLOPT_URL,
                     "https://www.cloudflare.com/cdn-cgi/trace");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 4L);

    if (curl_easy_perform(c) != CURLE_OK)
        return;

    char colo[16] = "";
    char loc[16] = "";
    char ip[64] = "";
    char *save = NULL;

    for (char *line = strtok_r(m.data, "\n", &save);
         line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "colo=", 5))
            snprintf(colo, sizeof colo, "%s", line + 5);
        else if (!strncmp(line, "loc=", 4))
            snprintf(loc, sizeof loc, "%s", line + 4);
        else if (!strncmp(line, "ip=", 3))
            snprintf(ip, sizeof ip, "%s", line + 3);
    }

    char server[128];
    if (colo[0] && loc[0])
        snprintf(server, sizeof server, "Cloudflare · %s, %s", colo, loc);
    else if (colo[0])
        snprintf(server, sizeof server, "Cloudflare · %s", colo);
    else
        snprintf(server, sizeof server, "Cloudflare");

    net_set_server(n, server, ip);
}

static int run_ping(Net *n, CURL *c, double *out_ping, double *out_jitter,
                    char *err, size_t errlen)
{
    double samples[PING_PROBES];
    int nsamples = 0;
    int failures = 0;
    unsigned seed = (unsigned)time(NULL);

    for (int i = 0; i <= PING_PROBES; i++) {
        if (cancelled())
            return -1;

        char url[256];
        snprintf(url, sizeof url,
                 "https://speed.cloudflare.com/__down?bytes=0&r=%u-%d",
                 seed, i);

        curl_easy_reset(c);
        curl_common(c);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, NULL);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 5000L);

        CURLcode res = curl_easy_perform(c);

        if (res == CURLE_ABORTED_BY_CALLBACK)
            return -1;

        if (res == CURLE_OK && i > 0) {
            double t = 0.0;
            curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &t);
            samples[nsamples++] = t * 1000.0;

            double sum = 0.0;
            for (int k = 0; k < nsamples; k++)
                sum += samples[k];
            double avg = sum / nsamples;

            double jit = 0.0;
            if (nsamples > 1) {
                double jsum = 0.0;
                for (int k = 1; k < nsamples; k++)
                    jsum += fabs(samples[k] - samples[k - 1]);
                jit = jsum / (nsamples - 1);
            }

            pthread_mutex_lock(&n->lock);
            n->ping = avg;
            n->jitter = jit;
            n->progress = (double)i / (double)(PING_PROBES + 1);
            pthread_mutex_unlock(&n->lock);
        } else if (res != CURLE_OK && ++failures >= 2) {
            break;
        }
    }

    if (nsamples == 0) {
        snprintf(err, errlen, "Could not reach the speed test server");
        return -1;
    }

    double sum = 0.0;
    for (int i = 0; i < nsamples; i++)
        sum += samples[i];
    *out_ping = sum / nsamples;

    double jsum = 0.0;
    for (int i = 1; i < nsamples; i++)
        jsum += fabs(samples[i] - samples[i - 1]);
    *out_jitter = nsamples > 1 ? jsum / (nsamples - 1) : 0.0;

    pthread_mutex_lock(&n->lock);
    n->ping = *out_ping;
    n->jitter = *out_jitter;
    n->progress = 1.0;
    pthread_mutex_unlock(&n->lock);
    return 0;
}

static int run_download(Net *n, CURL *c, double *out_mbps,
                        char *err, size_t errlen)
{
    Xfer x = { .net = n, .upload = 0, .expected = DL_BYTES };
    char url[256];

    snprintf(url, sizeof url,
             "https://speed.cloudflare.com/__down?bytes=%d&r=%u",
             DL_BYTES, (unsigned)rand());

    curl_easy_reset(c);
    curl_common(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, count_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &x);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 512L * 1024L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 12L);

    double t0 = mono_sec();
    CURLcode res = curl_easy_perform(c);
    double elapsed = mono_sec() - t0;

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    if (res == CURLE_ABORTED_BY_CALLBACK || cancelled())
        return -1;

    int ok = res == CURLE_OK && code == 200 && x.bytes > 262144;
    int partial = (res == CURLE_OPERATION_TIMEDOUT ||
                   res == CURLE_RECV_ERROR ||
                   res == CURLE_PARTIAL_FILE) &&
                  code == 200 && x.bytes > 131072;

    if ((ok || partial) && elapsed > 0.05) {
        *out_mbps = (double)x.bytes * 8.0 / elapsed / 1e6;
        return 0;
    }

    if (res == CURLE_OK && code != 200)
        snprintf(err, errlen, "Download failed (HTTP %ld)", code);
    else
        snprintf(err, errlen, "Download failed: %s",
                 curl_easy_strerror(res));
    return -1;
}

typedef struct {
    curl_off_t left;
    Xfer *xfer;
} UploadSrc;

static size_t upload_read(char *ptr, size_t size, size_t nmemb, void *user)
{
    UploadSrc *u = user;
    size_t want = size * nmemb;

    if ((curl_off_t)want > u->left)
        want = (size_t)u->left;
    if (want == 0)
        return 0;

    memset(ptr, 0x20, want);
    u->left -= (curl_off_t)want;
    u->xfer->bytes += (curl_off_t)want;
    return want;
}

static int run_upload(Net *n, CURL *c, double *out_mbps,
                      char *err, size_t errlen)
{
    Xfer x = { .net = n, .upload = 1, .expected = UL_BYTES };
    UploadSrc src = { .left = UL_BYTES, .xfer = &x };

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Expect:");
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");

    curl_easy_reset(c);
    curl_common(c);
    curl_easy_setopt(c, CURLOPT_URL, "https://speed.cloudflare.com/__up");
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)UL_BYTES);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, upload_read);
    curl_easy_setopt(c, CURLOPT_READDATA, &src);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &x);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 512L * 1024L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 10L);

    double t0 = mono_sec();
    CURLcode res = curl_easy_perform(c);
    double elapsed = mono_sec() - t0;

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);

    if (res == CURLE_ABORTED_BY_CALLBACK || cancelled())
        return -1;

    int ok = res == CURLE_OK && code >= 200 && code < 300 &&
             x.bytes > 262144;
    int partial = (res == CURLE_OPERATION_TIMEDOUT ||
                   res == CURLE_SEND_ERROR ||
                   res == CURLE_PARTIAL_FILE) &&
                  x.bytes > 131072;

    if ((ok || partial) && elapsed > 0.05) {
        *out_mbps = (double)x.bytes * 8.0 / elapsed / 1e6;
        return 0;
    }

    if (res == CURLE_OK && (code < 200 || code >= 300))
        snprintf(err, errlen, "Upload failed (HTTP %ld)", code);
    else
        snprintf(err, errlen, "Upload failed: %s",
                 curl_easy_strerror(res));
    return -1;
}

static void *test_worker(void *arg)
{
    Net *n = arg;
    char err[192] = "";
    double ping = 0.0, jitter = 0.0, dl = 0.0, ul = 0.0;

    CURL *c = curl_easy_init();
    if (!c) {
        net_error(n, "Could not initialise libcurl");
        return NULL;
    }

    net_phase(n, PH_CONNECT, "Connecting to Cloudflare…");
    fetch_trace(n, c);

    if (cancelled())
        goto cancelled_out;

    net_phase(n, PH_PING, "Measuring latency…");
    if (run_ping(n, c, &ping, &jitter, err, sizeof err) != 0) {
        if (cancelled())
            goto cancelled_out;
        net_error(n, err[0] ? err : "Latency test failed");
        goto out;
    }

    if (cancelled())
        goto cancelled_out;

    /* fresh handle per phase: avoids a stale keep-alive connection
     * turning a 25 MB download into a "could not connect" error */
    curl_easy_cleanup(c);
    c = curl_easy_init();
    if (!c) {
        net_error(n, "Could not initialise libcurl");
        goto out;
    }

    net_phase(n, PH_DOWNLOAD, "Measuring download…");
    if (run_download(n, c, &dl, err, sizeof err) != 0) {
        if (cancelled())
            goto cancelled_out;
        net_error(n, err[0] ? err : "Download test failed");
        goto out;
    }

    pthread_mutex_lock(&n->lock);
    n->dl = dl;
    n->progress = 1.0;
    pthread_mutex_unlock(&n->lock);

    if (cancelled())
        goto cancelled_out;

    curl_easy_cleanup(c);
    c = curl_easy_init();
    if (!c) {
        net_error(n, "Could not initialise libcurl");
        goto out;
    }

    net_phase(n, PH_UPLOAD, "Measuring upload…");
    if (run_upload(n, c, &ul, err, sizeof err) != 0) {
        if (cancelled())
            goto cancelled_out;
        net_error(n, err[0] ? err : "Upload test failed");
        goto out;
    }

    pthread_mutex_lock(&n->lock);
    n->ul = ul;
    n->phase = PH_DONE;
    n->running = 0;
    n->live = 0.0;
    n->progress = 1.0;
    snprintf(n->status, sizeof n->status, "Test complete");
    pthread_mutex_unlock(&n->lock);
    goto out;

cancelled_out:
    pthread_mutex_lock(&n->lock);
    n->phase = PH_IDLE;
    n->running = 0;
    n->live = 0.0;
    n->progress = 0.0;
    snprintf(n->status, sizeof n->status, "Test cancelled");
    pthread_mutex_unlock(&n->lock);

out:
    if (c)
        curl_easy_cleanup(c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const Snap *s;
    double t;
    double disp;
    int hover_start;
    int press_start;
    int hover_close;
    double done_t;
} View;

static void fmt_speed(char *buf, size_t n, double v)
{
    if (v < 0.005) {
        snprintf(buf, n, "—");
    } else if (v < 10.0) {
        snprintf(buf, n, "%.2f", v);
    } else if (v < 100.0) {
        snprintf(buf, n, "%.1f", v);
    } else {
        snprintf(buf, n, "%.0f", v);
    }
}

static void fmt_ms(char *buf, size_t n, double v)
{
    if (v <= 0.0) {
        snprintf(buf, n, "—");
    } else if (v < 10.0) {
        snprintf(buf, n, "%.1f", v);
    } else {
        snprintf(buf, n, "%.0f", v);
    }
}

static int is_running(Phase p)
{
    return p == PH_CONNECT || p == PH_PING ||
           p == PH_DOWNLOAD || p == PH_UPLOAD;
}

static double gauge_ratio(double mbps)
{
    if (mbps <= 0.0)
        return 0.0;
    double r = log1p(mbps) / log1p(1000.0);
    if (r > 1.0) r = 1.0;
    return r;
}

#define GAUGE_CX 270.0
#define GAUGE_CY 208.0
#define GAUGE_R  98.0
#define GAUGE_A0 (3.0 * PI / 4.0)
#define GAUGE_A1 (9.0 * PI / 4.0)

static void paint_ambient(cairo_t *cr, const View *v, int running, int done)
{
    double breath = 0.5 + 0.5 * sin(v->t * 0.55);

    double glow = (running ? 0.080 : 0.042) * (0.72 + 0.28 * breath);
    cairo_pattern_t *pat = cairo_pattern_create_radial(
        GAUGE_CX, GAUGE_CY, 18.0,
        GAUGE_CX, GAUGE_CY, 232.0);
    cairo_pattern_add_color_stop_rgba(pat, 0.00,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), glow);
    cairo_pattern_add_color_stop_rgba(pat, 0.55,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), glow * 0.28);
    cairo_pattern_add_color_stop_rgba(pat, 1.00,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.0);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);

    if (done && v->done_t >= 0.0) {
        double age = v->t - v->done_t;
        if (age >= 0.0 && age < 7.0) {
            double a = 0.055 * (1.0 - age / 7.0);
            cairo_pattern_t *g = cairo_pattern_create_radial(
                GAUGE_CX, GAUGE_CY, 10.0,
                GAUGE_CX, GAUGE_CY, 210.0);
            cairo_pattern_add_color_stop_rgba(g, 0.0,
                R_(C_GREEN), G_(C_GREEN), B_(C_GREEN), a);
            cairo_pattern_add_color_stop_rgba(g, 1.0,
                R_(C_GREEN), G_(C_GREEN), B_(C_GREEN), 0.0);
            cairo_set_source(cr, g);
            cairo_paint(cr);
            cairo_pattern_destroy(g);
        }
    }
}

static void paint_dust(cairo_t *cr, const View *v, int running)
{
    for (int i = 0; i < 26; i++) {
        double seed = (double)(i + 1) * 12.9898;
        double base_x = 34.0 + fmod(seed * 37.719, 472.0);
        double speed = 5.0 + fmod(seed * 11.13, 13.0);
        double span = 328.0;
        double drift = fmod(seed * 19.19 +
                            v->t * speed * (running ? 1.55 : 1.0), span);
        double y = 332.0 - drift;
        double x = base_x + sin(v->t * 0.33 + i * 1.7) *
                   (5.0 + fmod(seed * 3.3, 7.0));
        double a = (0.035 + 0.075 * (0.5 + 0.5 *
                   sin(v->t * 0.5 + i * 2.1))) * (running ? 1.55 : 1.0);
        double rad = 0.7 + fmod(seed * 5.7, 1.25);

        unsigned col = C_FG;
        if (i % 7 == 2) col = C_ACCENT;
        else if (i % 11 == 5) col = C_GREEN;

        rgba(cr, col, a);
        cairo_arc(cr, x, y, rad, 0.0, 2.0 * PI);
        cairo_fill(cr);
    }
}

static void paint_rings(cairo_t *cr, const View *v, int running, int done)
{
    for (int k = 0; k < 2; k++) {
        double r = 112.0 + k * 14.0 + sin(v->t * 0.5 + k) * 2.5;
        double a = (0.030 + 0.022 * (0.5 + 0.5 *
                   sin(v->t * 0.55 + k * 1.2))) * (running ? 1.7 : 1.0);
        rgba(cr, k ? C_GREEN : C_ACCENT, a);
        cairo_set_line_width(cr, 1.0);
        cairo_arc(cr, GAUGE_CX, GAUGE_CY, r, 0.0, 2.0 * PI);
        cairo_stroke(cr);
    }

    if (!running && !done)
        return;

    double base_t = 0.0;
    double life = 1.0;
    if (done && v->done_t >= 0.0) {
        base_t = v->done_t;
        double age = v->t - base_t;
        if (age > 6.5)
            return;
        if (age > 0.0)
            life = 1.0 - age / 6.5;
    }

    int count = running ? 3 : 2;
    double period = running ? 2.8 : 4.2;

    for (int k = 0; k < count; k++) {
        double ph = fmod((v->t - base_t) / period + (double)k / count, 1.0);
        if (ph < 0.0)
            ph += 1.0;
        double r = 101.0 + ph * 78.0;
        double a = (1.0 - ph) * (1.0 - ph) *
                   (running ? 0.16 : 0.13) * life;
        rgba(cr, done ? C_GREEN : C_ACCENT, a);
        cairo_set_line_width(cr, 0.9 + 1.1 * (1.0 - ph));
        cairo_arc(cr, GAUGE_CX, GAUGE_CY, r, 0.0, 2.0 * PI);
        cairo_stroke(cr);
    }
}

static void paint_gauge(cairo_t *cr, const View *v, int running)
{
    const Snap *s = v->s;
    double span = GAUGE_A1 - GAUGE_A0;

    /* track */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgba(cr, C_FG, 0.055);
    cairo_set_line_width(cr, 6.0);
    cairo_arc(cr, GAUGE_CX, GAUGE_CY, GAUGE_R, GAUGE_A0, GAUGE_A1);
    cairo_stroke(cr);

    if (s->phase == PH_CONNECT || s->phase == PH_PING) {
        /* indeterminate sweeping arc while waiting */
        double ph = fmod(v->t * 0.15, 1.0);
        double seg = span * 0.15;
        double start = GAUGE_A0 + span * ph;
        double pulse = 0.38 + 0.22 * sin(v->t * 1.35);

        rgba(cr, C_ACCENT, pulse * 0.22);
        cairo_set_line_width(cr, 12.0);
        cairo_arc(cr, GAUGE_CX, GAUGE_CY, GAUGE_R, start, start + seg);
        cairo_stroke(cr);

        rgba(cr, C_ACCENT, pulse);
        cairo_set_line_width(cr, 4.0);
        cairo_arc(cr, GAUGE_CX, GAUGE_CY, GAUGE_R, start, start + seg);
        cairo_stroke(cr);
    } else {
        double ratio = gauge_ratio(v->disp);
        if (ratio > 0.001) {
            double end = GAUGE_A0 + span * ratio;

            rgba(cr, C_ACCENT, 0.10);
            cairo_set_line_width(cr, 17.0);
            cairo_arc(cr, GAUGE_CX, GAUGE_CY, GAUGE_R, GAUGE_A0, end);
            cairo_stroke(cr);

            rgba(cr, C_ACCENT, 0.96);
            cairo_set_line_width(cr, 6.0);
            cairo_arc(cr, GAUGE_CX, GAUGE_CY, GAUGE_R, GAUGE_A0, end);
            cairo_stroke(cr);

            double kx = GAUGE_CX + cos(end) * GAUGE_R;
            double ky = GAUGE_CY + sin(end) * GAUGE_R;
            rgba(cr, C_ACCENT, 0.12);
            cairo_arc(cr, kx, ky, 9.0, 0.0, 2.0 * PI);
            cairo_fill(cr);
            rgba(cr, C_ACCENT, 1.0);
            cairo_arc(cr, kx, ky, 3.2, 0.0, 2.0 * PI);
            cairo_fill(cr);
            rgba(cr, C_FG, 0.75);
            cairo_arc(cr, kx, ky, 1.15, 0.0, 2.0 * PI);
            cairo_fill(cr);
        }
    }

    /* centre readout */
    char value[32];
    char unit[16] = "";
    const char *label = "READY";
    unsigned label_col = C_DIM;

    switch (s->phase) {
    case PH_CONNECT:
        label = "CONNECTING";
        snprintf(value, sizeof value, "—");
        label_col = C_ACCENT;
        break;
    case PH_PING:
        label = "LATENCY";
        fmt_ms(value, sizeof value, s->ping);
        snprintf(unit, sizeof unit, "ms");
        label_col = C_ACCENT;
        break;
    case PH_DOWNLOAD:
        label = "DOWNLOAD";
        fmt_speed(value, sizeof value, v->disp);
        snprintf(unit, sizeof unit, "Mbps");
        label_col = C_ACCENT;
        break;
    case PH_UPLOAD:
        label = "UPLOAD";
        fmt_speed(value, sizeof value, v->disp);
        snprintf(unit, sizeof unit, "Mbps");
        label_col = C_ACCENT;
        break;
    case PH_DONE:
        label = "DOWNLOAD";
        fmt_speed(value, sizeof value, v->disp);
        snprintf(unit, sizeof unit, "Mbps");
        label_col = C_GREEN;
        break;
    case PH_ERROR:
        label = "OFFLINE";
        snprintf(value, sizeof value, "—");
        label_col = C_ACCENT;
        break;
    case PH_IDLE:
    default:
        label = "READY";
        snprintf(value, sizeof value, "—");
        snprintf(unit, sizeof unit, "Mbps");
        break;
    }

    text_draw(cr, label, 10.0, 600, label_col, running ? 0.95 : 0.72,
              GAUGE_CX, 156.0, ALIGN_CENTER, 2.0);
    text_draw(cr, value, 50.0, 700, C_FG, 0.97,
              GAUGE_CX, 177.0, ALIGN_CENTER, 0.0);
    if (unit[0])
        text_draw(cr, unit, 11.0, 400, C_DIM, 0.88,
                  GAUGE_CX, 241.0, ALIGN_CENTER, 1.0);
}

static void paint_phase_bar(cairo_t *cr, const View *v)
{
    const Snap *s = v->s;
    int active = -1;
    int done = s->phase == PH_DONE;

    switch (s->phase) {
    case PH_CONNECT:
    case PH_PING:     active = 0; break;
    case PH_DOWNLOAD: active = 1; break;
    case PH_UPLOAD:   active = 2; break;
    case PH_DONE:     active = 3; break;
    default:          active = -1; break;
    }

    double gap = 7.0;
    double w = (300.0 - gap * 2.0) / 3.0;
    double x0 = (WIN_W - 300.0) / 2.0;
    double y = 318.0;
    double h = 4.0;

    for (int i = 0; i < 3; i++) {
        double x = x0 + i * (w + gap);

        rgba(cr, C_FG, 0.060);
        rounded_rect(cr, x, y, w, h, 2.0);
        cairo_fill(cr);

        double fill = 0.0;
        if (done || active > i)
            fill = 1.0;
        else if (active == i)
            fill = s->progress;

        if (fill > 0.001) {
            if (active == i && !done) {
                rgba(cr, C_ACCENT, 0.10);
                rounded_rect(cr, x - 1.0, y - 3.0, (w + 2.0) * fill,
                             h + 6.0, 4.0);
                cairo_fill(cr);
            }
            rgba(cr, done ? C_GREEN : C_ACCENT, done ? 0.9 : 0.95);
            rounded_rect(cr, x, y, w * fill, h, 2.0);
            cairo_fill(cr);
        }
    }

    /* status line */
    double tw = text_width(cr, s->status, 11.0, 400, 0.15);
    unsigned dot = C_ACCENT;

    if (s->phase == PH_DONE)
        dot = C_GREEN;

    if (is_running(s->phase) || done || s->phase == PH_ERROR) {
        double pulse = 0.45 + 0.35 * sin(v->t * 2.1);
        double dx = GAUGE_CX - tw / 2.0 - 11.0;
        rgba(cr, dot, s->phase == PH_ERROR ? 0.75 : pulse);
        cairo_arc(cr, dx, 341.0, 2.0, 0.0, 2.0 * PI);
        cairo_fill(cr);
    }

    text_draw(cr, s->status, 11.0, 400, C_DIM, 0.92,
              GAUGE_CX, 333.0, ALIGN_CENTER, 0.15);
}

static void paint_card(cairo_t *cr, const View *v,
                       double x, double y, double w, double h,
                       const char *label, const char *value,
                       const char *unit, unsigned color, int active)
{
    cairo_pattern_t *fill = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(fill, 0.0,
        R_(0x131416), G_(0x131416), B_(0x131416), 0.92);
    cairo_pattern_add_color_stop_rgba(fill, 1.0,
        R_(0x0A0A0C), G_(0x0A0A0C), B_(0x0A0A0C), 0.92);
    cairo_set_source(cr, fill);
    rounded_rect(cr, x, y, w, h, 9.0);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);

    rgba(cr, active ? color : C_BORDER,
         active ? 0.34 + 0.12 * sin(v->t * 2.2) : 0.42);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 8.5);
    cairo_stroke(cr);

    text_draw(cr, label, 9.5, 600, C_DIM, 0.88,
              x + 14.0, y + 11.0, ALIGN_LEFT, 1.35);

    int has = strcmp(value, "—") != 0;
    double vw = text_draw(cr, value, 21.0, 700, color, has ? 0.97 : 0.30,
                          x + 14.0, y + 28.0, ALIGN_LEFT, 0.0);

    if (unit[0])
        text_draw(cr, unit, 9.5, 400, C_DIM, has ? 0.80 : 0.24,
                  x + 14.0 + vw + 5.0, y + 41.0, ALIGN_LEFT, 0.5);
}

static void paint_button(cairo_t *cr, const View *v, int running)
{
    double bw = 200.0, bh = 46.0;
    double bx = (WIN_W - bw) / 2.0;
    double by = 532.0;

    for (int i = 3; i >= 1; i--) {
        rgba(cr, C_ACCENT, running ? 0.012 : 0.020);
        rounded_rect(cr, bx - i * 3.0, by - i * 3.0,
                     bw + i * 6.0, bh + i * 6.0, 23.0 + i * 3.0);
        cairo_fill(cr);
    }

    if (running) {
        rgba(cr, C_ACCENT, v->press_start ? 0.18 : (v->hover_start ? 0.11 : 0.05));
        rounded_rect(cr, bx, by, bw, bh, 23.0);
        cairo_fill(cr);

        rgba(cr, C_ACCENT, 0.90);
        cairo_set_line_width(cr, 1.2);
        rounded_rect(cr, bx + 0.6, by + 0.6, bw - 1.2, bh - 1.2, 22.4);
        cairo_stroke(cr);
    } else {
        unsigned fill = v->press_start ? C_ACCENT_LO :
                        (v->hover_start ? C_ACCENT_HI : C_ACCENT);
        rgba(cr, fill, 1.0);
        rounded_rect(cr, bx, by, bw, bh, 23.0);
        cairo_fill(cr);
    }

    const char *label = running ? "Stop test" : "Start test";
    unsigned ink = running ? C_ACCENT : C_ON_ACCENT;
    double tw = text_width(cr, label, 14.0, 600, 0.3);
    double iconw = 10.0;
    double gap = 10.0;
    double gx = (WIN_W - (iconw + gap + tw)) / 2.0;
    double cy = by + bh / 2.0;

    if (running) {
        rgba(cr, ink, 0.95);
        rounded_rect(cr, gx, cy - 5.0, 10.0, 10.0, 2.0);
        cairo_fill(cr);
    } else {
        cairo_move_to(cr, gx, cy - 5.5);
        cairo_line_to(cr, gx + 9.5, cy);
        cairo_line_to(cr, gx, cy + 5.5);
        cairo_close_path(cr);
        rgba(cr, ink, 1.0);
        cairo_fill(cr);
    }

    text_draw(cr, label, 14.0, 600, ink, 1.0,
              gx + iconw + gap, cy - 9.5, ALIGN_LEFT, 0.3);
}

static void paint_header(cairo_t *cr, const View *v, int running)
{
    (void)running;

    /* small equaliser mark */
    for (int i = 0; i < 4; i++) {
        double h = 6.0 + i * 3.0;
        rgba(cr, C_ACCENT, 0.55 + i * 0.12);
        rounded_rect(cr, 24.0 + i * 5.0, 36.0 - h, 3.0, h, 1.4);
        cairo_fill(cr);
    }

    double w1 = text_draw(cr, "JTLab", 15.0, 700, C_FG, 0.96,
                          49.0, 19.0, ALIGN_LEFT, 0.1);
    text_draw(cr, "Speedtest", 15.0, 400, C_DIM, 0.86,
              49.0 + w1 + 5.0, 19.0, ALIGN_LEFT, 0.1);

    rgba(cr, C_FG, 0.045);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 24.0, 54.5);
    cairo_line_to(cr, WIN_W - 24.0, 54.5);
    cairo_stroke(cr);

    double ccx = WIN_W - 30.0;
    double ccy = 28.0;
    if (v->hover_close) {
        rgba(cr, C_FG, 0.07);
        cairo_arc(cr, ccx, ccy, 15.0, 0.0, 2.0 * PI);
        cairo_fill(cr);
    }
    rgba(cr, v->hover_close ? C_FG : C_DIM, v->hover_close ? 0.9 : 0.62);
    cairo_set_line_width(cr, 1.35);
    cairo_move_to(cr, ccx - 5.0, ccy - 5.0);
    cairo_line_to(cr, ccx + 5.0, ccy + 5.0);
    cairo_move_to(cr, ccx + 5.0, ccy - 5.0);
    cairo_line_to(cr, ccx - 5.0, ccy + 5.0);
    cairo_stroke(cr);
}

static void paint_footer(cairo_t *cr, const View *v, int running)
{
    (void)running;

    const char *server = v->s->server[0] ? v->s->server : "Cloudflare";
    text_draw(cr, server, 9.5, 400, C_FAINT, 0.82,
              24.0, 603.0, ALIGN_LEFT, 0.25);

    text_draw(cr, "SPACE  START     ESC  QUIT", 9.0, 600, C_FAINT, 0.60,
              WIN_W - 24.0, 603.0, ALIGN_RIGHT, 1.1);
}

static void paint_content(cairo_t *cr, const View *v)
{
    const Snap *s = v->s;
    int running = is_running(s->phase);
    int done = s->phase == PH_DONE;

    cairo_save(cr);
    rounded_rect(cr, 0.5, 0.5, WIN_W - 1.0, WIN_H - 1.0, 12.0);
    cairo_clip(cr);

    cairo_pattern_t *bg = cairo_pattern_create_linear(0.0, 0.0, 0.0, WIN_H);
    cairo_pattern_add_color_stop_rgba(bg, 0.0,
        R_(C_BG_TOP), G_(C_BG_TOP), B_(C_BG_TOP), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 0.55,
        R_(0x050506), G_(0x050506), B_(0x050506), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 1.0,
        R_(C_BG), G_(C_BG), B_(C_BG), 1.0);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    paint_ambient(cr, v, running, done);
    paint_dust(cr, v, running);
    paint_rings(cr, v, running, done);
    paint_header(cr, v, running);
    paint_gauge(cr, v, running);
    paint_phase_bar(cr, v);

    double dl = s->dl > 0.0 ? s->dl :
                (s->phase == PH_DOWNLOAD ? s->live : 0.0);
    double ul = s->ul > 0.0 ? s->ul :
                (s->phase == PH_UPLOAD ? s->live : 0.0);
    double pg = s->ping;
    double jt = s->jitter;

    char v1[32], v2[32], v3[32], v4[32];
    fmt_speed(v1, sizeof v1, dl);
    fmt_speed(v2, sizeof v2, ul);
    fmt_ms(v3, sizeof v3, pg);
    fmt_ms(v4, sizeof v4, jt);

    double x0 = 24.0, gap = 12.0, cw = 240.0, ch = 66.0;
    double y1 = 366.0, y2 = 444.0;

    paint_card(cr, v, x0, y1, cw, ch,
               "DOWNLOAD", v1, "Mbps", C_ACCENT,
               s->phase == PH_DOWNLOAD);
    paint_card(cr, v, x0 + cw + gap, y1, cw, ch,
               "UPLOAD", v2, "Mbps", C_GREEN,
               s->phase == PH_UPLOAD);
    paint_card(cr, v, x0, y2, cw, ch,
               "PING", v3, "ms", C_FG,
               s->phase == PH_PING);
    paint_card(cr, v, x0 + cw + gap, y2, cw, ch,
               "JITTER", v4, "ms", C_FG, 0);

    paint_button(cr, v, running);
    paint_footer(cr, v, running);

    cairo_restore(cr);

    rgba(cr, C_BORDER, 1.0);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, 0.5, 0.5, WIN_W - 1.0, WIN_H - 1.0, 12.0);
    cairo_stroke(cr);
}

/* ------------------------------------------------------------------ */
/*  Application                                                        */
/* ------------------------------------------------------------------ */

typedef struct App App;

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height;
    int busy;
    int retired;
} ShmBuf;

typedef struct {
    struct wl_output *output;
    int32_t scale;
    int entered;
    App *app;
} Output;

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    uint32_t pointer_serial;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct xdg_toplevel_icon_manager_v1 *icon_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    uint32_t cursor_shape;

    int configured;
    int scale;
    int fixed_scale;
    int has_argb;
    ShmBuf bufs[4];

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    Output outputs[8];
    int output_count;

    Net net;
    pthread_t worker;
    int worker_alive;

    double t0;
    double disp;
    double done_t;
    Phase prev_phase;

    int hover_start;
    int press_start;
    int hover_close;
    int quit;

    double pointer_x, pointer_y;
    double last_draw;

    int ctl_fd;
    int ctl_client_fd;
    char ctl_buf[256];
    size_t ctl_len;
};

static int hit_start(double x, double y)
{
    return x >= (WIN_W - 200.0) / 2.0 && x <= (WIN_W + 200.0) / 2.0 &&
           y >= 532.0 && y <= 578.0;
}

static int hit_close(double x, double y)
{
    return x >= WIN_W - 48.0 && y <= 56.0;
}

static int hit_gauge(double x, double y)
{
    double dx = x - GAUGE_CX;
    double dy = y - GAUGE_CY;
    return dx * dx + dy * dy <= 112.0 * 112.0;
}

static void update_cursor(App *a)
{
    if (!a->cursor_shape_manager || !a->pointer)
        return;
    if (!a->cursor_shape_device) {
        a->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
            a->cursor_shape_manager, a->pointer);
        a->cursor_shape = 0;
    }

    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (a->hover_close || a->hover_start ||
        (hit_gauge(a->pointer_x, a->pointer_y) && !net_running(&a->net))) {
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    } else if (a->pointer_x >= 0.0 && a->pointer_y >= 0.0 &&
               a->pointer_y < 56.0) {
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
    }

    if (shape != a->cursor_shape) {
        wp_cursor_shape_device_v1_set_shape(a->cursor_shape_device,
                                            a->pointer_serial, shape);
        a->cursor_shape = shape;
    }
}

static void app_reap(App *a)
{
    if (a->worker_alive && !net_running(&a->net)) {
        pthread_join(a->worker, NULL);
        a->worker_alive = 0;
    }
}

static void start_test(App *a)
{
    if (a->worker_alive) {
        if (net_running(&a->net))
            return;
        pthread_join(a->worker, NULL);
        a->worker_alive = 0;
    }

    atomic_store_explicit(&g_cancel, 0, memory_order_relaxed);

    pthread_mutex_lock(&a->net.lock);
    a->net.phase = PH_CONNECT;
    a->net.running = 1;
    a->net.ping = a->net.jitter = 0.0;
    a->net.dl = a->net.ul = a->net.live = 0.0;
    a->net.progress = 0.0;
    snprintf(a->net.status, sizeof a->net.status, "Connecting…");
    pthread_mutex_unlock(&a->net.lock);

    a->disp = 0.0;
    a->done_t = -1.0;
    a->prev_phase = PH_CONNECT;

    if (pthread_create(&a->worker, NULL, test_worker, &a->net) == 0)
        a->worker_alive = 1;
    else
        net_error(&a->net, "Could not start test thread");
}

static void stop_test(App *a)
{
    if (!a->worker_alive)
        return;

    atomic_store_explicit(&g_cancel, 1, memory_order_relaxed);
    pthread_join(a->worker, NULL);
    a->worker_alive = 0;

    pthread_mutex_lock(&a->net.lock);
    a->net.running = 0;
    a->net.phase = PH_IDLE;
    a->net.live = 0.0;
    a->net.progress = 0.0;
    snprintf(a->net.status, sizeof a->net.status, "Test cancelled");
    pthread_mutex_unlock(&a->net.lock);

    a->disp = 0.0;
}

static void toggle_test(App *a)
{
    if (net_running(&a->net))
        stop_test(a);
    else
        start_test(a);
}

/* ------------------------------------------------------------------ */
/*  Wayland backend                                                    */
/* ------------------------------------------------------------------ */

#define NBUFS 4
#define MAX_OUTPUTS 8
#define BTN_LEFT 0x110

static int create_anon_file(size_t size)
{
    int fd = memfd_create("jtspeedtest", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0)
            return fd;
        close(fd);
    }

    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/jtspeedtest-XXXXXX",
             (dir && *dir) ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void shm_buffer_destroy(ShmBuf *b)
{
    if (b->buffer) {
        wl_buffer_destroy(b->buffer);
        b->buffer = NULL;
    }
    if (b->data) {
        munmap(b->data, b->size);
        b->data = NULL;
    }
    b->size = 0;
    b->width = b->height = 0;
    b->busy = b->retired = 0;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ShmBuf *b = data;
    b->busy = 0;
    if (b->retired)
        shm_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int shm_buffer_create(App *a, ShmBuf *b, int w, int h)
{
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = create_anon_file(size);
    if (fd < 0)
        return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    if (!pool) {
        munmap(data, size);
        close(fd);
        return -1;
    }

    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!b->buffer) {
        munmap(data, size);
        return -1;
    }

    b->data = data;
    b->size = size;
    b->width = w;
    b->height = h;
    b->busy = 0;
    b->retired = 0;
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    return 0;
}

static ShmBuf *next_buffer(App *a, int w, int h)
{
    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired &&
            b->width == w && b->height == h)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer && shm_buffer_create(a, b, w, h) == 0)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired) {
            shm_buffer_destroy(b);
            if (shm_buffer_create(a, b, w, h) == 0)
                return b;
        }
    }

    return NULL;
}

static void app_draw(App *a, const Snap *s)
{
    if (!a->configured || !a->surface || !a->shm)
        return;

    int w = (int)lround(WIN_W * a->scale);
    int h = (int)lround(WIN_H * a->scale);
    if (w < 1 || h < 1)
        return;

    ShmBuf *b = next_buffer(a, w, h);
    if (!b)
        return;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, a->scale, a->scale);

    View v;
    v.s = s;
    v.t = mono_sec() - a->t0;
    v.disp = a->disp;
    v.hover_start = a->hover_start;
    v.press_start = a->press_start;
    v.hover_close = a->hover_close;
    v.done_t = a->done_t;

    paint_content(cr, &v);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_attach(a->surface, b->buffer, 0, 0);
    wl_surface_damage(a->surface, 0, 0, (int)WIN_W, (int)WIN_H);
    wl_surface_commit(a->surface);
    b->busy = 1;
}

static void recalc_scale(App *a)
{
    if (a->fixed_scale)
        return;

    int s = 0;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].entered && a->outputs[i].scale > s)
            s = a->outputs[i].scale;
    }
    if (s < 1)
        s = 1;
    if (s > 3)
        s = 3;
    if (s == a->scale)
        return;

    a->scale = s;
    if (a->surface && wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, s);

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer)
            continue;
        if (b->busy)
            b->retired = 1;
        else
            shm_buffer_destroy(b);
    }
}

/* ---- protocol listeners ------------------------------------------ */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    (void)shm;
    App *a = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        a->has_argb = 1;
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial)
{
    App *a = data;
    xdg_surface_ack_configure(xs, serial);
    a->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)t;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    App *a = data;
    a->quit = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void decoration_configure(void *data,
                                 struct zxdg_toplevel_decoration_v1 *dec,
                                 uint32_t mode)
{
    (void)data;
    (void)dec;
    (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make,
                            const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags;
    (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    Output *o = data;
    if (factor < 1)
        factor = 1;
    o->scale = factor;
    if (o->entered)
        recalc_scale(o->app);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 1;
            break;
        }
    }
    recalc_scale(a);
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 0;
            break;
        }
    }
    recalc_scale(a);
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)surface;
    App *a = data;
    a->pointer_serial = serial;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    a->cursor_shape = 0;
    a->hover_start = hit_start(a->pointer_x, a->pointer_y);
    a->hover_close = hit_close(a->pointer_x, a->pointer_y);
    update_cursor(a);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)pointer; (void)serial; (void)surface;
    App *a = data;
    a->pointer_x = -1;
    a->pointer_y = -1;
    a->hover_start = 0;
    a->hover_close = 0;
    a->press_start = 0;
    update_cursor(a);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    App *a = data;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    a->hover_start = hit_start(a->pointer_x, a->pointer_y);
    a->hover_close = hit_close(a->pointer_x, a->pointer_y);
    update_cursor(a);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)time;
    App *a = data;

    if (button != BTN_LEFT)
        return;

    double x = a->pointer_x;
    double y = a->pointer_y;
    a->hover_start = hit_start(x, y);
    a->hover_close = hit_close(x, y);

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (hit_close(x, y)) {
            a->quit = 1;
        } else if (hit_start(x, y)) {
            a->press_start = 1;
        } else if (y < 56.0) {
            if (a->toplevel && a->seat)
                xdg_toplevel_move(a->toplevel, a->seat, serial);
        } else if (hit_gauge(x, y) && !net_running(&a->net)) {
            start_test(a);
        }
    } else if (a->press_start) {
        a->press_start = 0;
        if (hit_start(x, y))
            toggle_test(a);
    }
    update_cursor(a);
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    App *a = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km)
        return;

    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }

    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->xkb_keymap);
    a->xkb_keymap = km;
    a->xkb_state = st;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)serial; (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard; (void)serial; (void)time;
    App *a = data;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !a->xkb_state)
        return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);
    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q) {
        a->quit = 1;
    } else if (sym == XKB_KEY_space || sym == XKB_KEY_Return ||
               sym == XKB_KEY_KP_Enter) {
        toggle_test(a);
        update_cursor(a);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t dep,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)keyboard; (void)serial;
    App *a = data;
    if (a->xkb_state)
        xkb_state_update_mask(a->xkb_state, dep, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    App *a = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        if (a->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
            a->cursor_shape_device = NULL;
            a->cursor_shape = 0;
        }
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    App *a = data;

    if (!strcmp(interface, wl_compositor_interface.name)) {
        a->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface,
                                         version < 4 ? version : 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(a->shm, &shm_listener, a);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        a->wm_base = wl_registry_bind(registry, name,
                                      &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(a->wm_base, &wm_base_listener, a);
    } else if (!strcmp(interface,
                       zxdg_decoration_manager_v1_interface.name)) {
        a->deco_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       xdg_toplevel_icon_manager_v1_interface.name)) {
        a->icon_manager = wl_registry_bind(
            registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       wp_cursor_shape_manager_v1_interface.name)) {
        a->cursor_shape_manager = wl_registry_bind(
            registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        a->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        if (a->output_count < MAX_OUTPUTS) {
            Output *o = &a->outputs[a->output_count];
            o->output = wl_registry_bind(registry, name,
                                         &wl_output_interface,
                                         version < 2 ? version : 2);
            o->scale = 1;
            o->entered = 0;
            o->app = a;
            wl_output_add_listener(o->output, &output_listener, o);
            a->output_count++;
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int wayland_start(App *a)
{
    a->display = wl_display_connect(NULL);
    if (!a->display) {
        fprintf(stderr, "jtspeedtest: cannot connect to a Wayland display\n");
        return 0;
    }

    a->registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(a->registry, &registry_listener, a);
    if (wl_display_roundtrip(a->display) < 0 ||
        wl_display_roundtrip(a->display) < 0) {
        fprintf(stderr, "jtspeedtest: Wayland roundtrip failed\n");
        return 0;
    }

    if (!a->compositor || !a->shm || !a->wm_base) {
        fprintf(stderr, "jtspeedtest: missing wl_compositor, wl_shm "
                        "or xdg_wm_base\n");
        return 0;
    }
    if (!a->has_argb) {
        fprintf(stderr, "jtspeedtest: compositor has no ARGB8888 shm format\n");
        return 0;
    }

    a->surface = wl_compositor_create_surface(a->compositor);
    if (!a->surface) {
        fprintf(stderr, "jtspeedtest: cannot create wl_surface\n");
        return 0;
    }
    wl_surface_add_listener(a->surface, &surface_listener, a);

    if (wl_surface_get_version(a->surface) < 3) {
        a->scale = 1;
        a->fixed_scale = 1;
    }

    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, a->surface);
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);
    a->toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    xdg_toplevel_add_listener(a->toplevel, &toplevel_listener, a);
    xdg_toplevel_set_title(a->toplevel, "JTLab Speedtest");
    xdg_toplevel_set_app_id(a->toplevel, "jtspeedtest");
    xdg_toplevel_set_min_size(a->toplevel, (int)WIN_W, (int)WIN_H);
    xdg_toplevel_set_max_size(a->toplevel, (int)WIN_W, (int)WIN_H);

    if (a->deco_manager) {
        a->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, a->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(
            a->decoration, &decoration_listener, a);
        zxdg_toplevel_decoration_v1_set_mode(
            a->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }

    if (a->icon_manager) {
        struct xdg_toplevel_icon_v1 *icon =
            xdg_toplevel_icon_manager_v1_create_icon(a->icon_manager);
        if (icon) {
            xdg_toplevel_icon_v1_set_name(icon, "jtspeedtest");
            xdg_toplevel_icon_manager_v1_set_icon(a->icon_manager,
                                                  a->toplevel, icon);
            xdg_toplevel_icon_v1_destroy(icon);
        }
    }

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_commit(a->surface);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Live control socket                                                */
/* ------------------------------------------------------------------ */

static void
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtspeedtest-ctl.sock", rt);
}

static void
ctl_handle_line(App *a, char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';

    if (!strcmp(line, "quit")) {
        a->quit = 1;
        return;
    }

    if (strncmp(line, "accent", 6) == 0 &&
        (line[6] == ' ' || line[6] == '\t')) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        unsigned rgb;
        if (accent_parse(arg, &rgb)) {
            accent_set(rgb);
            /* Redraw on the next loop iteration instead of waiting for the
             * idle animation interval. */
            a->last_draw = -1.0;
        }
    }
}

static void
ctl_close_client(App *a)
{
    if (a->ctl_client_fd >= 0) {
        close(a->ctl_client_fd);
        a->ctl_client_fd = -1;
    }
    a->ctl_len = 0;
}

static void
ctl_process(App *a, int flush)
{
    size_t start = 0;
    for (size_t i = 0; i < a->ctl_len; i++) {
        if (a->ctl_buf[i] == '\n') {
            a->ctl_buf[i] = '\0';
            ctl_handle_line(a, a->ctl_buf + start);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(a->ctl_buf, a->ctl_buf + start, a->ctl_len - start);
        a->ctl_len -= start;
    }
    if (flush && a->ctl_len > 0) {
        a->ctl_buf[a->ctl_len] = '\0';
        ctl_handle_line(a, a->ctl_buf);
        a->ctl_len = 0;
    }
}

static void
ctl_read_client(App *a)
{
    char buf[128];
    for (;;) {
        ssize_t n = read(a->ctl_client_fd, buf, sizeof(buf));
        if (n > 0) {
            size_t room = sizeof(a->ctl_buf) - 1 - a->ctl_len;
            if ((size_t)n > room) {
                ctl_close_client(a);
                return;
            }
            memcpy(a->ctl_buf + a->ctl_len, buf, (size_t)n);
            a->ctl_len += (size_t)n;
            ctl_process(a, 0);
            continue;
        }
        if (n == 0) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        return;
    }
}

static void
ctl_accept(App *a)
{
    for (;;) {
        int fd = accept4(a->ctl_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (a->ctl_client_fd >= 0) {
            close(fd);
            continue;
        }
        a->ctl_client_fd = fd;
        a->ctl_len = 0;
    }
}

static int
ctl_start(App *a)
{
    char path[256];
    ctl_path(path, sizeof(path));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "jtspeedtest: control socket path too long: %s\n",
                path);
        return 0;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("jtspeedtest: control socket");
        return 0;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "jtspeedtest: cannot bind %s: %s\n",
                path, strerror(errno));
        close(fd);
        return 0;
    }
    if (listen(fd, 4) < 0) {
        fprintf(stderr, "jtspeedtest: cannot listen on %s: %s\n",
                path, strerror(errno));
        close(fd);
        unlink(path);
        return 0;
    }

    a->ctl_fd = fd;
    return 1;
}

static void
ctl_cleanup(App *a)
{
    ctl_close_client(a);
    if (a->ctl_fd >= 0) {
        close(a->ctl_fd);
        a->ctl_fd = -1;
        char path[256];
        ctl_path(path, sizeof(path));
        unlink(path);
    }
}

/* Returns < 0 on a fatal display error, 0 otherwise. */
static int pump_events(App *a, int timeout_ms)
{
    if (wl_display_dispatch_pending(a->display) < 0)
        return -1;

    int want_write = 0;
    if (wl_display_flush(a->display) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        want_write = 1;
    }

    struct pollfd pfd[3];
    pfd[0].fd = wl_display_get_fd(a->display);
    pfd[0].events = POLLIN | (want_write ? POLLOUT : 0);
    pfd[0].revents = 0;

    int nfds = 1;
    int ctl_idx = -1;
    int ctl_client_idx = -1;
    if (a->ctl_fd >= 0) {
        ctl_idx = nfds;
        pfd[nfds].fd = a->ctl_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if (a->ctl_client_fd >= 0) {
        ctl_client_idx = nfds;
        pfd[nfds].fd = a->ctl_client_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }

    int pr = poll(pfd, (nfds_t)nfds, timeout_ms);
    if (pr < 0)
        return (errno == EINTR) ? 0 : -1;
    if (pr == 0)
        return 0;

    if (pfd[0].revents & POLLOUT) {
        if (wl_display_flush(a->display) < 0 && errno != EAGAIN)
            return -1;
    }
    if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    if (pfd[0].revents & POLLIN) {
        if (wl_display_dispatch(a->display) < 0)
            return -1;
    }
    if (ctl_idx >= 0 && (pfd[ctl_idx].revents & POLLIN))
        ctl_accept(a);
    if (ctl_client_idx >= 0 && pfd[ctl_client_idx].revents)
        ctl_read_client(a);
    return 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_signal_quit = 1;
}

static void app_cleanup(App *a)
{
    ctl_cleanup(a);

    if (a->worker_alive) {
        atomic_store_explicit(&g_cancel, 1, memory_order_relaxed);
        pthread_join(a->worker, NULL);
        a->worker_alive = 0;
    }

    if (a->keyboard)
        wl_keyboard_destroy(a->keyboard);
    if (a->cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
        a->cursor_shape_device = NULL;
    }
    if (a->pointer)
        wl_pointer_destroy(a->pointer);
    if (a->cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(a->cursor_shape_manager);
    if (a->decoration)
        zxdg_toplevel_decoration_v1_destroy(a->decoration);
    if (a->toplevel)
        xdg_toplevel_destroy(a->toplevel);
    if (a->xdg_surface)
        xdg_surface_destroy(a->xdg_surface);
    if (a->surface)
        wl_surface_destroy(a->surface);
    if (a->seat)
        wl_seat_destroy(a->seat);
    if (a->deco_manager)
        zxdg_decoration_manager_v1_destroy(a->deco_manager);
    if (a->icon_manager)
        xdg_toplevel_icon_manager_v1_destroy(a->icon_manager);
    if (a->wm_base)
        xdg_wm_base_destroy(a->wm_base);
    if (a->shm)
        wl_shm_destroy(a->shm);
    if (a->compositor)
        wl_compositor_destroy(a->compositor);

    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output)
            wl_output_destroy(a->outputs[i].output);
    }
    if (a->registry)
        wl_registry_destroy(a->registry);

    if (a->xkb_state)
        xkb_state_unref(a->xkb_state);
    if (a->xkb_keymap)
        xkb_keymap_unref(a->xkb_keymap);
    if (a->xkb_ctx)
        xkb_context_unref(a->xkb_ctx);

    if (a->display) {
        for (int i = 0; i < NBUFS; i++) {
            ShmBuf *b = &a->bufs[i];
            if (b->buffer && !b->busy) {
                wl_buffer_destroy(b->buffer);
                b->buffer = NULL;
            }
        }
        wl_display_flush(a->display);
        wl_display_disconnect(a->display);
        a->display = NULL;
    }

    /* The compositor keeps its own mapping of each shm pool, so it is safe
     * to drop ours after the connection is gone. */
    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->data) {
            munmap(b->data, b->size);
            b->data = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Screenshot mode (also handy as a preview)                          */
/* ------------------------------------------------------------------ */

static int write_screenshot(const char *path)
{
    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, (int)WIN_W, (int)WIN_H);
    cairo_t *cr = cairo_create(surf);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    rgba(cr, C_BG, 1.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    Snap s;
    memset(&s, 0, sizeof s);
    s.phase = PH_DOWNLOAD;
    s.running = 1;
    s.ping = 18.4;
    s.jitter = 2.1;
    s.live = 287.4;
    s.progress = 0.62;
    snprintf(s.status, sizeof s.status, "Measuring download…");
    snprintf(s.server, sizeof s.server, "Cloudflare · JNB, ZA");
    snprintf(s.ip, sizeof s.ip, "197.185.152.158");

    View v;
    v.s = &s;
    v.t = 1.9;
    v.disp = 287.4;
    v.hover_start = 0;
    v.press_start = 0;
    v.hover_close = 0;
    v.done_t = -1.0;

    paint_content(cr, &v);

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);

    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "jtspeedtest: could not write %s: %s\n",
                path, cairo_status_to_string(st));
        return 1;
    }
    printf("wrote %s\n", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (!accent_cli(&argc, argv))
        return 2;

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: %s [ACCENT] [--screenshot PATH]\n\n"
               "  ACCENT              #RRGGBB or RRGGBB, for example '#378484'\n"
               "  --accent COLOR      explicit form of ACCENT\n"
               "  --screenshot PATH   render a demo frame to PATH and exit\n\n"
               "the default accent is #E8A03C (amber)\n\n"
               "controls: Space/Enter start or stop, Esc/q quit\n"
               "live accent: jtspeedtestctl accent '#RRGGBB'\n"
               "requires a Wayland compositor supporting xdg-shell\n", argv[0]);
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "--screenshot")) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s --screenshot PATH\n", argv[0]);
            return 2;
        }
        return write_screenshot(argv[2]);
    }

    App a;
    memset(&a, 0, sizeof a);
    a.done_t = -1.0;
    a.prev_phase = PH_IDLE;
    a.scale = 1;
    a.ctl_fd = -1;
    a.ctl_client_fd = -1;

    const char *scale_env = getenv("JTSPEEDTEST_SCALE");
    if (scale_env && *scale_env) {
        int s = (int)lround(atof(scale_env));
        if (s < 1) s = 1;
        if (s > 3) s = 3;
        a.scale = s;
        a.fixed_scale = 1;
    }

    pthread_mutex_init(&a.net.lock, NULL);
    a.net.phase = PH_IDLE;
    snprintf(a.net.status, sizeof a.net.status, "Ready when you are.");
    snprintf(a.net.server, sizeof a.net.server, "Cloudflare");

    a.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!a.xkb_ctx) {
        fprintf(stderr, "jtspeedtest: cannot create xkb context\n");
        pthread_mutex_destroy(&a.net.lock);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!wayland_start(&a)) {
        app_cleanup(&a);
        pthread_mutex_destroy(&a.net.lock);
        curl_global_cleanup();
        return 1;
    }

    ctl_start(&a);

    a.t0 = mono_sec();
    double last = mono_sec();

    while (!a.quit && !g_signal_quit) {
        app_reap(&a);

        Snap s;
        net_snapshot(&a.net, &s);

        double now = mono_sec();
        double dt = now - last;
        if (dt > 0.12) dt = 0.12;
        if (dt < 0.0) dt = 0.0;
        last = now;

        if (s.phase != a.prev_phase) {
            if (s.phase == PH_DONE)
                a.done_t = now - a.t0;
            a.prev_phase = s.phase;
        }

        double target = 0.0;
        if (s.phase == PH_DOWNLOAD || s.phase == PH_UPLOAD)
            target = s.live;
        else if (s.phase == PH_DONE)
            target = s.dl;

        double k = 1.0 - exp(-dt * 4.0);
        a.disp += (target - a.disp) * k;
        if (fabs(target - a.disp) < 0.003)
            a.disp = target;

        double interval = (s.running || fabs(target - a.disp) > 0.01)
                          ? 1.0 / 30.0 : 1.0 / 15.0;
        if (a.configured && now - a.last_draw >= interval) {
            app_draw(&a, &s);
            a.last_draw = now;
        }

        int timeout;
        if (!a.configured) {
            timeout = 100;
        } else {
            double wait = interval - (mono_sec() - a.last_draw);
            if (wait < 0.0)
                wait = 0.0;
            timeout = (int)(wait * 1000.0) + 1;
        }

        if (pump_events(&a, timeout) < 0)
            break;
    }

    app_cleanup(&a);

    pthread_mutex_destroy(&a.net.lock);
    curl_global_cleanup();

    return 0;
}
