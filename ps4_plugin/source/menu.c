// menu.c - open a Skylanders dump menu with a DualShock 4 button combo.
//
// How it works
//   * Hooks scePadReadState / scePadRead (libScePad) with the plugin's own hook.c.
//   * Hold the combo (default L1+R1+L3+R3) for 0.5 s -> menu opens.
//   * While open, the game sees a neutral controller; the d-pad / face buttons drive the menu.
//   * The menu is drawn with PS4 notifications (a plugin cannot safely draw into the game's screen).
//   * File access and notifications happen on the menu thread, never on the game's pad thread.
//
// Optional files in /data/goldportal/
//   nomenu           - empty file: disable the menu
//   menu_combo.txt   - hex button mask, e.g. "C06" (see button bits below)
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

#include "menu.h"
#include "portal.h"
#include "hook.h"
#include "log.h"
#include <GoldHEN.h>      // OrbisNotificationRequest (same include order as main.c)

#define SKY_DIR      "/data/skylanders"
#define COMBO_FILE   GP_DATA_DIR "/menu_combo.txt"
#define NOMENU_FILE  GP_DATA_DIR "/nomenu"

#define MAX_FILES    512
#define NAME_LEN     100          // longest file name we list (incl. NUL)
#define PATH_LEN     192
#define PAGE_ROWS    4
#define HOLD_US      500000       // combo must be held this long
#define REPEAT_DELAY 400000       // d-pad auto repeat
#define REPEAT_RATE  110000
#define DEDUPE_US    1500         // nested pad calls inside one frame count as one sample

// DualShock 4 button bits (OrbisPadData.buttons)
#define MB_L3        0x00000002u
#define MB_R3        0x00000004u
#define MB_OPTIONS   0x00000008u
#define MB_UP        0x00000010u
#define MB_RIGHT     0x00000020u
#define MB_DOWN      0x00000040u
#define MB_LEFT      0x00000080u
#define MB_L2        0x00000100u
#define MB_R2        0x00000200u
#define MB_L1        0x00000400u
#define MB_R1        0x00000800u
#define MB_TRIANGLE  0x00001000u
#define MB_CIRCLE    0x00002000u
#define MB_CROSS     0x00004000u
#define MB_SQUARE    0x00008000u
#define MB_TOUCH     0x00100000u

#define DEFAULT_COMBO (MB_L1 | MB_R1 | MB_L3 | MB_R3)
#define VALID_COMBO_BITS (MB_L3 | MB_R3 | MB_OPTIONS | MB_UP | MB_RIGHT | MB_DOWN | MB_LEFT | \
                          MB_L2 | MB_R2 | MB_L1 | MB_R1 | MB_TRIANGLE | MB_CIRCLE | MB_CROSS | \
                          MB_SQUARE | MB_TOUCH)

// First bytes of OrbisPadData (same layout in the OpenOrbis and Sony headers).
typedef struct {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;     // sticks, 0x80 = centre
    uint8_t  l2, r2;             // analog triggers
} pad_head;

// FreeBSD 9 dirent as returned by sceKernelGetdents.
typedef struct {
    uint32_t d_fileno;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_namlen;
    char     d_name[256];
} sky_dirent;

enum { ACT_NONE = 0, ACT_OPEN, ACT_CLOSE, ACT_UP, ACT_DOWN, ACT_PGUP, ACT_PGDN,
       ACT_SELECT, ACT_TOGGLE, ACT_SAVE };

// ---------------------------------------------------------------------------
// State shared between the game's pad thread (hook) and the menu thread.
// Everything below is protected by g_lock.
// ---------------------------------------------------------------------------
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_stop;
static int             g_open;                 // menu is open: input is swallowed
static int             g_swallow;              // keep blanking input until all buttons are released
static int             g_handle = -1;          // controller that opened the menu
static uint32_t        g_combo = DEFAULT_COMBO;
static uint32_t        g_prev;                 // buttons in the previous sample (menu open)
static uint64_t        g_rep_at;
static uint64_t        g_hold_t;               // when the combo started being held (0 = not held)
static int             g_hold_h = -1;
static uint64_t        g_last_logic_us;
static int             g_last_logic_h = -1;
static int             g_q[32], g_qh, g_qt;    // action queue

static void push_action(int a)                 // lock held
{
    int next = (g_qt + 1) % 32;
    if (next == g_qh) return;                  // full: drop
    g_q[g_qt] = a;
    g_qt = next;
}

static int pop_action(void)
{
    int a = ACT_NONE;
    pthread_mutex_lock(&g_lock);
    if (g_qh != g_qt) { a = g_q[g_qh]; g_qh = (g_qh + 1) % 32; }
    pthread_mutex_unlock(&g_lock);
    return a;
}

static void blank_pad(pad_head *p)
{
    p->buttons = 0;
    p->lx = p->ly = p->rx = p->ry = 0x80;
    p->l2 = p->r2 = 0;
}

static void close_menu_locked(void)
{
    g_open = 0;
    g_swallow = 1;                             // game must not see the buttons still held
    g_hold_t = 0;
    push_action(ACT_CLOSE);
}

static void menu_input_locked(uint32_t cur, uint64_t now)
{
    uint32_t pressed = cur & ~g_prev;
    uint32_t dirs = cur & (MB_UP | MB_DOWN);
    if (dirs) {
        if (pressed & dirs) g_rep_at = now + REPEAT_DELAY;
        else if (now >= g_rep_at) { pressed |= dirs; g_rep_at = now + REPEAT_RATE; }
    }
    g_prev = cur;

    // Pressing the whole combo again closes the menu (it can only be a new press
    // after at least one of its buttons was released).
    if ((pressed & g_combo) && (cur & g_combo) == g_combo) { close_menu_locked(); return; }
    if (pressed & MB_CIRCLE) { close_menu_locked(); return; }

    if (pressed & MB_UP)            push_action(ACT_UP);
    else if (pressed & MB_DOWN)     push_action(ACT_DOWN);
    if (pressed & MB_LEFT)          push_action(ACT_PGUP);
    if (pressed & MB_RIGHT)         push_action(ACT_PGDN);
    if (pressed & MB_CROSS)         push_action(ACT_SELECT);
    if (pressed & MB_TRIANGLE)      push_action(ACT_TOGGLE);
    if (pressed & MB_SQUARE)        push_action(ACT_SAVE);
}

// Called with every sample the game reads. Cheap: no file access, no notifications.
static void pad_filter(int32_t handle, pad_head *p)
{
    uint64_t now = sceKernelGetProcessTime();
    uint32_t cur = p->buttons;
    int do_mask = 0;

    pthread_mutex_lock(&g_lock);
    int dup = (handle == g_last_logic_h) && (now - g_last_logic_us < DEDUPE_US);
    if (!dup) { g_last_logic_us = now; g_last_logic_h = handle; }

    if (g_open) {
        if (handle == g_handle) {
            do_mask = 1;
            if (!dup) menu_input_locked(cur, now);
        }
    } else if (g_swallow) {
        if (handle == g_handle) {
            if (cur) do_mask = 1; else g_swallow = 0;
        }
    } else if (!dup) {
        if ((cur & g_combo) == g_combo) {
            if (g_hold_t == 0 || g_hold_h != handle) { g_hold_t = now; g_hold_h = handle; }
            else if (now - g_hold_t >= HOLD_US) {
                g_open = 1;
                g_handle = handle;
                g_prev = cur;                  // buttons held right now are not "presses"
                g_rep_at = 0;
                g_hold_t = 0;
                push_action(ACT_OPEN);
                do_mask = 1;
            }
        } else if (g_hold_h == handle) {
            g_hold_t = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (do_mask) blank_pad(p);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static hook_rec g_rec_state, g_rec_read;
static int g_hook_state_ok, g_hook_read_ok;

static int h_scePadReadState(int32_t handle, void *data)
{
    int r = ((int (*)(int32_t, void *))g_rec_state.orig)(handle, data);
    if (r == 0 && data) pad_filter(handle, (pad_head *)data);
    return r;
}

static int h_scePadRead(int32_t handle, void *data, int32_t count)
{
    int r = ((int (*)(int32_t, void *, int32_t))g_rec_read.orig)(handle, data, count);
    if (r > 0 && data) pad_filter(handle, (pad_head *)data);   // newest sample only
    return r;
}

// ---------------------------------------------------------------------------
// Small helpers (menu thread only)
// ---------------------------------------------------------------------------
static void toast(const char *fmt, ...)
{
    static OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.targetId = -1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void app(char *m, size_t sz, size_t *o, const char *fmt, ...)
{
    if (*o >= sz - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(m + *o, sz - *o, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *o += (size_t)n;
    if (*o > sz - 1) *o = sz - 1;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (b > a) return 0;
    for (size_t i = 0; i < b; i++)
        if (lower((unsigned char)s[a - b + i]) != lower((unsigned char)suf[i])) return 0;
    return 1;
}

static int cmp_names(const void *a, const void *b)
{
    const char *x = a, *y = b;
    for (;; x++, y++) {
        int cx = lower((unsigned char)*x), cy = lower((unsigned char)*y);
        if (cx != cy) return cx - cy;
        if (!cx) return 0;
    }
}

static int read_all(const char *path, uint8_t *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    return (int)got;
}

static int write_new(const char *path, const uint8_t *buf, size_t len)   // create/truncate
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) return -1;
    ssize_t n = write(fd, buf, len);
    fsync(fd);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static long file_size(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t e = lseek(fd, 0, SEEK_END);
    close(fd);
    return (long)e;
}

// ---------------------------------------------------------------------------
// Menu model (menu thread only)
// ---------------------------------------------------------------------------
static char     g_names[MAX_FILES][NAME_LEN];
static int      g_nfiles;
static char     g_dirbuf[65536];
static uint8_t  g_filebuf[FIGURE_SIZE * 4 + 16];

static char     g_src[PORTAL_SLOTS][PATH_LEN];          // file a slot was loaded from (via menu)
static char     g_srcname[PORTAL_SLOTS][FIGURE_NAME_LEN];

static int      g_visible;                              // menu open (menu thread's view)
static int      g_mode;                                 // 0 = files, 1 = portal slots
static int      g_cur[2];
static char     g_status[48];
static int      g_need_draw;
static uint64_t g_draw_at, g_last_toast;

static int scan_files(void)
{
    g_nfiles = 0;
    mkdir(SKY_DIR, 0777);                               // harmless if it exists
    int fd = sceKernelOpen(SKY_DIR, 0, 0);
    if (fd < 0) { LOG("menu: cannot open " SKY_DIR " (0x%x)", fd); return -1; }

    for (;;) {
        int n = sceKernelGetdents(fd, g_dirbuf, (int)sizeof(g_dirbuf));
        if (n <= 0) break;
        for (int pos = 0; pos < n;) {
            sky_dirent *d = (sky_dirent *)(g_dirbuf + pos);
            if (d->d_reclen == 0) { pos = n; break; }
            pos += d->d_reclen;

            if (d->d_type == 4) continue;                          // directory
            if (d->d_name[0] == 0 || d->d_name[0] == '.') continue;
            size_t nl = strlen(d->d_name);
            if (nl >= NAME_LEN) continue;
            if (ends_with(d->d_name, ".bak") || ends_with(d->d_name, ".txt") ||
                ends_with(d->d_name, ".tmp")) continue;
            if (g_nfiles >= MAX_FILES) continue;

            char path[PATH_LEN];
            snprintf(path, sizeof(path), SKY_DIR "/%s", d->d_name);
            long sz = file_size(path);
            if (sz < FIGURE_SIZE || sz > FIGURE_SIZE * 4) continue;

            memcpy(g_names[g_nfiles], d->d_name, nl + 1);
            g_nfiles++;
        }
    }
    sceKernelClose(fd);
    qsort(g_names, (size_t)g_nfiles, NAME_LEN, cmp_names);
    LOG("menu: %d dump file(s) in " SKY_DIR, g_nfiles);
    return g_nfiles;
}

static int stable_dump(int slot, uint8_t *out)
{
    uint8_t a[FIGURE_SIZE], b[FIGURE_SIZE];
    for (int tries = 0; tries < 5; tries++) {
        if (portal_get_dump(slot, a) != 0) return -1;
        usleep(60000);
        if (portal_get_dump(slot, b) != 0) return -1;
        if (memcmp(a, b, FIGURE_SIZE) == 0) { memcpy(out, a, FIGURE_SIZE); return 0; }
    }
    return -1;
}

// Writes the slot's changed data back to the dump it came from.
// Returns 1 = saved, 0 = nothing to do, -1 = error.
static int save_slot(int slot)
{
    portal_slot_info in;
    portal_get_info(slot, &in);
    if (!in.dirty || !(in.loaded || in.name[0])) return 0;

    uint8_t dump[FIGURE_SIZE];
    if (stable_dump(slot, dump) != 0) return -1;

    if (!g_src[slot][0] || strcmp(g_srcname[slot], in.name) != 0) {
        // Not loaded from the menu (PC app / browser): keep the old backup behaviour.
        char path[PATH_LEN];
        int o = snprintf(path, sizeof(path), GP_DATA_DIR "/backup_");
        for (const char *s = in.name; *s && o < (int)sizeof(path) - 2; s++)
            path[o++] = (*s == '/' || *s == '\\') ? '_' : *s;
        path[o] = 0;
        return write_new(path, dump, FIGURE_SIZE) == 0 ? 1 : -1;
    }

    char bak[PATH_LEN + 8];
    snprintf(bak, sizeof(bak), "%s.bak", g_src[slot]);
    if (!file_exists(bak)) {                                       // keep the pre-session original
        int n = read_all(g_src[slot], g_filebuf, sizeof(g_filebuf));
        if (n > 0) write_new(bak, g_filebuf, (size_t)n);
    }
    // Overwrite the first 1024 bytes in place (no truncate, so larger dumps keep their tail).
    int fd = open(g_src[slot], O_WRONLY);
    if (fd < 0) { LOG("menu: cannot write %s", g_src[slot]); return -1; }
    ssize_t w = write(fd, dump, FIGURE_SIZE);
    fsync(fd);
    close(fd);
    if (w != FIGURE_SIZE) return -1;
    LOG("menu: saved slot %d -> %s", slot + 1, g_src[slot]);
    return 1;
}

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

static void load_selected(void)
{
    if (g_nfiles == 0) { set_status("No dumps found"); return; }
    const char *name = g_names[g_cur[0]];

    int free_slot = -1;
    for (int i = 0; i < PORTAL_SLOTS; i++) {
        portal_slot_info in;
        portal_get_info(i, &in);
        if (in.loaded && g_src[i][0] && strcmp(g_srcname[i], name) == 0 && strcmp(in.name, name) == 0) {
            set_status("Already in slot %d", i + 1);
            return;
        }
        if (!in.loaded && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) { set_status("Portal full! Tri, then X"); return; }

    char path[PATH_LEN];
    snprintf(path, sizeof(path), SKY_DIR "/%s", name);
    int n = read_all(path, g_filebuf, sizeof(g_filebuf));
    if (n < FIGURE_SIZE) { set_status("Read error"); LOG("menu: read failed %s", path); return; }

    char nm[FIGURE_NAME_LEN];
    memset(nm, 0, sizeof(nm));
    strncpy(nm, name, sizeof(nm) - 1);
    if (portal_load(free_slot, g_filebuf, (size_t)n, nm) != 0) { set_status("Load failed"); return; }

    snprintf(g_src[free_slot], sizeof(g_src[free_slot]), "%s", path);
    snprintf(g_srcname[free_slot], sizeof(g_srcname[free_slot]), "%s", nm);
    LOG("menu: loaded '%s' into slot %d", name, free_slot + 1);
    set_status("Slot %d: %.28s", free_slot + 1, name);
}

static void remove_selected(void)
{
    int slot = g_cur[1];
    portal_slot_info in;
    portal_get_info(slot, &in);
    if (!in.loaded) { set_status("Slot %d is empty", slot + 1); return; }
    int saved = save_slot(slot);
    portal_remove(slot);
    g_src[slot][0] = 0;
    g_srcname[slot][0] = 0;
    LOG("menu: removed slot %d (save result %d)", slot + 1, saved);
    set_status(saved > 0 ? "Slot %d removed+saved" : "Slot %d removed", slot + 1);
}

static void save_all(void)
{
    int saved = 0, err = 0;
    for (int i = 0; i < PORTAL_SLOTS; i++) {
        portal_slot_info in;
        portal_get_info(i, &in);
        if (!in.loaded) continue;
        int r = save_slot(i);
        if (r > 0) saved++; else if (r < 0) err++;
    }
    if (err) set_status("Saved %d, %d failed", saved, err);
    else if (saved) set_status("Saved %d figure(s)", saved);
    else set_status("Nothing changed");
}

static void draw(void)
{
    char m[190];
    size_t o = 0;
    int n = g_mode == 0 ? g_nfiles : PORTAL_SLOTS;
    int cur = g_cur[g_mode];
    int page = n ? cur / PAGE_ROWS : 0;
    int pages = n ? (n + PAGE_ROWS - 1) / PAGE_ROWS : 1;

    if (g_mode == 0) app(m, sizeof(m), &o, "Goldportal FILES %d/%d  pg %d/%d\n", n ? cur + 1 : 0, n, page + 1, pages);
    else             app(m, sizeof(m), &o, "Goldportal PORTAL  pg %d/%d\n", page + 1, pages);

    if (g_mode == 0 && n == 0) app(m, sizeof(m), &o, "No dumps in " SKY_DIR "\n");

    for (int r = 0; r < PAGE_ROWS; r++) {
        int idx = page * PAGE_ROWS + r;
        if (idx >= n) break;
        char mark = idx == cur ? '>' : ' ';
        if (g_mode == 0) {
            app(m, sizeof(m), &o, "%c %.24s\n", mark, g_names[idx]);
        } else {
            portal_slot_info in;
            portal_get_info(idx, &in);
            if (in.loaded) app(m, sizeof(m), &o, "%c %d: %.20s%s\n", mark, idx + 1, in.name, in.dirty ? " *" : "");
            else           app(m, sizeof(m), &o, "%c %d: (empty)\n", mark, idx + 1);
        }
    }

    if (g_status[0])      app(m, sizeof(m), &o, "%s", g_status);
    else if (g_mode == 0) app(m, sizeof(m), &o, "X Load  Tri Portal  Sq Save  O Close");
    else                  app(m, sizeof(m), &o, "X Remove  Tri Files  Sq Save  O Close");
    g_status[0] = 0;

    toast("%s", m);
}

static void request_draw(int delay_us)
{
    g_need_draw = 1;
    g_draw_at = sceKernelGetProcessTime() + delay_us;
}
