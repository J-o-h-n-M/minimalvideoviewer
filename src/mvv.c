// MinimalVideoViewer: a tiny looping video player built on libmpv.
//
//   Mouse wheel        zoom in/out at the cursor (zoom persists across loops)
//   Left drag          pan while zoomed
//   Double-click / F   toggle fullscreen
//   Right-click        menu (audio track, subtitles, jump distance, open, fullscreen,
//                      reset zoom, quit). Dropping a subtitle file loads it.
//   R                  reset zoom
//   Space / K          play/pause (also leaves fast forward/reverse)
//   J / L              VCR-style fast reverse / fast forward (press again: faster)
//   Left/Right         jump back / forward (distance set in the menu, default 5 s)
//   Up/Down, M         volume, mute
//   O                  open file (drag & drop also works)
//   Esc                leave fullscreen,  Q  quit
//
// Moving the mouse shows a control bar (progress, rewind, play/pause, fast forward)
// that hides again after 5 seconds without mouse movement. Volume changes and jumps
// are briefly shown at the top of the window.

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

#define WM_MPV_WAKEUP (WM_APP + 1)
#define ZOOM_STEP 0.125 // log2 units per wheel notch (~9% per notch)
#define ZOOM_MAX  6.0   // 64x

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

// Menu ids. Audio/subtitle items are ID_AUDIO/ID_SUB + mpv track id (+ 0 = off).
enum { ID_OPEN = 1, ID_FULLSCREEN, ID_RESET_ZOOM, ID_QUIT, ID_SUB_LOAD,
       ID_JUMP = 100, ID_AUDIO = 200, ID_SUB = 400 };

static const wchar_t kVideoFilter[] = L"Video files\0*.mp4;*.mkv;*.webm;*.mov;*.avi;*.wmv;*.m4v;*.flv;*.ts;*.mts;*.m2ts;*.mpg;*.mpeg;*.gif\0All files\0*.*\0";
static const wchar_t kSubFilter[] = L"Subtitles\0*.srt;*.ass;*.ssa;*.vtt;*.sub;*.idx;*.sup\0All files\0*.*\0";

static HWND g_hwnd;
static mpv_handle *g_mpv;
static BOOL g_closing;

// View state, mirrored into mpv's video-zoom / video-pan-x / video-pan-y.
// These are global mpv options, so they survive every loop of the file.
static double g_zoom, g_panx, g_pany;
static double g_vw, g_vh; // displayed video size in pixels (after rotation)
static BOOL g_sized_to_video;
static BOOL g_restored_placement; // window size/state comes from the last session

static BOOL g_dragging;
static POINT g_drag_last;

static BOOL g_fullscreen;
static WINDOWPLACEMENT g_wp = { sizeof(WINDOWPLACEMENT) };

// Control bar state.
enum { HIT_NONE, HIT_BAR, HIT_TRACK, HIT_REW, HIT_PLAY, HIT_FF };
static BOOL g_bar_visible;
static int g_hover = HIT_NONE;
static BOOL g_scrubbing;
static double g_scrub_frac;
static BOOL g_tracking_leave;
static POINT g_last_mouse = { -1, -1 };
static double g_time, g_duration;
static BOOL g_paused;

// Shuttle (VCR fast forward/reverse): 0 = normal play, +n = forward at
// kShuttle[n-1]x, -n = reverse at kShuttle[n-1]x.
static const double kShuttle[] = { 2, 4, 8, 16 };
static int g_shuttle;
static char g_shuttle_aid[32] = "auto"; // audio track to restore after shuttling

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

static void draw_bar(void);
static void draw_osd(void);

static void mpv_cmd(const char *a, const char *b, const char *c)
{
    const char *cmd[] = { a, b, c, NULL };
    mpv_command_async(g_mpv, 0, cmd);
}

// ---------------------------------------------------------------------------
// Zoom and pan

// Size of the video on screen at the current zoom, plus the window client size.
static BOOL view_metrics(double *w, double *h, double *ww, double *wh)
{
    RECT rc;
    if (g_vw <= 0 || g_vh <= 0)
        return FALSE;
    GetClientRect(g_hwnd, &rc);
    *ww = rc.right;
    *wh = rc.bottom;
    double fit = min(*ww / g_vw, *wh / g_vh) * exp2(g_zoom);
    *w = g_vw * fit;
    *h = g_vh * fit;
    return *w > 0 && *h > 0;
}

// mpv places the video centre at win/2 + pan * size. Keep the video covering the
// window on any axis where it is larger than the window; centre it otherwise.
static double clamp_pan(double pan, double size, double win)
{
    if (size <= win)
        return 0;
    double m = (size - win) / (2 * size);
    return pan > m ? m : pan < -m ? -m : pan;
}

static void apply_view(void)
{
    double w, h, ww, wh;
    if (view_metrics(&w, &h, &ww, &wh)) {
        g_panx = clamp_pan(g_panx, w, ww);
        g_pany = clamp_pan(g_pany, h, wh);
    }
    mpv_set_property_async(g_mpv, 0, "video-zoom", MPV_FORMAT_DOUBLE, &g_zoom);
    mpv_set_property_async(g_mpv, 0, "video-pan-x", MPV_FORMAT_DOUBLE, &g_panx);
    mpv_set_property_async(g_mpv, 0, "video-pan-y", MPV_FORMAT_DOUBLE, &g_pany);
}

static void reset_view(void)
{
    g_zoom = g_panx = g_pany = 0;
    apply_view();
}

// Zoom by dz (log2 units) keeping the video point under (mx, my) fixed.
static void zoom_at(int mx, int my, double dz)
{
    double w, h, ww, wh;
    if (!view_metrics(&w, &h, &ww, &wh))
        return;
    double u = (mx - ww / 2) / w - g_panx;
    double v = (my - wh / 2) / h - g_pany;
    double z = g_zoom + dz;
    g_zoom = z < 0 ? 0 : z > ZOOM_MAX ? ZOOM_MAX : z;
    view_metrics(&w, &h, &ww, &wh);
    g_panx = (mx - ww / 2) / w - u;
    g_pany = (my - wh / 2) / h - v;
    apply_view();
}

static void pan_by(int dx, int dy)
{
    double w, h, ww, wh;
    if (!view_metrics(&w, &h, &ww, &wh))
        return;
    g_panx += dx / w;
    g_pany += dy / h;
    apply_view();
}

// ---------------------------------------------------------------------------
// Playback

static void set_shuttle(int s)
{
    char speed[16];
    if (s == 0 && g_shuttle == 0)
        return;
    if ((s < 0) != (g_shuttle < 0)) {
        // Real backward decoding gives smooth, frame-by-frame reverse. Like a tape,
        // rewinding stops at the start (keep-open pauses there instead of looping).
        mpv_cmd("set", "play-direction", s < 0 ? "backward" : "forward");
        mpv_cmd("set", "loop-file", s < 0 ? "no" : "inf");
    }
    if ((s != 0) != (g_shuttle != 0)) {
        // No sound while shuttling (like a VCR); skipping audio decoding and allowing
        // the decoder to drop frames keeps high speeds fluid. Afterwards the audio
        // track that was playing is restored.
        if (s) {
            char *aid = mpv_get_property_string(g_mpv, "aid");
            snprintf(g_shuttle_aid, sizeof(g_shuttle_aid), "%s", aid ? aid : "auto");
            mpv_free(aid);
        }
        mpv_cmd("set", "aid", s ? "no" : g_shuttle_aid);
        mpv_cmd("set", "framedrop", s ? "decoder+vo" : "vo");
    }
    snprintf(speed, sizeof(speed), "%g", s ? kShuttle[abs(s) - 1] : 1.0);
    mpv_cmd("set", "speed", speed);
    mpv_cmd("set", "pause", "no");
    g_shuttle = s;
    draw_bar();
}

// dir = +1 fast forward, -1 fast reverse. Pressing again in the same direction
// goes faster; switching direction starts at the slowest speed.
static void shuttle_step(int dir)
{
    int n = (int)ARRAYSIZE(kShuttle);
    int s = g_shuttle * dir > 0 ? g_shuttle + dir : dir;
    set_shuttle(abs(s) > n ? dir * n : s);
}

static void play_pause(void)
{
    if (g_shuttle)
        set_shuttle(0);
    else
        mpv_cmd("cycle", "pause", NULL);
}

static void show_osd(int kind)
{
    g_osd = kind;
    SetTimer(g_hwnd, OSD_TIMER, OSD_MS, NULL);
    draw_osd();
}

static void change_volume(int delta)
{
    char v[16];
    g_volume = g_volume + delta < 0 ? 0 : g_volume + delta > 100 ? 100 : g_volume + delta;
    g_muted = FALSE;
    snprintf(v, sizeof(v), "%g", g_volume);
    mpv_cmd("set", "volume", v);
    mpv_cmd("set", "mute", "no");
    show_osd(OSD_VOLUME);
}

static void toggle_mute(void)
{
    g_muted = !g_muted;
    mpv_cmd("set", "mute", g_muted ? "yes" : "no");
    show_osd(OSD_VOLUME);
}

// dir = +1 / -1. Repeated presses in the same direction add up on screen.
static void jump(int dir)
{
    char s[16];
    if (g_osd != OSD_SEEK || (g_seek_total > 0) != (dir > 0))
        g_seek_total = 0;
    g_seek_total += dir * g_jump;
    snprintf(s, sizeof(s), "%d", dir * g_jump);
    mpv_cmd("seek", s, "relative+exact");
    show_osd(OSD_SEEK);
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
// Control bar, drawn by mpv as an ASS overlay in window pixel coordinates

typedef struct {
    double s; // DPI scale
    int w, h, top, x0, x1, ty, by, cell;
    int bx[3]; // button centres: rewind, play, fast forward
} Layout;

#define S(v) ((int)lround((v) * L->s))

static void get_layout(Layout *L)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    L->s = GetDpiForWindow(g_hwnd) / 96.0;
    L->w = rc.right;
    L->h = rc.bottom;
    L->top = L->h - S(64);
    L->x0 = S(16);
    L->x1 = L->w - S(16);
    L->ty = L->top + S(16);
    L->by = L->top + S(42);
    L->cell = S(36);
    L->bx[0] = L->w / 2 - S(48);
    L->bx[1] = L->w / 2;
    L->bx[2] = L->w / 2 + S(48);
}

static int hit_test(int x, int y)
{
    Layout l, *L = &l;
    get_layout(L);
    if (!g_bar_visible || y < L->top || y >= L->h || x < 0 || x >= L->w)
        return HIT_NONE;
    if (y < L->ty + S(10) && x >= L->x0 - S(8) && x <= L->x1 + S(8))
        return HIT_TRACK;
    for (int i = 0; i < 3; i++)
        if (abs(x - L->bx[i]) <= L->cell / 2 && abs(y - L->by) <= L->cell / 2)
            return HIT_REW + i;
    return HIT_BAR;
}

typedef struct {
    char buf[8192];
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

// Sends the overlay to mpv (or removes it when empty), unless it is unchanged
// since the last call: no need to make mpv re-render it.
static void send_overlay(const char *id, const Ass *a, char *last)
{
    if (!g_mpv || strcmp(a->buf, last) == 0)
        return;
    memcpy(last, a->buf, a->len + 1);
    if (a->len == 0) {
        const char *cmd[] = { "osd-overlay", id, "none", "", NULL };
        mpv_command_async(g_mpv, 0, cmd);
    } else {
        char rx[16], ry[16];
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        snprintf(rx, sizeof(rx), "%ld", rc.right);
        snprintf(ry, sizeof(ry), "%ld", rc.bottom);
        const char *cmd[] = { "osd-overlay", id, "ass-events", a->buf, rx, ry, NULL };
        mpv_command_async(g_mpv, 0, cmd);
    }
}

// While the bar is up, raise subtitles above it.
static void update_sub_margin(void)
{
    static int last = -1;
    Layout l, *L = &l;
    char v[16];
    get_layout(L);
    int m = SUB_MARGIN;
    if (g_bar_visible && L->h > 0)
        m += (int)lround((L->h - L->top) * 720.0 / L->h);
    if (m == last || !g_mpv)
        return;
    last = m;
    snprintf(v, sizeof(v), "%d", m);
    mpv_cmd("set", "sub-margin-y", v);
}

static void draw_bar(void)
{
    static Ass a;
    static char last[sizeof(a.buf)];
    a.len = 0;
    a.buf[0] = 0;
    update_sub_margin();

    if (g_bar_visible && g_mpv) {
        Layout l, *L = &l;
        get_layout(L);
        double frac = g_scrubbing ? g_scrub_frac : g_duration > 0 ? g_time / g_duration : 0;
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
            BOOL active = (i == 0 && g_shuttle < 0) || (i == 2 && g_shuttle > 0);
            ass_shape(&a, active ? ACCENT : 0xFFFFFF, 0);
            if (i == 0) {
                ass_tri(&a, cx + S(1), cy - S(7), cx + S(1), cy + S(7), cx - S(8), cy);
                ass_tri(&a, cx + S(10), cy - S(7), cx + S(10), cy + S(7), cx + S(1), cy);
            } else if (i == 2) {
                ass_tri(&a, cx - S(10), cy - S(7), cx - S(10), cy + S(7), cx - S(1), cy);
                ass_tri(&a, cx - S(1), cy - S(7), cx - S(1), cy + S(7), cx + S(8), cy);
            } else if (g_paused || g_shuttle) {
                ass_tri(&a, cx - S(6), cy - S(9), cx - S(6), cy + S(9), cx + S(9), cy);
            } else {
                ass_rect(&a, cx - S(7), cy - S(8), cx - S(2), cy + S(8));
                ass_rect(&a, cx + S(2), cy - S(8), cx + S(7), cy + S(8));
            }
        }

        char t[32], d[32];
        fmt_time(t, sizeof(t), g_scrubbing ? frac * g_duration : g_time);
        fmt_time(d, sizeof(d), g_duration);
        ass_printf(&a, "\n{\\an4\\pos(%d,%d)\\fs%d\\bord0\\shad0\\1c&HFFFFFF&\\1a&H00&}%s / %s",
                   L->x0, L->by, S(15), t, d);
        if (g_shuttle)
            ass_printf(&a, "\n{\\an6\\pos(%d,%d)\\fs%d\\b1\\bord0\\shad0\\1c&H%06X&\\1a&H00&}%s %g\xC3\x97",
                       L->x1, L->by, S(15), ACCENT, g_shuttle < 0 ? "REW" : "FF",
                       kShuttle[abs(g_shuttle) - 1]);
    }
    send_overlay(BAR_OVERLAY, &a, last);
}

// Message box at the top centre: volume with a level bar, or the jump distance.
static void draw_osd(void)
{
    static Ass a;
    static char last[sizeof(a.buf)];
    a.len = 0;
    a.buf[0] = 0;

    if (g_osd != OSD_NONE && g_mpv) {
        Layout l, *L = &l;
        char text[sizeof(g_osd_text)];
        get_layout(L);
        if (g_osd == OSD_VOLUME && g_muted)
            snprintf(text, sizeof(text), "Muted");
        else if (g_osd == OSD_VOLUME)
            snprintf(text, sizeof(text), "Volume %d%%", (int)lround(g_volume));
        else if (g_osd == OSD_SEEK)
            snprintf(text, sizeof(text), "%s%d s", g_seek_total < 0 ? "\xE2\x88\x92" : "+",
                     abs(g_seek_total));
        else
            snprintf(text, sizeof(text), "%s", g_osd_text);

        // Box wide enough for the text (rough average glyph width), within the window.
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
    send_overlay(OSD_OVERLAY, &a, last);
}

static void show_bar(void)
{
    if (!g_bar_visible) {
        g_bar_visible = TRUE;
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
    }
    SetTimer(g_hwnd, BAR_TIMER, BAR_HIDE_MS, NULL);
    draw_bar();
}

static void on_bar_timer(void)
{
    // Stay up while the mouse is on the bar, while scrubbing and while shuttling.
    if (g_hover != HIT_NONE || g_scrubbing || g_shuttle)
        return;
    KillTimer(g_hwnd, BAR_TIMER);
    g_bar_visible = FALSE;
    draw_bar();
    if (g_fullscreen && g_last_mouse.x >= 0)
        SetCursor(NULL);
}

static void scrub_to(int x)
{
    Layout l, *L = &l;
    char pct[32];
    get_layout(L);
    double f = (double)(x - L->x0) / (L->x1 - L->x0);
    g_scrub_frac = f < 0 ? 0 : f > 1 ? 1 : f;
    snprintf(pct, sizeof(pct), "%.4f", g_scrub_frac * 100);
    mpv_cmd("seek", pct, "absolute-percent+exact");
    draw_bar();
}

// ---------------------------------------------------------------------------
// Window

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

// Resize the window once per file to the video's size (at most 80% of the screen),
// unless the window size was restored from the last session.
static void size_window_to_video(void)
{
    MONITORINFO mi = { sizeof(mi) };
    if (g_restored_placement || g_fullscreen || IsZoomed(g_hwnd) ||
        !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return;
    RECT wa = mi.rcWork;
    double aw = wa.right - wa.left, ah = wa.bottom - wa.top;
    double s = min(1.0, min(aw * 0.8 / g_vw, ah * 0.8 / g_vh));
    RECT r = { 0, 0, (LONG)(g_vw * s), (LONG)(g_vh * s) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(g_hwnd, GWL_STYLE), FALSE,
                             GetWindowLongW(g_hwnd, GWL_EXSTYLE), GetDpiForWindow(g_hwnd));
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(g_hwnd, NULL, wa.left + ((int)aw - w) / 2, wa.top + ((int)ah - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// mpv renders into its own child window. Disabling it makes Windows route all
// mouse input to our window, so we handle wheel/drag/clicks ourselves.
static BOOL CALLBACK disable_child(HWND child, LPARAM lp)
{
    (void)lp;
    if (IsWindowEnabled(child))
        EnableWindow(child, FALSE);
    return TRUE;
}

static void on_video_reconfig(void)
{
    int64_t dw = 0, dh = 0, rot = 0;
    EnumChildWindows(g_hwnd, disable_child, 0);
    if (mpv_get_property(g_mpv, "video-out-params/dw", MPV_FORMAT_INT64, &dw) < 0 ||
        mpv_get_property(g_mpv, "video-out-params/dh", MPV_FORMAT_INT64, &dh) < 0 ||
        dw <= 0 || dh <= 0)
        return;
    mpv_get_property(g_mpv, "video-out-params/rotate", MPV_FORMAT_INT64, &rot);
    g_vw = (double)(rot % 180 ? dh : dw);
    g_vh = (double)(rot % 180 ? dw : dh);
    if (!g_sized_to_video) {
        g_sized_to_video = TRUE;
        size_window_to_video();
    }
    apply_view();
}

static void on_property_change(mpv_event_property *p)
{
    double d = p->format == MPV_FORMAT_DOUBLE ? *(double *)p->data : 0;
    int flag = p->format == MPV_FORMAT_FLAG ? *(int *)p->data : 0;
    if (!strcmp(p->name, "time-pos"))
        g_time = d;
    else if (!strcmp(p->name, "duration"))
        g_duration = d;
    else if (!strcmp(p->name, "pause"))
        g_paused = flag;
    else if (!strcmp(p->name, "eof-reached") && flag && g_shuttle < 0)
        set_shuttle(0); // rewound to the start: play forward again
    draw_bar();
}

static void handle_mpv_events(void)
{
    while (g_mpv) {
        mpv_event *e = mpv_wait_event(g_mpv, 0);
        switch (e->event_id) {
        case MPV_EVENT_NONE:
            return;
        case MPV_EVENT_VIDEO_RECONFIG:
            on_video_reconfig();
            break;
        case MPV_EVENT_PROPERTY_CHANGE:
            on_property_change(e->data);
            break;
        case MPV_EVENT_SHUTDOWN:
            mpv_terminate_destroy(g_mpv);
            g_mpv = NULL;
            DestroyWindow(g_hwnd);
            return;
        default:
            break;
        }
    }
}

static void on_mpv_wakeup(void *ctx)
{
    (void)ctx;
    PostMessageW(g_hwnd, WM_MPV_WAKEUP, 0, 0);
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

static void open_file(const wchar_t *path)
{
    char *utf8 = to_utf8(path);
    if (!utf8)
        return;
    set_shuttle(0);
    // Track choices are global options in mpv; let each new file pick its own.
    mpv_cmd("set", "aid", "auto");
    mpv_cmd("set", "sid", "auto");
    mpv_cmd("loadfile", utf8, NULL);
    free(utf8);

    const wchar_t *name = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            name = p + 1;
    SetWindowTextW(g_hwnd, name);
    g_sized_to_video = FALSE;
    reset_view();
    show_bar();
}

static void add_subtitle(const wchar_t *path)
{
    char *utf8 = to_utf8(path);
    if (!utf8)
        return;
    mpv_cmd("sub-add", utf8, "select");
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

static BOOL pick_file(HWND owner, const wchar_t *filter, wchar_t *buf, DWORD len)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    buf[0] = 0;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = len;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn);
}

static void on_left_down(HWND hwnd, int x, int y)
{
    switch (hit_test(x, y)) {
    case HIT_TRACK:
        g_scrubbing = TRUE;
        SetCapture(hwnd);
        scrub_to(x);
        break;
    case HIT_REW:  shuttle_step(-1); break;
    case HIT_PLAY: play_pause(); break;
    case HIT_FF:   shuttle_step(+1); break;
    case HIT_BAR:  break;
    default:
        g_dragging = TRUE;
        g_drag_last.x = x;
        g_drag_last.y = y;
        SetCapture(hwnd);
        break;
    }
}

static void open_dialog(HWND hwnd)
{
    static wchar_t path[32768];
    if (pick_file(hwnd, kVideoFilter, path, ARRAYSIZE(path)))
        open_file(path);
}

static void subtitle_dialog(HWND hwnd)
{
    static wchar_t path[32768];
    if (pick_file(hwnd, kSubFilter, path, ARRAYSIZE(path)))
        add_subtitle(path);
}

static char *track_prop(int64_t i, const char *field)
{
    char name[64];
    snprintf(name, sizeof(name), "track-list/%lld/%s", (long long)i, field);
    return mpv_get_property_string(g_mpv, name);
}

// Track descriptions from the last menu, by track id, for the on-screen message.
static char g_audio_names[MAX_TRACKS][128], g_sub_names[MAX_TRACKS][128];

// Adds one radio item per track of the given type ("audio" or "sub") plus "Off",
// labelled like "2: Commentary, eng, aac". Returns the number of tracks.
static int add_track_items(HMENU menu, const char *type, int base, char (*names)[128])
{
    int64_t count = 0;
    int n = 0, checked = 0;
    mpv_get_property(g_mpv, "track-list/count", MPV_FORMAT_INT64, &count);
    AppendMenuW(menu, MF_STRING, base, L"Off");
    for (int64_t i = 0; i < count; i++) {
        char *s = track_prop(i, "type");
        BOOL match = s && !strcmp(s, type);
        mpv_free(s);
        int64_t id = 0;
        char idname[64];
        snprintf(idname, sizeof(idname), "track-list/%lld/id", (long long)i);
        if (!match || mpv_get_property(g_mpv, idname, MPV_FORMAT_INT64, &id) < 0 ||
            id < 1 || id >= MAX_TRACKS)
            continue;

        char text[256] = "";
        static const char *fields[] = { "title", "lang", "codec" };
        for (int f = 0; f < 3; f++) {
            s = track_prop(i, fields[f]);
            if (s && *s)
                snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s", *text ? ", " : "", s);
            mpv_free(s);
        }
        s = track_prop(i, "external");
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

        s = track_prop(i, "selected");
        if (s && !strcmp(s, "yes"))
            checked = (int)id;
        mpv_free(s);
        n++;
    }
    // Audio is switched off while shuttling; show the track that will come back.
    if (!strcmp(type, "audio") && g_shuttle)
        checked = !strcmp(g_shuttle_aid, "no") ? 0 : atoi(g_shuttle_aid);
    CheckMenuRadioItem(menu, base, base + MAX_TRACKS - 1, base + checked, MF_BYCOMMAND);
    return n;
}

static void select_track(const char *prop, int id)
{
    char v[16];
    if (id)
        snprintf(v, sizeof(v), "%d", id);
    else
        snprintf(v, sizeof(v), "no");
    BOOL audio = !strcmp(prop, "aid");
    if (audio && g_shuttle)
        snprintf(g_shuttle_aid, sizeof(g_shuttle_aid), "%s", v); // applied when shuttling ends
    else
        mpv_cmd("set", prop, v);

    if (!id)
        snprintf(g_osd_text, sizeof(g_osd_text), audio ? "Audio off" : "Subtitles off");
    else
        snprintf(g_osd_text, sizeof(g_osd_text), "%s: %s", audio ? "Audio" : "Subtitles",
                 audio ? g_audio_names[id] : g_sub_names[id]);
    show_osd(OSD_TEXT);
}

// Right-click menu at screen position (x, y).
static void show_menu(HWND hwnd, int x, int y)
{
    int n = (int)ARRAYSIZE(kJumps);
    HMENU audio = CreatePopupMenu(), subs = CreatePopupMenu();
    HMENU jumps = CreatePopupMenu(), menu = CreatePopupMenu();

    int naudio = add_track_items(audio, "audio", ID_AUDIO, g_audio_names);
    add_track_items(subs, "sub", ID_SUB, g_sub_names);
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
    AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open file...\tO");
    AppendMenuW(menu, MF_STRING | (g_fullscreen ? MF_CHECKED : 0), ID_FULLSCREEN, L"Fullscreen\tF");
    AppendMenuW(menu, MF_STRING | (g_zoom > 0 ? 0 : MF_GRAYED), ID_RESET_ZOOM, L"Reset zoom\tR");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit\tQ");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(menu); // also destroys the submenus
    if (cmd >= ID_JUMP && cmd < ID_JUMP + n)
        set_jump(kJumps[cmd - ID_JUMP]);
    else if (cmd >= ID_AUDIO && cmd < ID_AUDIO + MAX_TRACKS)
        select_track("aid", cmd - ID_AUDIO);
    else if (cmd >= ID_SUB && cmd < ID_SUB + MAX_TRACKS)
        select_track("sid", cmd - ID_SUB);
    else if (cmd == ID_SUB_LOAD)
        subtitle_dialog(hwnd);
    else if (cmd == ID_OPEN)
        open_dialog(hwnd);
    else if (cmd == ID_FULLSCREEN)
        toggle_fullscreen();
    else if (cmd == ID_RESET_ZOOM)
        reset_view();
    else if (cmd == ID_QUIT)
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MPV_WAKEUP:
        handle_mpv_events();
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        zoom_at(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA * ZOOM_STEP);
        return 0;
    }

    case WM_LBUTTONDOWN:
        on_left_down(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!g_tracking_leave) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            g_tracking_leave = TrackMouseEvent(&t);
        }
        if (x != g_last_mouse.x || y != g_last_mouse.y) { // ignore synthetic moves
            g_last_mouse.x = x;
            g_last_mouse.y = y;
            show_bar();
        }
        if (g_scrubbing) {
            scrub_to(x);
        } else if (g_dragging) {
            if (g_zoom > 0) {
                SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
                pan_by(x - g_drag_last.x, y - g_drag_last.y);
            }
            g_drag_last.x = x;
            g_drag_last.y = y;
        }
        g_hover = hit_test(x, y);
        draw_bar();
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking_leave = FALSE;
        g_last_mouse.x = g_last_mouse.y = -1;
        g_hover = HIT_NONE;
        draw_bar();
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        g_dragging = FALSE;
        if (g_scrubbing) {
            g_scrubbing = FALSE;
            g_time = g_scrub_frac * g_duration; // until mpv reports the new position
            draw_bar();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) != HIT_NONE)
            on_left_down(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); // fast clicks on the bar
        else
            toggle_fullscreen();
        return 0;
    case WM_CONTEXTMENU: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (lp == -1) // opened from the keyboard
            GetCursorPos(&pt);
        show_menu(hwnd, pt.x, pt.y);
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
            draw_osd();
        }
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:
        case 'K':       play_pause(); break;
        case 'J':       shuttle_step(-1); break;
        case 'L':       shuttle_step(+1); break;
        case VK_LEFT:   jump(-1); break;
        case VK_RIGHT:  jump(+1); break;
        case VK_UP:     change_volume(5); break;
        case VK_DOWN:   change_volume(-5); break;
        case 'M':       toggle_mute(); break;
        case 'R':       reset_view(); break;
        case 'F':
        case VK_F11:    toggle_fullscreen(); break;
        case VK_ESCAPE: if (g_fullscreen) toggle_fullscreen(); break;
        case 'Q':       PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case 'O':       open_dialog(hwnd); break;
        }
        return 0;

    case WM_DROPFILES: {
        static wchar_t path[32768];
        if (DragQueryFileW((HDROP)wp, 0, path, ARRAYSIZE(path))) {
            if (is_subtitle_file(path))
                add_subtitle(path);
            else
                open_file(path);
        }
        DragFinish((HDROP)wp);
        SetForegroundWindow(hwnd);
        return 0;
    }

    case WM_SIZE:
        if (g_mpv) {
            if (g_zoom > 0)
                apply_view(); // re-clamp pan for the new window size
            draw_bar();
            draw_osd();
        }
        return 0;

    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        // Let mpv shut down (and destroy its child window) while we keep pumping
        // messages; the window is destroyed on MPV_EVENT_SHUTDOWN.
        if (g_mpv && !g_closing) {
            g_closing = TRUE;
            save_placement();
            ShowWindow(hwnd, SW_HIDE);
            mpv_cmd("quit", NULL, NULL);
            return 0;
        }
        if (!g_mpv)
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

    static wchar_t picked[32768];
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *path = argc > 1 ? argv[1] : NULL;
    if (!path) {
        if (!pick_file(NULL, kVideoFilter, picked, ARRAYSIZE(picked)))
            return 0;
        path = picked;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"MinimalVideoViewer";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"MinimalVideoViewer",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 540, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;

    g_mpv = mpv_create();
    if (!g_mpv) {
        MessageBoxW(g_hwnd, L"Could not create mpv instance (is libmpv-2.dll next to the exe?)",
                    L"MinimalVideoViewer", MB_ICONERROR);
        return 1;
    }
    int64_t wid = (int64_t)(intptr_t)g_hwnd;
    mpv_set_option(g_mpv, "wid", MPV_FORMAT_INT64, &wid);
    mpv_set_option_string(g_mpv, "loop-file", "inf");
    mpv_set_option_string(g_mpv, "keep-open", "yes");     // fast reverse stops at the start
    mpv_set_option_string(g_mpv, "cache", "yes");         // packets in RAM: quick reverse switch, loops from memory
    mpv_set_option_string(g_mpv, "hwdec", "auto-safe");   // GPU decoding (D3D11VA etc.)
    mpv_set_option_string(g_mpv, "idle", "yes");          // stay open if a file fails to load
    mpv_set_option_string(g_mpv, "osd-level", "0");
    mpv_set_option_string(g_mpv, "osc", "no");
    mpv_set_option_string(g_mpv, "input-default-bindings", "no");
    mpv_set_option_string(g_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(g_mpv, "input-cursor", "no");
    mpv_set_option_string(g_mpv, "cursor-autohide", "no");
    if (mpv_initialize(g_mpv) < 0) {
        MessageBoxW(g_hwnd, L"Could not initialize mpv.", L"MinimalVideoViewer", MB_ICONERROR);
        return 1;
    }
    mpv_observe_property(g_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    mpv_set_wakeup_callback(g_mpv, on_mpv_wakeup, NULL);

    g_restored_placement = restore_placement();
    if (!g_restored_placement)
        ShowWindow(g_hwnd, show);
    open_file(path);
    LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
