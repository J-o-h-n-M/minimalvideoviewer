// MinimalVideoViewer: a tiny looping video player built on libmpv.
//
//   Mouse wheel        zoom in/out at the cursor (zoom persists across loops)
//   Left drag          pan while zoomed
//   Double-click / F   toggle fullscreen (with several videos: double-click shows
//                      only that video, double-click again returns to the grid)
//   Right-click        menu (audio track, subtitles, jump distance, add/replace/close
//                      video, fullscreen, reset zoom, quit). Dropping a subtitle file
//                      loads it into the video it is dropped on.
//   R                  reset zoom
//   Space / K          play/pause (also leaves fast forward/reverse)
//   J / L              VCR-style fast reverse / fast forward (press again: faster)
//   Left/Right         jump back / forward (distance set in the menu, default 5 s)
//   Up/Down, M         volume, mute
//   O / A              open a file in this video's place / add videos
//   Delete             close the video under the mouse (when there are several)
//   Esc                leave fullscreen or single-video view,  Q  quit
//
// Several videos play at once in a grid: drop more files onto the window or use
// "Add videos". Mouse and keys act on the video under the mouse pointer, which is
// also the only one with sound. Hold Shift with Space, J/L, Left/Right or R to act
// on all videos.
//
// Moving the mouse shows a control bar (progress, rewind, play/pause, fast forward)
// on the video under the pointer that hides again after 5 seconds without mouse
// movement. Volume changes and jumps are briefly shown at the top of the video.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpv/client.h>

#define WM_MPV_WAKEUP   (WM_APP + 1)  // lParam = Tile*
#define FORWARDED_MOUSE 0x10000       // wParam marker: mouse message passed on by a tile
#define ZOOM_STEP 0.125 // log2 units per wheel notch (~9% per notch)
#define ZOOM_MAX  6.0   // 64x
#define MAX_TILES 16

#define BAR_TIMER   1
#define BAR_HIDE_MS 5000
#define BAR_OVERLAY "1"       // osd-overlay ids
#define OSD_OVERLAY "2"
#define OSD_TIMER   2
#define OSD_MS      1200
#define ACCENT      0x3C8CFF  // ASS colours are BGR: orange
#define REG_KEY     L"Software\\MinimalVideoViewer"

#define SUB_MARGIN  22        // mpv's default sub-margin-y (units of a 720 px tall window)
#define MAX_TRACKS  100
#define ASS_SIZE    8192

// Menu ids. Audio/subtitle items are ID_AUDIO/ID_SUB + mpv track id (+ 0 = off).
enum { ID_OPEN = 1, ID_ADD, ID_SOLO, ID_CLOSE_TILE, ID_FULLSCREEN, ID_RESET_ZOOM, ID_QUIT,
       ID_SUB_LOAD, ID_JUMP = 100, ID_AUDIO = 200, ID_SUB = 400 };

static const wchar_t kVideoFilter[] = L"Video files\0*.mp4;*.mkv;*.webm;*.mov;*.avi;*.wmv;*.m4v;*.flv;*.ts;*.mts;*.m2ts;*.mpg;*.mpeg;*.gif\0All files\0*.*\0";
static const wchar_t kSubFilter[] = L"Subtitles\0*.srt;*.ass;*.ssa;*.vtt;*.sub;*.idx;*.sup\0All files\0*.*\0";

// One video: a child window that its own mpv instance renders into.
typedef struct Tile {
    HWND hwnd;
    mpv_handle *mpv;
    RECT rc;                // position in the main window's client area
    wchar_t name[MAX_PATH];
    BOOL closing;           // mpv is shutting down; no longer part of the grid
    BOOL sized;             // first video reconfig of the current file handled

    // View state, mirrored into mpv's video-zoom / video-pan-x / video-pan-y.
    // These are mpv options, so they survive every loop of the file.
    double zoom, panx, pany;
    double vw, vh;          // displayed video size in pixels (after rotation)

    double time, duration;
    BOOL paused;

    // Shuttle (VCR fast forward/reverse): 0 = normal play, +n = forward at
    // kShuttle[n-1]x, -n = reverse at kShuttle[n-1]x.
    int shuttle;
    char shuttle_aid[32];   // audio track to restore after shuttling

    int mute_sent;          // last values sent to mpv, -1 = none yet
    int sub_margin_sent;
    char bar_last[ASS_SIZE], osd_last[ASS_SIZE];
} Tile;

static HWND g_hwnd;
static BOOL g_closing;

static Tile *g_tiles[MAX_TILES];
static int g_ntiles;          // including tiles that are shutting down
static Tile *g_hover_tile;    // tile under the mouse pointer
static Tile *g_audio_tile;    // tile with sound that the keys act on (last hovered)
static Tile *g_bar_tile;      // tile showing the control bar
static Tile *g_osd_tile;      // tile showing the message box
static Tile *g_capture_tile;  // tile being panned or scrubbed
static Tile *g_solo;          // tile shown alone (double-click), NULL = grid
static BOOL g_restored_placement; // window size/state comes from the last session

static BOOL g_dragging;
static POINT g_drag_last;
static BOOL g_closed_by_click; // the last click hit a video's X

static BOOL g_fullscreen;
static WINDOWPLACEMENT g_wp = { sizeof(WINDOWPLACEMENT) };

// Control bar state.
enum { HIT_NONE, HIT_BAR, HIT_TRACK, HIT_REW, HIT_PLAY, HIT_FF, HIT_CLOSE };
static BOOL g_bar_visible;
static int g_hover = HIT_NONE; // part of the bar under the mouse
static BOOL g_scrubbing;
static double g_scrub_frac;
static POINT g_last_mouse = { -1, -1 };

static const double kShuttle[] = { 2, 4, 8, 16 };

// Brief on-screen message for volume changes and jumps.
enum { OSD_NONE, OSD_VOLUME, OSD_SEEK, OSD_TEXT };
static char g_osd_text[160];
static int g_osd;
static int g_seek_total; // seconds jumped by the current run of key presses
static double g_volume = 100;
static BOOL g_muted;

// Left/Right jump distance in seconds, chosen in the right-click menu and saved
// in the registry.
static const int kJumps[] = { 1, 2, 3, 5, 10, 15, 30, 60 };
static int g_jump = 5;

static void draw_bar(Tile *t);
static void draw_osd(Tile *t);

static void mpv_cmd(Tile *t, const char *a, const char *b, const char *c)
{
    const char *cmd[] = { a, b, c, NULL };
    mpv_command_async(t->mpv, 0, cmd);
}

// ---------------------------------------------------------------------------
// Tiles

static int live_tiles(Tile **out)
{
    int n = 0;
    for (int i = 0; i < g_ntiles; i++)
        if (!g_tiles[i]->closing)
            out[n++] = g_tiles[i];
    return n;
}

static BOOL tile_exists(Tile *t)
{
    for (int i = 0; i < g_ntiles; i++)
        if (g_tiles[i] == t)
            return TRUE;
    return FALSE;
}

// Visible tile at a point in the main window's client area.
static Tile *tile_at(int x, int y)
{
    POINT pt = { x, y };
    for (int i = 0; i < g_ntiles; i++) {
        Tile *t = g_tiles[i];
        if (!t->closing && (!g_solo || t == g_solo) && PtInRect(&t->rc, pt))
            return t;
    }
    return NULL;
}

static int tile_w(Tile *t) { return t->rc.right - t->rc.left; }
static int tile_h(Tile *t) { return t->rc.bottom - t->rc.top; }

// Only the tile under the mouse (the last one hovered) has sound.
static void apply_audio(void)
{
    for (int i = 0; i < g_ntiles; i++) {
        Tile *t = g_tiles[i];
        int mute = g_muted || t != g_audio_tile;
        if (!t->closing && mute != t->mute_sent) {
            t->mute_sent = mute;
            mpv_cmd(t, "set", "mute", mute ? "yes" : "no");
        }
    }
}

static void update_title(void)
{
    Tile *v[MAX_TILES];
    wchar_t title[MAX_PATH + 32];
    int n = live_tiles(v);
    if (n == 0)
        return;
    if (n == 1 || g_solo)
        swprintf(title, ARRAYSIZE(title), L"%s", g_solo ? g_solo->name : v[0]->name);
    else
        swprintf(title, ARRAYSIZE(title), L"%d videos", n);
    SetWindowTextW(g_hwnd, title);
}

// ---------------------------------------------------------------------------
// Zoom and pan (coordinates relative to the tile)

// Size of the video on screen at the current zoom, plus the tile size.
static BOOL view_metrics(Tile *t, double *w, double *h, double *ww, double *wh)
{
    if (t->vw <= 0 || t->vh <= 0)
        return FALSE;
    *ww = tile_w(t);
    *wh = tile_h(t);
    double fit = min(*ww / t->vw, *wh / t->vh) * exp2(t->zoom);
    *w = t->vw * fit;
    *h = t->vh * fit;
    return *w > 0 && *h > 0;
}

// mpv places the video centre at win/2 + pan * size. Keep the video covering the
// tile on any axis where it is larger than the tile; centre it otherwise.
static double clamp_pan(double pan, double size, double win)
{
    if (size <= win)
        return 0;
    double m = (size - win) / (2 * size);
    return pan > m ? m : pan < -m ? -m : pan;
}

static void apply_view(Tile *t)
{
    double w, h, ww, wh;
    if (view_metrics(t, &w, &h, &ww, &wh)) {
        t->panx = clamp_pan(t->panx, w, ww);
        t->pany = clamp_pan(t->pany, h, wh);
    }
    mpv_set_property_async(t->mpv, 0, "video-zoom", MPV_FORMAT_DOUBLE, &t->zoom);
    mpv_set_property_async(t->mpv, 0, "video-pan-x", MPV_FORMAT_DOUBLE, &t->panx);
    mpv_set_property_async(t->mpv, 0, "video-pan-y", MPV_FORMAT_DOUBLE, &t->pany);
}

static void reset_view(Tile *t)
{
    t->zoom = t->panx = t->pany = 0;
    apply_view(t);
}

// Zoom by dz (log2 units) keeping the video point under (mx, my) fixed.
static void zoom_at(Tile *t, int mx, int my, double dz)
{
    double w, h, ww, wh;
    if (!view_metrics(t, &w, &h, &ww, &wh))
        return;
    double u = (mx - ww / 2) / w - t->panx;
    double v = (my - wh / 2) / h - t->pany;
    double z = t->zoom + dz;
    t->zoom = z < 0 ? 0 : z > ZOOM_MAX ? ZOOM_MAX : z;
    view_metrics(t, &w, &h, &ww, &wh);
    t->panx = (mx - ww / 2) / w - u;
    t->pany = (my - wh / 2) / h - v;
    apply_view(t);
}

static void pan_by(Tile *t, int dx, int dy)
{
    double w, h, ww, wh;
    if (!view_metrics(t, &w, &h, &ww, &wh))
        return;
    t->panx += dx / w;
    t->pany += dy / h;
    apply_view(t);
}

// ---------------------------------------------------------------------------
// Playback

static void set_shuttle(Tile *t, int s)
{
    char speed[16];
    if (s == 0 && t->shuttle == 0)
        return;
    if ((s < 0) != (t->shuttle < 0)) {
        // Real backward decoding gives smooth, frame-by-frame reverse. Like a tape,
        // rewinding stops at the start (keep-open pauses there instead of looping).
        mpv_cmd(t, "set", "play-direction", s < 0 ? "backward" : "forward");
        mpv_cmd(t, "set", "loop-file", s < 0 ? "no" : "inf");
    }
    if ((s != 0) != (t->shuttle != 0)) {
        // No sound while shuttling (like a VCR); skipping audio decoding and allowing
        // the decoder to drop frames keeps high speeds fluid. Afterwards the audio
        // track that was playing is restored.
        if (s) {
            char *aid = mpv_get_property_string(t->mpv, "aid");
            snprintf(t->shuttle_aid, sizeof(t->shuttle_aid), "%s", aid ? aid : "auto");
            mpv_free(aid);
        }
        mpv_cmd(t, "set", "aid", s ? "no" : t->shuttle_aid);
        mpv_cmd(t, "set", "framedrop", s ? "decoder+vo" : "vo");
    }
    snprintf(speed, sizeof(speed), "%g", s ? kShuttle[abs(s) - 1] : 1.0);
    mpv_cmd(t, "set", "speed", speed);
    mpv_cmd(t, "set", "pause", "no");
    t->shuttle = s;
    draw_bar(t);
}

// dir = +1 fast forward, -1 fast reverse. Pressing again in the same direction
// goes faster; switching direction starts at the slowest speed.
static void shuttle_step(Tile *t, int dir)
{
    int n = (int)ARRAYSIZE(kShuttle);
    int s = t->shuttle * dir > 0 ? t->shuttle + dir : dir;
    set_shuttle(t, abs(s) > n ? dir * n : s);
}

static void play_pause(Tile *t)
{
    if (t->shuttle)
        set_shuttle(t, 0);
    else
        mpv_cmd(t, "cycle", "pause", NULL);
}

// Shift+Space: pause all videos, or play all if the active one is paused.
static void play_pause_all(void)
{
    Tile *v[MAX_TILES];
    int n = live_tiles(v);
    BOOL pause = g_audio_tile && !g_audio_tile->paused && !g_audio_tile->shuttle;
    for (int i = 0; i < n; i++) {
        set_shuttle(v[i], 0);
        mpv_cmd(v[i], "set", "pause", pause ? "yes" : "no");
    }
}

static void show_osd(Tile *t, int kind)
{
    if (!t)
        return;
    if (g_osd_tile != t) {
        Tile *old = g_osd_tile;
        g_osd_tile = t;
        draw_osd(old);
    }
    g_osd = kind;
    SetTimer(g_hwnd, OSD_TIMER, OSD_MS, NULL);
    draw_osd(t);
}

static void change_volume(int delta)
{
    char v[16];
    Tile *all[MAX_TILES];
    int n = live_tiles(all);
    g_volume = g_volume + delta < 0 ? 0 : g_volume + delta > 100 ? 100 : g_volume + delta;
    g_muted = FALSE;
    snprintf(v, sizeof(v), "%g", g_volume);
    for (int i = 0; i < n; i++)
        mpv_cmd(all[i], "set", "volume", v);
    apply_audio();
    show_osd(g_audio_tile, OSD_VOLUME);
}

static void toggle_mute(void)
{
    g_muted = !g_muted;
    apply_audio();
    show_osd(g_audio_tile, OSD_VOLUME);
}

static void jump(Tile *t, int dir)
{
    char s[16];
    snprintf(s, sizeof(s), "%d", dir * g_jump);
    mpv_cmd(t, "seek", s, "relative+exact");
}

// Repeated jumps in the same direction add up on screen.
static void show_jump(Tile *t, int dir)
{
    if (g_osd != OSD_SEEK || g_osd_tile != t || (g_seek_total > 0) != (dir > 0))
        g_seek_total = 0;
    g_seek_total += dir * g_jump;
    show_osd(t, OSD_SEEK);
}

static void load_settings(void)
{
    DWORD v, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"JumpSeconds", RRF_RT_REG_DWORD, NULL, &v, &n)
            == ERROR_SUCCESS && v >= 1 && v <= 3600)
        g_jump = (int)v;
}

static void set_jump(int seconds)
{
    DWORD v = (DWORD)seconds;
    g_jump = seconds;
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"JumpSeconds", REG_DWORD, &v, sizeof(v));
}

// ---------------------------------------------------------------------------
// Control bar, drawn by mpv as an ASS overlay in tile pixel coordinates

typedef struct {
    double s; // DPI scale
    int w, h, top, x0, x1, ty, by, cell;
    int bx[3];       // button centres: rewind, play, fast forward
    int xx, xy, xr;  // close (X) button in the top-right corner: centre, radius
} Layout;

#define S(v) ((int)lround((v) * L->s))

static void get_layout(Tile *t, Layout *L)
{
    L->s = GetDpiForWindow(g_hwnd) / 96.0;
    L->w = tile_w(t);
    L->h = tile_h(t);
    L->top = L->h - S(64);
    L->x0 = S(16);
    L->x1 = L->w - S(16);
    L->ty = L->top + S(16);
    L->by = L->top + S(42);
    L->cell = S(36);
    L->bx[0] = L->w / 2 - S(48);
    L->bx[1] = L->w / 2;
    L->bx[2] = L->w / 2 + S(48);
    L->xr = S(14);
    L->xx = L->w - S(24);
    L->xy = S(24);
}

// The X that removes a video is only offered when there are several.
static BOOL can_close(void)
{
    Tile *v[MAX_TILES];
    return live_tiles(v) > 1;
}

static int hit_test(Tile *t, int x, int y)
{
    Layout l, *L = &l;
    if (!g_bar_visible || t != g_bar_tile)
        return HIT_NONE;
    get_layout(t, L);
    int dx = x - L->xx, dy = y - L->xy, r = L->xr + S(2);
    if (can_close() && dx * dx + dy * dy <= r * r)
        return HIT_CLOSE;
    if (y < L->top || y >= L->h || x < 0 || x >= L->w)
        return HIT_NONE;
    if (y < L->ty + S(10) && x >= L->x0 - S(8) && x <= L->x1 + S(8))
        return HIT_TRACK;
    for (int i = 0; i < 3; i++)
        if (abs(x - L->bx[i]) <= L->cell / 2 && abs(y - L->by) <= L->cell / 2)
            return HIT_REW + i;
    return HIT_BAR;
}

typedef struct {
    char buf[ASS_SIZE];
    int len;
} Ass;

static void ass_printf(Ass *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(a->buf + a->len, sizeof(a->buf) - a->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        a->len = min(a->len + n, (int)sizeof(a->buf) - 1);
}

// Starts a new event: a filled vector drawing in colour bgr, alpha 0 = opaque.
static void ass_shape(Ass *a, unsigned bgr, int alpha)
{
    ass_printf(a, "%s{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H%06X&\\1a&H%02X&\\p1}",
               a->len ? "\n" : "", bgr, alpha);
}

static void ass_rect(Ass *a, int x0, int y0, int x1, int y1)
{
    ass_printf(a, "m %d %d l %d %d l %d %d l %d %d ", x0, y0, x1, y0, x1, y1, x0, y1);
}

static void ass_tri(Ass *a, int x0, int y0, int x1, int y1, int x2, int y2)
{
    ass_printf(a, "m %d %d l %d %d l %d %d ", x0, y0, x1, y1, x2, y2);
}

static void ass_quad(Ass *a, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3)
{
    ass_printf(a, "m %d %d l %d %d l %d %d l %d %d ", x0, y0, x1, y1, x2, y2, x3, y3);
}

static void ass_circle(Ass *a, int cx, int cy, int r)
{
    int k = (int)lround(r * 0.5523);
    ass_printf(a, "m %d %d b %d %d %d %d %d %d b %d %d %d %d %d %d "
                  "b %d %d %d %d %d %d b %d %d %d %d %d %d ",
               cx, cy - r,
               cx + k, cy - r, cx + r, cy - k, cx + r, cy,
               cx + r, cy + k, cx + k, cy + r, cx, cy + r,
               cx - k, cy + r, cx - r, cy + k, cx - r, cy,
               cx - r, cy - k, cx - k, cy - r, cx, cy - r);
}

static void fmt_time(char *buf, size_t n, double t)
{
    int s = t > 0 ? (int)t : 0;
    if (s >= 3600)
        snprintf(buf, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

// Sends the overlay to the tile's mpv (or removes it when empty), unless it is
// unchanged since the last call: no need to make mpv re-render it.
static void send_overlay(Tile *t, const char *id, const Ass *a, char *last)
{
    if (strcmp(a->buf, last) == 0)
        return;
    memcpy(last, a->buf, a->len + 1);
    if (a->len == 0) {
        const char *cmd[] = { "osd-overlay", id, "none", "", NULL };
        mpv_command_async(t->mpv, 0, cmd);
    } else {
        char rx[16], ry[16];
        snprintf(rx, sizeof(rx), "%d", tile_w(t));
        snprintf(ry, sizeof(ry), "%d", tile_h(t));
        const char *cmd[] = { "osd-overlay", id, "ass-events", a->buf, rx, ry, NULL };
        mpv_command_async(t->mpv, 0, cmd);
    }
}

// While the bar is up, raise subtitles above it.
static void update_sub_margin(Tile *t)
{
    Layout l, *L = &l;
    char v[16];
    get_layout(t, L);
    int m = SUB_MARGIN;
    if (g_bar_visible && t == g_bar_tile && L->h > 0)
        m += (int)lround((L->h - L->top) * 720.0 / L->h);
    if (m == t->sub_margin_sent)
        return;
    t->sub_margin_sent = m;
    snprintf(v, sizeof(v), "%d", m);
    mpv_cmd(t, "set", "sub-margin-y", v);
}

static void draw_bar(Tile *t)
{
    static Ass a;
    if (!t || t->closing)
        return;
    a.len = 0;
    a.buf[0] = 0;
    update_sub_margin(t);

    if (g_bar_visible && t == g_bar_tile) {
        Layout l, *L = &l;
        get_layout(t, L);
        double frac = g_scrubbing && t == g_capture_tile ? g_scrub_frac
                    : t->duration > 0 ? t->time / t->duration : 0;
        frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        int px = L->x0 + (int)lround((L->x1 - L->x0) * frac);

        ass_shape(&a, 0x000000, 0x60);
        ass_rect(&a, 0, L->top, L->w, L->h);

        // Progress track with a knob.
        ass_shape(&a, 0xFFFFFF, 0xB0);
        ass_rect(&a, L->x0, L->ty - S(2), L->x1, L->ty + S(2));
        ass_shape(&a, ACCENT, 0);
        ass_rect(&a, L->x0, L->ty - S(2), px, L->ty + S(2));
        ass_shape(&a, 0xFFFFFF, 0);
        ass_circle(&a, px, L->ty, g_hover == HIT_TRACK || g_scrubbing ? S(7) : S(5));

        // Buttons: rewind, play/pause, fast forward.
        for (int i = 0; i < 3; i++) {
            int cx = L->bx[i], cy = L->by;
            if (g_hover == HIT_REW + i) {
                ass_shape(&a, 0xFFFFFF, 0xC8);
                ass_circle(&a, cx, cy, L->cell / 2);
            }
            BOOL active = (i == 0 && t->shuttle < 0) || (i == 2 && t->shuttle > 0);
            ass_shape(&a, active ? ACCENT : 0xFFFFFF, 0);
            if (i == 0) {
                ass_tri(&a, cx + S(1), cy - S(7), cx + S(1), cy + S(7), cx - S(8), cy);
                ass_tri(&a, cx + S(10), cy - S(7), cx + S(10), cy + S(7), cx + S(1), cy);
            } else if (i == 2) {
                ass_tri(&a, cx - S(10), cy - S(7), cx - S(10), cy + S(7), cx - S(1), cy);
                ass_tri(&a, cx - S(1), cy - S(7), cx - S(1), cy + S(7), cx + S(8), cy);
            } else if (t->paused || t->shuttle) {
                ass_tri(&a, cx - S(6), cy - S(9), cx - S(6), cy + S(9), cx + S(9), cy);
            } else {
                ass_rect(&a, cx - S(7), cy - S(8), cx - S(2), cy + S(8));
                ass_rect(&a, cx + S(2), cy - S(8), cx + S(7), cy + S(8));
            }
        }

        // X in the top-right corner removes this video (red while hovered).
        if (can_close()) {
            int cx = L->xx, cy = L->xy, d = S(5), k = max(1, S(1));
            ass_shape(&a, g_hover == HIT_CLOSE ? 0x2828D8 : 0x000000, g_hover == HIT_CLOSE ? 0x00 : 0x60);
            ass_circle(&a, cx, cy, L->xr);
            ass_shape(&a, 0xFFFFFF, 0); // the two strokes as separate shapes so they don't cancel out
            ass_quad(&a, cx - d + k, cy - d - k, cx + d + k, cy + d - k, cx + d - k, cy + d + k, cx - d - k, cy - d + k);
            ass_shape(&a, 0xFFFFFF, 0);
            ass_quad(&a, cx - d - k, cy + d - k, cx + d - k, cy - d - k, cx + d + k, cy - d + k, cx - d + k, cy + d + k);
        }

        // The time is left out when the tile is too narrow for it next to the buttons.
        if (L->bx[0] - L->cell / 2 - L->x0 > S(90)) {
            char tm[32], d[32];
            fmt_time(tm, sizeof(tm), g_scrubbing && t == g_capture_tile ? frac * t->duration : t->time);
            fmt_time(d, sizeof(d), t->duration);
            ass_printf(&a, "\n{\\an4\\pos(%d,%d)\\fs%d\\bord0\\shad0\\1c&HFFFFFF&\\1a&H00&}%s / %s",
                       L->x0, L->by, S(15), tm, d);
        }
        if (t->shuttle)
            ass_printf(&a, "\n{\\an6\\pos(%d,%d)\\fs%d\\b1\\bord0\\shad0\\1c&H%06X&\\1a&H00&}%s %g\xC3\x97",
                       L->x1, L->by, S(15), ACCENT, t->shuttle < 0 ? "REW" : "FF",
                       kShuttle[abs(t->shuttle) - 1]);
    }
    send_overlay(t, BAR_OVERLAY, &a, t->bar_last);
}

// Message box at the top centre: volume with a level bar, the jump distance, or text.
static void draw_osd(Tile *t)
{
    static Ass a;
    if (!t || t->closing)
        return;
    a.len = 0;
    a.buf[0] = 0;

    if (g_osd != OSD_NONE && t == g_osd_tile) {
        Layout l, *L = &l;
        char text[sizeof(g_osd_text)];
        get_layout(t, L);
        if (g_osd == OSD_VOLUME && g_muted)
            snprintf(text, sizeof(text), "Muted");
        else if (g_osd == OSD_VOLUME)
            snprintf(text, sizeof(text), "Volume %d%%", (int)lround(g_volume));
        else if (g_osd == OSD_SEEK)
            snprintf(text, sizeof(text), "%s%d s", g_seek_total < 0 ? "\xE2\x88\x92" : "+",
                     abs(g_seek_total));
        else
            snprintf(text, sizeof(text), "%s", g_osd_text);

        // Box wide enough for the text (rough average glyph width), within the tile.
        int chars = 0;
        for (const char *p = text; *p; p++)
            chars += (*p & 0xC0) != 0x80; // count UTF-8 characters, not bytes
        int bw = max(S(200), S(40) + S(22) * chars * 11 / 20);
        bw = min(bw, L->w - S(16));
        int bh = g_osd == OSD_VOLUME ? S(64) : S(44);
        int x0 = (L->w - bw) / 2, y0 = S(32), ty = y0 + bh / 2;

        ass_shape(&a, 0x000000, 0x60);
        ass_rect(&a, x0, y0, x0 + bw, y0 + bh);
        if (g_osd == OSD_VOLUME) {
            int bx0 = x0 + S(20), bx1 = x0 + bw - S(20), by = y0 + S(46);
            ass_shape(&a, 0xFFFFFF, 0xB0);
            ass_rect(&a, bx0, by - S(2), bx1, by + S(2));
            ass_shape(&a, g_muted ? 0x808080 : ACCENT, 0);
            ass_rect(&a, bx0, by - S(2), bx0 + (int)lround((bx1 - bx0) * g_volume / 100), by + S(2));
            ty = y0 + S(24);
        }
        // Escape "{" so track titles can't inject ASS override tags.
        ass_printf(&a, "\n{\\an5\\pos(%d,%d)\\fs%d\\bord0\\shad0\\1c&HFFFFFF&\\1a&H00&\\q2}",
                   L->w / 2, ty, S(22));
        for (const char *p = text; *p; p++)
            ass_printf(&a, *p == '{' ? "\\{" : "%c", *p);
    }
    send_overlay(t, OSD_OVERLAY, &a, t->osd_last);
}

// Shows the bar on tile t (moving it from another tile if needed).
static void show_bar(Tile *t)
{
    if (!t)
        return;
    if (g_bar_tile != t) {
        Tile *old = g_bar_tile;
        g_bar_tile = t;
        draw_bar(old);
    }
    if (!g_bar_visible) {
        g_bar_visible = TRUE;
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
    }
    SetTimer(g_hwnd, BAR_TIMER, BAR_HIDE_MS, NULL);
    draw_bar(t);
}

static void on_bar_timer(void)
{
    // Stay up while the mouse is on the bar, while scrubbing and while shuttling.
    if (g_hover != HIT_NONE || g_scrubbing || (g_bar_tile && g_bar_tile->shuttle))
        return;
    KillTimer(g_hwnd, BAR_TIMER);
    g_bar_visible = FALSE;
    draw_bar(g_bar_tile);
    if (g_fullscreen && g_last_mouse.x >= 0)
        SetCursor(NULL);
}

static void scrub_to(Tile *t, int x)
{
    Layout l, *L = &l;
    char pct[32];
    get_layout(t, L);
    double f = (double)(x - L->x0) / (L->x1 - L->x0);
    g_scrub_frac = f < 0 ? 0 : f > 1 ? 1 : f;
    snprintf(pct, sizeof(pct), "%.4f", g_scrub_frac * 100);
    mpv_cmd(t, "seek", pct, "absolute-percent+exact");
    draw_bar(t);
}

// ---------------------------------------------------------------------------
// Window and grid

// Width/height of a tile's video; 16:9 until the video is loaded.
static double tile_aspect(Tile *t)
{
    return t->vw > 0 && t->vh > 0 ? t->vw / t->vh : 16.0 / 9;
}

// Puts the n tiles in order over `rows` rows (row k gets tiles k*n/rows up to
// (k+1)*n/rows). Each row is as tall as its videos can be while filling the window
// width, each tile exactly as wide as its video needs, and everything is scaled down
// together when the rows don't fit the height. Returns the total video area and,
// when out is not NULL, the centred tile rectangles.
static double justify(Tile **v, int n, int rows, int W, int H, int gap, RECT *out)
{
    double rh[MAX_TILES], total = 0, area = 0;
    for (int k = 0; k < rows; k++) {
        int a = k * n / rows, b = (k + 1) * n / rows;
        double sum = 0;
        for (int i = a; i < b; i++)
            sum += tile_aspect(v[i]);
        rh[k] = max(1.0, (double)W - gap * (b - a - 1)) / sum;
        total += rh[k];
        area += rh[k] * rh[k] * sum;
    }
    double s = min(1.0, max(1.0, (double)H - gap * (rows - 1)) / total);
    if (out) {
        double y = (H - (total * s + gap * (rows - 1))) / 2;
        for (int k = 0; k < rows; k++) {
            int a = k * n / rows, b = (k + 1) * n / rows;
            double h = rh[k] * s, width = gap * (b - a - 1);
            for (int i = a; i < b; i++)
                width += tile_aspect(v[i]) * h;
            double x = (W - width) / 2;
            for (int i = a; i < b; i++) {
                double w = tile_aspect(v[i]) * h;
                SetRect(&out[i], (int)lround(x), (int)lround(y), (int)lround(x + w), (int)lround(y + h));
                x += w + gap;
            }
            y += h + gap;
        }
    }
    return area * s * s;
}

// Arranges the tiles so the videos are as large as possible: tried with every
// number of rows, tiles shaped like their videos (vertical videos side by side,
// wide ones stacked). A single video, or the one shown alone, fills the window.
static void layout_tiles(void)
{
    Tile *v[MAX_TILES];
    RECT rc, rects[MAX_TILES];
    int n = live_tiles(v);
    if (!n || g_closing)
        return;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    if (n > 1 && !g_solo) {
        int gap = max(1, (int)lround(2 * GetDpiForWindow(g_hwnd) / 96.0)), rows = 1;
        double best = -1;
        for (int r = 1; r <= n; r++) {
            double area = justify(v, n, r, W, H, gap, NULL);
            if (area > best + 1e-6) {
                best = area;
                rows = r;
            }
        }
        justify(v, n, rows, W, H, gap, rects);
    }

    for (int i = 0; i < n; i++) {
        Tile *t = v[i];
        RECT r = rects[i];
        if (n == 1 || g_solo) {
            if (g_solo && t != g_solo) {
                ShowWindow(t->hwnd, SW_HIDE);
                SetRectEmpty(&t->rc);
                continue;
            }
            SetRect(&r, 0, 0, W, H);
        }
        if (!EqualRect(&r, &t->rc)) {
            t->rc = r;
            MoveWindow(t->hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
            if (t->zoom > 0)
                apply_view(t); // re-clamp pan for the new size
        }
        ShowWindow(t->hwnd, SW_SHOWNA);
        draw_bar(t);
        draw_osd(t);
    }
}

static void toggle_solo(Tile *t)
{
    g_solo = g_solo ? NULL : t;
    layout_tiles();
    update_title();
}

static void toggle_fullscreen(void)
{
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (!GetWindowPlacement(g_hwnd, &g_wp) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongW(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
}

// Remember size, position and maximized state for the next start. In fullscreen
// the state from before going fullscreen is saved.
static void save_placement(void)
{
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (g_fullscreen)
        wp = g_wp;
    else if (!GetWindowPlacement(g_hwnd, &wp))
        return;
    if (wp.showCmd != SW_SHOWMAXIMIZED) // e.g. minimized: reopen as it was before
        wp.showCmd = wp.flags & WPF_RESTORETOMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"WindowPlacement", REG_BINARY, &wp, sizeof(wp));
}

// Applies (and shows) the saved window state. Fails when nothing is saved yet or
// the screen it was on is no longer connected.
static BOOL restore_placement(void)
{
    WINDOWPLACEMENT wp;
    DWORD n = sizeof(wp);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"WindowPlacement", RRF_RT_REG_BINARY, NULL,
                     &wp, &n) != ERROR_SUCCESS || n != sizeof(wp) || wp.length != sizeof(wp) ||
        !MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL))
        return FALSE;
    if (wp.showCmd != SW_SHOWMAXIMIZED)
        wp.showCmd = SW_SHOWNORMAL;
    wp.flags = 0;
    return SetWindowPlacement(g_hwnd, &wp);
}

// With a single video, resize the window once per file to the video's size (at
// most 80% of the screen), unless the window size was restored from the last session.
static void size_window_to_video(Tile *t)
{
    Tile *v[MAX_TILES];
    MONITORINFO mi = { sizeof(mi) };
    if (g_restored_placement || g_fullscreen || IsZoomed(g_hwnd) || live_tiles(v) != 1 ||
        !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return;
    RECT wa = mi.rcWork;
    double aw = wa.right - wa.left, ah = wa.bottom - wa.top;
    double s = min(1.0, min(aw * 0.8 / t->vw, ah * 0.8 / t->vh));
    RECT r = { 0, 0, (LONG)(t->vw * s), (LONG)(t->vh * s) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(g_hwnd, GWL_STYLE), FALSE,
                             GetWindowLongW(g_hwnd, GWL_EXSTYLE), GetDpiForWindow(g_hwnd));
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(g_hwnd, NULL, wa.left + ((int)aw - w) / 2, wa.top + ((int)ah - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// mpv renders into its own child window inside the tile. Disabling it makes Windows
// route its mouse input to the tile, which passes it on to the main window.
static BOOL CALLBACK disable_child(HWND child, LPARAM lp)
{
    (void)lp;
    if (IsWindowEnabled(child))
        EnableWindow(child, FALSE);
    return TRUE;
}

static void on_video_reconfig(Tile *t)
{
    int64_t dw = 0, dh = 0, rot = 0;
    EnumChildWindows(t->hwnd, disable_child, 0);
    if (mpv_get_property(t->mpv, "video-out-params/dw", MPV_FORMAT_INT64, &dw) < 0 ||
        mpv_get_property(t->mpv, "video-out-params/dh", MPV_FORMAT_INT64, &dh) < 0 ||
        dw <= 0 || dh <= 0)
        return;
    mpv_get_property(t->mpv, "video-out-params/rotate", MPV_FORMAT_INT64, &rot);
    double aspect = tile_aspect(t);
    t->vw = (double)(rot % 180 ? dh : dw);
    t->vh = (double)(rot % 180 ? dw : dh);
    if (!t->sized) {
        t->sized = TRUE;
        size_window_to_video(t);
    }
    if (fabs(tile_aspect(t) - aspect) > 1e-3)
        layout_tiles(); // the grid is shaped by the videos
    apply_view(t);
}

static void on_property_change(Tile *t, mpv_event_property *p)
{
    double d = p->format == MPV_FORMAT_DOUBLE ? *(double *)p->data : 0;
    int flag = p->format == MPV_FORMAT_FLAG ? *(int *)p->data : 0;
    if (!strcmp(p->name, "time-pos"))
        t->time = d;
    else if (!strcmp(p->name, "duration"))
        t->duration = d;
    else if (!strcmp(p->name, "pause"))
        t->paused = flag;
    else if (!strcmp(p->name, "eof-reached") && flag && t->shuttle < 0)
        set_shuttle(t, 0); // rewound to the start: play forward again
    draw_bar(t);
}

// Drops every reference to a tile that is going away.
static void forget_tile(Tile *t)
{
    Tile *v[MAX_TILES];
    if (g_hover_tile == t)
        g_hover_tile = NULL;
    if (g_bar_tile == t)
        g_bar_tile = NULL;
    if (g_osd_tile == t)
        g_osd_tile = NULL;
    if (g_solo == t)
        g_solo = NULL;
    if (g_capture_tile == t) {
        g_capture_tile = NULL;
        ReleaseCapture();
    }
    if (g_audio_tile == t)
        g_audio_tile = live_tiles(v) ? v[0] : NULL;
}

static void destroy_tile(Tile *t)
{
    t->closing = TRUE;
    forget_tile(t);
    for (int i = 0; i < g_ntiles; i++)
        if (g_tiles[i] == t) {
            memmove(&g_tiles[i], &g_tiles[i + 1], (g_ntiles - i - 1) * sizeof(*g_tiles));
            g_ntiles--;
            break;
        }
    mpv_terminate_destroy(t->mpv);
    DestroyWindow(t->hwnd);
    free(t);
    if (g_closing && g_ntiles == 0)
        DestroyWindow(g_hwnd);
}

static void handle_mpv_events(Tile *t)
{
    for (;;) {
        mpv_event *e = mpv_wait_event(t->mpv, 0);
        switch (e->event_id) {
        case MPV_EVENT_NONE:
            return;
        case MPV_EVENT_VIDEO_RECONFIG:
            on_video_reconfig(t);
            break;
        case MPV_EVENT_PROPERTY_CHANGE:
            on_property_change(t, e->data);
            break;
        case MPV_EVENT_SHUTDOWN:
            destroy_tile(t);
            return;
        default:
            break;
        }
    }
}

static void on_mpv_wakeup(void *ctx)
{
    PostMessageW(g_hwnd, WM_MPV_WAKEUP, 0, (LPARAM)ctx);
}

// mpv wants UTF-8 strings. Caller frees.
static char *to_utf8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *utf8 = n > 0 ? malloc(n) : NULL;
    if (utf8)
        WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, n, NULL, NULL);
    return utf8;
}

static Tile *create_tile(void)
{
    char volume[16];
    if (g_ntiles >= MAX_TILES)
        return NULL;
    Tile *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->mute_sent = t->sub_margin_sent = -1;
    snprintf(t->shuttle_aid, sizeof(t->shuttle_aid), "auto");
    t->hwnd = CreateWindowExW(0, L"MinimalVideoViewerTile", NULL, WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 1, 1, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    t->mpv = mpv_create();
    if (!t->hwnd || !t->mpv) {
        if (!t->mpv)
            MessageBoxW(g_hwnd, L"Could not create mpv instance (is libmpv-2.dll next to the exe?)",
                        L"MinimalVideoViewer", MB_ICONERROR);
        if (t->hwnd)
            DestroyWindow(t->hwnd);
        free(t);
        return NULL;
    }
    int64_t wid = (int64_t)(intptr_t)t->hwnd;
    snprintf(volume, sizeof(volume), "%g", g_volume);
    mpv_set_option(t->mpv, "wid", MPV_FORMAT_INT64, &wid);
    mpv_set_option_string(t->mpv, "loop-file", "inf");
    mpv_set_option_string(t->mpv, "keep-open", "yes");     // fast reverse stops at the start
    mpv_set_option_string(t->mpv, "cache", "yes");         // packets in RAM: quick reverse switch, loops from memory
    mpv_set_option_string(t->mpv, "demuxer-max-bytes", "64MiB");      // per video, so several
    mpv_set_option_string(t->mpv, "demuxer-max-back-bytes", "32MiB"); // fit in memory
    mpv_set_option_string(t->mpv, "hwdec", "auto-safe");   // GPU decoding (D3D11VA etc.)
    mpv_set_option_string(t->mpv, "idle", "yes");          // stay open if a file fails to load
    mpv_set_option_string(t->mpv, "volume", volume);
    mpv_set_option_string(t->mpv, "mute", "yes");          // apply_audio() unmutes the active one
    mpv_set_option_string(t->mpv, "osd-level", "0");
    mpv_set_option_string(t->mpv, "osc", "no");
    mpv_set_option_string(t->mpv, "input-default-bindings", "no");
    mpv_set_option_string(t->mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(t->mpv, "input-cursor", "no");
    mpv_set_option_string(t->mpv, "cursor-autohide", "no");
    if (mpv_initialize(t->mpv) < 0) {
        MessageBoxW(g_hwnd, L"Could not initialize mpv.", L"MinimalVideoViewer", MB_ICONERROR);
        mpv_terminate_destroy(t->mpv);
        DestroyWindow(t->hwnd);
        free(t);
        return NULL;
    }
    t->mute_sent = 1;
    mpv_observe_property(t->mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(t->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(t->mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(t->mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    g_tiles[g_ntiles++] = t;
    mpv_set_wakeup_callback(t->mpv, on_mpv_wakeup, t);
    return t;
}

static void load_file(Tile *t, const wchar_t *path)
{
    char *utf8 = to_utf8(path);
    if (!utf8)
        return;
    set_shuttle(t, 0);
    // Track choices are mpv options that would carry over; let each file pick its own.
    mpv_cmd(t, "set", "aid", "auto");
    mpv_cmd(t, "set", "sid", "auto");
    mpv_cmd(t, "loadfile", utf8, NULL);
    free(utf8);

    const wchar_t *name = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            name = p + 1;
    swprintf(t->name, ARRAYSIZE(t->name), L"%s", name);
    t->sized = FALSE;
    reset_view(t);
    update_title();
    show_bar(t);
}

// Adds a video to the grid. The first one gets the sound.
static Tile *add_video(const wchar_t *path)
{
    Tile *t = create_tile();
    if (!t)
        return NULL;
    if (!g_audio_tile)
        g_audio_tile = t;
    g_solo = NULL;
    apply_audio();
    layout_tiles();
    load_file(t, path);
    return t;
}

static void close_tile(Tile *t)
{
    Tile *v[MAX_TILES];
    if (!t || live_tiles(v) <= 1)
        return;
    t->closing = TRUE;
    forget_tile(t);
    ShowWindow(t->hwnd, SW_HIDE);
    mpv_cmd(t, "quit", NULL, NULL); // freed on MPV_EVENT_SHUTDOWN
    layout_tiles();
    apply_audio();
    update_title();
}

static void add_subtitle(Tile *t, const wchar_t *path)
{
    char *utf8 = to_utf8(path);
    if (!utf8)
        return;
    mpv_cmd(t, "sub-add", utf8, "select");
    free(utf8);
}

static BOOL is_subtitle_file(const wchar_t *path)
{
    static const wchar_t *exts[] = { L".srt", L".ass", L".ssa", L".vtt", L".sub", L".idx", L".sup" };
    const wchar_t *dot = wcsrchr(path, L'.');
    for (int i = 0; dot && i < (int)ARRAYSIZE(exts); i++)
        if (!_wcsicmp(dot, exts[i]))
            return TRUE;
    return FALSE;
}

// File dialog result: one path, or with multi-select "folder\0name\0name\0\0".
static wchar_t g_picked[65536];

static BOOL pick_files(HWND owner, const wchar_t *filter, BOOL multi)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    g_picked[0] = 0;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = g_picked;
    ofn.nMaxFile = ARRAYSIZE(g_picked);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                (multi ? OFN_ALLOWMULTISELECT : 0);
    return GetOpenFileNameW(&ofn);
}

static void add_picked_videos(void)
{
    const wchar_t *dir = g_picked, *name = dir + wcslen(dir) + 1;
    if (!*name) {
        add_video(dir);
        return;
    }
    for (; *name; name += wcslen(name) + 1) {
        wchar_t path[2 * MAX_PATH + 2];
        size_t n = wcslen(dir);
        swprintf(path, ARRAYSIZE(path), L"%s%s%s", dir, n && dir[n - 1] == L'\\' ? L"" : L"\\", name);
        add_video(path);
    }
}

static void on_left_down(int x, int y)
{
    Tile *t = tile_at(x, y);
    if (!t)
        return;
    int lx = x - t->rc.left, ly = y - t->rc.top;
    switch (hit_test(t, lx, ly)) {
    case HIT_TRACK:
        g_scrubbing = TRUE;
        g_capture_tile = t;
        SetCapture(g_hwnd);
        scrub_to(t, lx);
        break;
    case HIT_REW:  shuttle_step(t, -1); break;
    case HIT_PLAY: play_pause(t); break;
    case HIT_FF:   shuttle_step(t, +1); break;
    case HIT_CLOSE:
        close_tile(t);
        g_closed_by_click = TRUE;
        break;
    case HIT_BAR:  break;
    default:
        g_dragging = TRUE;
        g_capture_tile = t;
        g_drag_last.x = x;
        g_drag_last.y = y;
        SetCapture(g_hwnd);
        break;
    }
}

static void on_mouse_move(int x, int y)
{
    Tile *t = g_capture_tile ? g_capture_tile : tile_at(x, y);
    if (x != g_last_mouse.x || y != g_last_mouse.y) { // ignore synthetic moves
        g_last_mouse.x = x;
        g_last_mouse.y = y;
        show_bar(t);
    }
    if (!g_capture_tile && t != g_hover_tile) {
        g_hover_tile = t;
        if (t && t != g_audio_tile) { // sound and keys follow the mouse
            g_audio_tile = t;
            apply_audio();
        }
    }
    if (t && g_scrubbing) {
        scrub_to(t, x - t->rc.left);
    } else if (t && g_dragging) {
        if (t->zoom > 0) {
            SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
            pan_by(t, x - g_drag_last.x, y - g_drag_last.y);
        }
        g_drag_last.x = x;
        g_drag_last.y = y;
    }
    g_hover = t ? hit_test(t, x - t->rc.left, y - t->rc.top) : HIT_NONE;
    draw_bar(g_bar_tile);
}

static void subtitle_dialog(HWND hwnd, Tile *t)
{
    if (pick_files(hwnd, kSubFilter, FALSE))
        add_subtitle(t, g_picked);
}

static char *track_prop(Tile *t, int64_t i, const char *field)
{
    char name[64];
    snprintf(name, sizeof(name), "track-list/%lld/%s", (long long)i, field);
    return mpv_get_property_string(t->mpv, name);
}

// Track descriptions from the last menu, by track id, for the on-screen message.
static char g_audio_names[MAX_TRACKS][128], g_sub_names[MAX_TRACKS][128];

// Adds one radio item per track of the given type ("audio" or "sub") plus "Off",
// labelled like "2: Commentary, eng, aac". Returns the number of tracks.
static int add_track_items(Tile *t, HMENU menu, const char *type, int base, char (*names)[128])
{
    int64_t count = 0;
    int n = 0, checked = 0;
    mpv_get_property(t->mpv, "track-list/count", MPV_FORMAT_INT64, &count);
    AppendMenuW(menu, MF_STRING, base, L"Off");
    for (int64_t i = 0; i < count; i++) {
        char *s = track_prop(t, i, "type");
        BOOL match = s && !strcmp(s, type);
        mpv_free(s);
        int64_t id = 0;
        char idname[64];
        snprintf(idname, sizeof(idname), "track-list/%lld/id", (long long)i);
        if (!match || mpv_get_property(t->mpv, idname, MPV_FORMAT_INT64, &id) < 0 ||
            id < 1 || id >= MAX_TRACKS)
            continue;

        char text[256] = "";
        static const char *fields[] = { "title", "lang", "codec" };
        for (int f = 0; f < 3; f++) {
            s = track_prop(t, i, fields[f]);
            if (s && *s)
                snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s", *text ? ", " : "", s);
            mpv_free(s);
        }
        s = track_prop(t, i, "external");
        if (s && !strcmp(s, "yes"))
            snprintf(text + strlen(text), sizeof(text) - strlen(text), "%sexternal", *text ? ", " : "");
        mpv_free(s);
        snprintf(names[id], sizeof(names[id]), "%d: %s", (int)id, text);

        // Menu text: "&" would become a mnemonic, so double it.
        wchar_t wtext[256], label[300];
        int k = 0;
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, ARRAYSIZE(wtext));
        k = swprintf(label, ARRAYSIZE(label), L"%d: ", (int)id);
        for (const wchar_t *p = wtext; *p && k < (int)ARRAYSIZE(label) - 2; p++) {
            if (*p == L'&')
                label[k++] = L'&';
            label[k++] = *p;
        }
        label[k] = 0;
        AppendMenuW(menu, MF_STRING, base + (int)id, label);

        s = track_prop(t, i, "selected");
        if (s && !strcmp(s, "yes"))
            checked = (int)id;
        mpv_free(s);
        n++;
    }
    // Audio is switched off while shuttling; show the track that will come back.
    if (!strcmp(type, "audio") && t->shuttle)
        checked = !strcmp(t->shuttle_aid, "no") ? 0 : atoi(t->shuttle_aid);
    CheckMenuRadioItem(menu, base, base + MAX_TRACKS - 1, base + checked, MF_BYCOMMAND);
    return n;
}

static void select_track(Tile *t, const char *prop, int id)
{
    char v[16];
    if (id)
        snprintf(v, sizeof(v), "%d", id);
    else
        snprintf(v, sizeof(v), "no");
    BOOL audio = !strcmp(prop, "aid");
    if (audio && t->shuttle)
        snprintf(t->shuttle_aid, sizeof(t->shuttle_aid), "%s", v); // applied when shuttling ends
    else
        mpv_cmd(t, "set", prop, v);

    if (!id)
        snprintf(g_osd_text, sizeof(g_osd_text), audio ? "Audio off" : "Subtitles off");
    else
        snprintf(g_osd_text, sizeof(g_osd_text), "%s: %s", audio ? "Audio" : "Subtitles",
                 audio ? g_audio_names[id] : g_sub_names[id]);
    show_osd(t, OSD_TEXT);
}

// Right-click menu at screen position (x, y) for tile t.
static void show_menu(HWND hwnd, int x, int y, Tile *t)
{
    Tile *v[MAX_TILES];
    int n = (int)ARRAYSIZE(kJumps), tiles = live_tiles(v);
    HMENU audio = CreatePopupMenu(), subs = CreatePopupMenu();
    HMENU jumps = CreatePopupMenu(), menu = CreatePopupMenu();

    int naudio = add_track_items(t, audio, "audio", ID_AUDIO, g_audio_names);
    add_track_items(t, subs, "sub", ID_SUB, g_sub_names);
    AppendMenuW(subs, MF_SEPARATOR, 0, NULL);
    AppendMenuW(subs, MF_STRING, ID_SUB_LOAD, L"Load subtitle file...");
    for (int i = 0; i < n; i++) {
        wchar_t label[32];
        swprintf(label, ARRAYSIZE(label), kJumps[i] == 1 ? L"%d second" : L"%d seconds", kJumps[i]);
        AppendMenuW(jumps, MF_STRING, ID_JUMP + i, label);
        if (kJumps[i] == g_jump)
            CheckMenuRadioItem(jumps, ID_JUMP, ID_JUMP + n - 1, ID_JUMP + i, MF_BYCOMMAND);
    }
    AppendMenuW(menu, MF_POPUP | (naudio ? 0 : MF_GRAYED), (UINT_PTR)audio, L"Audio");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)subs, L"Subtitles");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)jumps, L"Jump distance (Left/Right keys)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (tiles < MAX_TILES ? 0 : MF_GRAYED), ID_ADD, L"Add videos...\tA");
    AppendMenuW(menu, MF_STRING, ID_OPEN, tiles > 1 ? L"Replace this video...\tO" : L"Open file...\tO");
    if (tiles > 1) {
        AppendMenuW(menu, MF_STRING, ID_SOLO, g_solo ? L"Show all videos\tDouble-click"
                                                     : L"Show only this video\tDouble-click");
        AppendMenuW(menu, MF_STRING, ID_CLOSE_TILE, L"Close this video\tDel");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_fullscreen ? MF_CHECKED : 0), ID_FULLSCREEN, L"Fullscreen\tF");
    AppendMenuW(menu, MF_STRING | (t->zoom > 0 ? 0 : MF_GRAYED), ID_RESET_ZOOM, L"Reset zoom\tR");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit\tQ");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(menu); // also destroys the submenus
    if (!tile_exists(t) || t->closing)
        return;
    if (cmd >= ID_JUMP && cmd < ID_JUMP + n)
        set_jump(kJumps[cmd - ID_JUMP]);
    else if (cmd >= ID_AUDIO && cmd < ID_AUDIO + MAX_TRACKS)
        select_track(t, "aid", cmd - ID_AUDIO);
    else if (cmd >= ID_SUB && cmd < ID_SUB + MAX_TRACKS)
        select_track(t, "sid", cmd - ID_SUB);
    else if (cmd == ID_SUB_LOAD)
        subtitle_dialog(hwnd, t);
    else if (cmd == ID_ADD && pick_files(hwnd, kVideoFilter, TRUE))
        add_picked_videos();
    else if (cmd == ID_OPEN && pick_files(hwnd, kVideoFilter, FALSE) && tile_exists(t))
        load_file(t, g_picked);
    else if (cmd == ID_SOLO)
        toggle_solo(t);
    else if (cmd == ID_CLOSE_TILE)
        close_tile(t);
    else if (cmd == ID_FULLSCREEN)
        toggle_fullscreen();
    else if (cmd == ID_RESET_ZOOM)
        reset_view(t);
    else if (cmd == ID_QUIT)
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

// Keys act on the video under the mouse (the one with sound); with Shift, the
// playback keys act on all videos.
static void on_key(HWND hwnd, WPARAM key)
{
    Tile *t = g_audio_tile, *v[MAX_TILES];
    BOOL all = GetKeyState(VK_SHIFT) < 0;
    int n = all ? live_tiles(v) : 0;
    if (!all && t)
        v[n++] = t;
    if (!t)
        return;
    switch (key) {
    case VK_SPACE:
    case 'K':
        if (all)
            play_pause_all();
        else
            play_pause(t);
        break;
    case 'J':
    case 'L':
        for (int i = 0; i < n; i++)
            shuttle_step(v[i], key == 'L' ? +1 : -1);
        break;
    case VK_LEFT:
    case VK_RIGHT:
        for (int i = 0; i < n; i++)
            jump(v[i], key == VK_RIGHT ? +1 : -1);
        show_jump(t, key == VK_RIGHT ? +1 : -1);
        break;
    case 'R':
        for (int i = 0; i < n; i++)
            reset_view(v[i]);
        break;
    case VK_UP:     change_volume(5); break;
    case VK_DOWN:   change_volume(-5); break;
    case 'M':       toggle_mute(); break;
    case 'F':
    case VK_F11:    toggle_fullscreen(); break;
    case VK_ESCAPE:
        if (g_fullscreen)
            toggle_fullscreen();
        else if (g_solo)
            toggle_solo(NULL);
        break;
    case VK_DELETE: close_tile(t); break;
    case 'Q':       PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
    case 'O':
        if (pick_files(hwnd, kVideoFilter, FALSE) && tile_exists(t))
            load_file(t, g_picked);
        break;
    case 'A':
        if (pick_files(hwnd, kVideoFilter, TRUE))
            add_picked_videos();
        break;
    }
}

// Tiles pass their mouse input on to the main window, in its coordinates.
static LRESULT CALLBACK tile_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (msg == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        MapWindowPoints(hwnd, g_hwnd, &pt, 1);
        return SendMessageW(g_hwnd, msg, wp | FORWARDED_MOUSE, MAKELPARAM(pt.x, pt.y));
    }
    case WM_MOUSELEAVE:
        return SendMessageW(g_hwnd, msg, 0, 0);
    }
    return DefWindowProcW(hwnd, msg, wp, lp); // passes wheel, right-click menu and cursor up
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MPV_WAKEUP:
        if (tile_exists((Tile *)lp)) // it may have been freed since
            handle_mpv_events((Tile *)lp);
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        Tile *t = tile_at(pt.x, pt.y);
        if (t)
            zoom_at(t, pt.x - t->rc.left, pt.y - t->rc.top,
                    GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA * ZOOM_STEP);
        return 0;
    }

    case WM_LBUTTONDOWN:
        g_closed_by_click = FALSE;
        on_left_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (!(wp & FORWARDED_MOUSE)) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE: {
        // Moving between the main window and its tiles is not leaving.
        POINT pt;
        GetCursorPos(&pt);
        HWND w = WindowFromPoint(pt);
        if (w == hwnd || IsChild(hwnd, w))
            return 0;
        g_last_mouse.x = g_last_mouse.y = -1;
        g_hover_tile = NULL;
        g_hover = HIT_NONE;
        draw_bar(g_bar_tile);
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        g_dragging = FALSE;
        if (g_scrubbing) {
            g_scrubbing = FALSE;
            if (g_capture_tile) {
                g_capture_tile->time = g_scrub_frac * g_capture_tile->duration; // until mpv reports it
                draw_bar(g_capture_tile);
            }
        }
        g_capture_tile = NULL;
        return 0;
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        Tile *t = tile_at(x, y), *v[MAX_TILES];
        if (!t || g_closed_by_click) // the first click removed a video; another may be here now
            return 0;
        if (hit_test(t, x - t->rc.left, y - t->rc.top) != HIT_NONE)
            on_left_down(x, y); // fast clicks on the bar
        else if (live_tiles(v) > 1)
            toggle_solo(t);
        else
            toggle_fullscreen();
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, cpt;
        if (lp == -1) // opened from the keyboard
            GetCursorPos(&pt);
        cpt = pt;
        ScreenToClient(hwnd, &cpt);
        Tile *t = tile_at(cpt.x, cpt.y);
        if (!t)
            t = g_audio_tile;
        if (t)
            show_menu(hwnd, pt.x, pt.y, t);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_fullscreen && !g_bar_visible) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_TIMER:
        if (wp == BAR_TIMER) {
            on_bar_timer();
        } else if (wp == OSD_TIMER) {
            KillTimer(hwnd, OSD_TIMER);
            g_osd = OSD_NONE;
            draw_osd(g_osd_tile);
        }
        return 0;

    case WM_KEYDOWN:
        on_key(hwnd, wp);
        return 0;

    case WM_DROPFILES: {
        // A video dropped on a video replaces it; further videos, videos dropped on
        // the black space around the grid, and videos dropped with Ctrl held are added.
        // Subtitles go to the video they are dropped on.
        static wchar_t path[32768];
        POINT pt;
        DragQueryPoint((HDROP)wp, &pt);
        Tile *target = tile_at(pt.x, pt.y), *replace = target;
        if (GetKeyState(VK_CONTROL) < 0)
            replace = NULL;
        UINT count = DragQueryFileW((HDROP)wp, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < count; i++) {
            if (!DragQueryFileW((HDROP)wp, i, path, ARRAYSIZE(path)))
                continue;
            if (is_subtitle_file(path)) {
                Tile *t = target ? target : g_audio_tile;
                if (t)
                    add_subtitle(t, path);
            } else if (replace) {
                load_file(replace, path);
                replace = NULL;
            } else {
                add_video(path);
            }
        }
        DragFinish((HDROP)wp);
        SetForegroundWindow(hwnd);
        return 0;
    }

    case WM_SIZE:
        layout_tiles();
        return 0;

    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        // Let every mpv shut down (and destroy its child window) while we keep pumping
        // messages; the window is destroyed when the last one is gone.
        if (!g_closing) {
            g_closing = TRUE;
            save_placement();
            ShowWindow(hwnd, SW_HIDE);
            for (int i = 0; i < g_ntiles; i++) {
                Tile *t = g_tiles[i];
                if (!t->closing) {
                    t->closing = TRUE;
                    forget_tile(t);
                    mpv_cmd(t, "quit", NULL, NULL);
                }
            }
        }
        if (g_ntiles == 0)
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    load_settings();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2 && !pick_files(NULL, kVideoFilter, TRUE))
        return 0;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpfnWndProc = tile_proc;
    wc.lpszClassName = L"MinimalVideoViewerTile";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"MinimalVideoViewer";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"MinimalVideoViewer",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 540, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;

    g_restored_placement = restore_placement();
    if (!g_restored_placement)
        ShowWindow(g_hwnd, show);
    if (argc >= 2)
        for (int i = 1; i < argc; i++)
            add_video(argv[i]);
    else
        add_picked_videos();
    LocalFree(argv);
    if (g_ntiles == 0)
        return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
