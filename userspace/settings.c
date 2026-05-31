/* settings - a GUI system settings app for Lariat (Phase 4).
 *
 * A windowserver client (ws.h + wtk.h) with five tabs:
 *   Host   - view/change the hostname (writes /etc/hostname, persists to
 *            /var/etc via etc_sync, applies live through /proc).
 *   Users  - list local accounts and add/remove them via /bin/useradd and
 *            /bin/userdel (requires the desktop to run as root).
 *   Net    - read-only view of /proc/net/info.
 *   Time   - read-only view of /proc/uptime and /proc/version.
 *   Disp   - display/window information.
 *
 * Tabs are implemented by rebuilding the widget list on demand; wtk's retained
 * model is small enough that a full rebuild + redraw per interaction is fine.
 */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/pwd.h"
#include "libc/ws.h"
#include "libc/wtk.h"

#define WIN_W 470
#define WIN_H 300
#define BG    0x00202830
#define CONTENT_Y 48

static ws_conn_t   c;
static wtk_window_t W;
static int   g_tab;            /* 0 Host, 1 Users, 2 Net, 3 Time, 4 Disp */
static int   g_host_input = -1;
static int   g_user_input = -1;
static char  g_status[64];

#define MAXU 8
static char  g_names[MAXU][32];
static int   g_nuser;

static const char *TABS[5] = { "Host", "Users", "Net", "Time", "Disp" };

/* --- small file helpers --------------------------------------------------- */
static int readfile(const char *path, char *buf, int n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int o = 0;
    for (;;) {
        if (o + 1 >= n) break;
        long r = read(fd, buf + o, n - 1 - o);
        if (r <= 0) break;
        o += (int)r;
    }
    close(fd);
    buf[o] = '\0';
    return o;
}

static int writefile(const char *path, const char *data, int len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long w = write(fd, data, (size_t)len);
    close(fd);
    return (w == (long)len) ? 0 : -1;
}

/* fork/execve a program, wait, return exit status (-1 on error). */
static int run(const char *path, char *const argv[]) {
    int pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execve(path, argv, 0); _exit(127); }
    int st = -1;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return (st >> 8) & 0xff;
}

/* --- tab content builders ------------------------------------------------- */
static void rebuild(void);

static void on_tab(wtk_window_t *w, int id, void *user) {
    (void)w; (void)id;
    g_tab = (int)(long)user;
    g_status[0] = '\0';
    rebuild();
}

static void on_apply_host(wtk_window_t *w, int id, void *user) {
    (void)id; (void)user;
    const char *hn = wtk_get_text(w, g_host_input);
    char line[80];
    int n = snprintf(line, sizeof(line), "%s\n", hn);
    if (hn[0] == '\0') {
        strcpy(g_status, "hostname cannot be empty");
    } else if (writefile("/etc/hostname", line, n) != 0) {
        strcpy(g_status, "failed to write /etc/hostname");
    } else {
        etc_sync("hostname");                         /* persist to /var/etc */
        writefile("/proc/sys/kernel/hostname", hn, (int)strlen(hn)); /* live */
        snprintf(g_status, sizeof(g_status), "hostname set to %s", hn);
    }
    rebuild();
}

static void load_users(void) {
    g_nuser = 0;
    static char buf[16384];
    if (readfile("/etc/passwd", buf, sizeof(buf)) < 0) return;
    char *save = 0;
    for (char *line = strtok_r(buf, "\n", &save); line && g_nuser < MAXU;
         line = strtok_r(0, "\n", &save)) {
        if (line[0] == '\0' || line[0] == '#') continue;
        int i = 0;
        for (; line[i] && line[i] != ':' && i < 31; i++) g_names[g_nuser][i] = line[i];
        g_names[g_nuser][i] = '\0';
        g_nuser++;
    }
}

static void on_add_user(wtk_window_t *w, int id, void *user) {
    (void)id; (void)user;
    const char *name = wtk_get_text(w, g_user_input);
    if (name[0] == '\0') { strcpy(g_status, "enter a user name"); rebuild(); return; }
    char nbuf[32];
    strncpy(nbuf, name, sizeof(nbuf) - 1); nbuf[sizeof(nbuf) - 1] = '\0';
    char *argv[] = { "useradd", nbuf, 0 };
    int rc = run("/bin/useradd", argv);
    if (rc == 0) snprintf(g_status, sizeof(g_status), "added %s", nbuf);
    else         snprintf(g_status, sizeof(g_status), "useradd failed (%d)", rc);
    rebuild();
}

static void on_del_user(wtk_window_t *w, int id, void *user) {
    (void)w; (void)id;
    const char *name = (const char *)user;
    char *argv[] = { "userdel", (char *)name, 0 };
    int rc = run("/bin/userdel", argv);
    if (rc == 0) snprintf(g_status, sizeof(g_status), "removed %s", name);
    else         snprintf(g_status, sizeof(g_status), "userdel failed (%d)", rc);
    rebuild();
}

static void add_tabbar(void) {
    for (int i = 0; i < 5; i++)
        wtk_button(&W, 8 + i * 92, 8, 86, 28, TABS[i], on_tab, (void *)(long)i);
}

static void build_host(void) {
    wtk_label(&W, 12, CONTENT_Y, "System hostname:", 0);
    g_host_input = wtk_input(&W, 12, CONTENT_Y + 24, 280, 26);
    char hn[80];
    int n = readfile("/etc/hostname", hn, sizeof(hn));
    if (n > 0) { while (n && (hn[n - 1] == '\n' || hn[n - 1] == '\r')) hn[--n] = '\0'; }
    wtk_set_text(&W, g_host_input, n > 0 ? hn : "lariat");
    wtk_button(&W, 300, CONTENT_Y + 24, 90, 26, "Apply", on_apply_host, 0);
    wtk_label(&W, 12, CONTENT_Y + 64, "Click the field, type a name, then Apply.", 0x00808a98);
}

static void build_users(void) {
    load_users();
    wtk_label(&W, 12, CONTENT_Y, "Local accounts:", 0);
    int y = CONTENT_Y + 24;
    for (int i = 0; i < g_nuser; i++) {
        wtk_label(&W, 20, y + 6, g_names[i], 0x0080e0a0);
        wtk_button(&W, 360, y, 70, 24, "Delete", on_del_user, g_names[i]);
        y += 28;
    }
    int iy = WIN_H - 40;
    wtk_label(&W, 12, iy - 22, "New user:", 0);
    g_user_input = wtk_input(&W, 90, iy - 26, 200, 26);
    wtk_button(&W, 300, iy - 26, 90, 26, "Add", on_add_user, 0);
}

static void build_textfile(const char *title, const char *path) {
    wtk_label(&W, 12, CONTENT_Y, title, 0);
    static char buf[1024];
    int n = readfile(path, buf, sizeof(buf));
    int y = CONTENT_Y + 24;
    if (n <= 0) { wtk_label(&W, 20, y, "(unavailable)", 0x00808a98); return; }
    char *save = 0;
    for (char *line = strtok_r(buf, "\n", &save); line && y < WIN_H - 28;
         line = strtok_r(0, "\n", &save)) {
        wtk_label(&W, 20, y, line, 0x0080e0a0);
        y += 18;
    }
}

static void build_time(void) {
    wtk_label(&W, 12, CONTENT_Y, "Date / time", 0);
    char buf[128];
    int y = CONTENT_Y + 24;
    int n = readfile("/proc/uptime", buf, sizeof(buf));
    if (n > 0) { while (n && (buf[n-1]=='\n')) buf[--n]='\0';
        char l[160]; snprintf(l, sizeof(l), "Uptime (s): %s", buf);
        wtk_label(&W, 20, y, l, 0x0080e0a0); y += 20; }
    n = readfile("/proc/version", buf, sizeof(buf));
    if (n > 0) { while (n && (buf[n-1]=='\n')) buf[--n]='\0';
        wtk_label(&W, 20, y, buf, 0x0080e0a0); y += 20; }
    wtk_label(&W, 20, y + 6, "(RTC set not supported yet)", 0x00808a98);
}

static void build_disp(void) {
    wtk_label(&W, 12, CONTENT_Y, "Display", 0);
    char l[80];
    snprintf(l, sizeof(l), "Window surface: %dx%d", WIN_W, WIN_H);
    wtk_label(&W, 20, CONTENT_Y + 24, l, 0x0080e0a0);
    wtk_label(&W, 20, CONTENT_Y + 46, "Framebuffer managed by the windowserver.", 0x0080e0a0);
}

static void rebuild(void) {
    W.nwidget = 0;
    add_tabbar();
    /* highlight active tab */
    if (g_tab >= 0 && g_tab < 5)
        W.widgets[g_tab].bg = 0x00586273;
    switch (g_tab) {
    case 0: build_host();  break;
    case 1: build_users(); break;
    case 2: build_textfile("Network", "/proc/net/info"); break;
    case 3: build_time();  break;
    case 4: build_disp();  break;
    }
    if (g_status[0])
        wtk_label(&W, 12, WIN_H - 18, g_status, 0x00e0c060);
    wtk_draw(&W);
}

int main(void) {
    if (ws_connect(&c) != 0) { printf("settings: no window server\n"); return 1; }
    int win = ws_create_window(&c, 120, 90, WIN_W, WIN_H, "Settings");
    if (win < 0) { printf("settings: create window failed\n"); return 1; }

    wtk_init(&W, &c, win, WIN_W, WIN_H, BG);
    g_tab = 0;
    rebuild();

    for (;;) {
        ws_event_t ev;
        if (!ws_wait_event(&c, &ev)) break;
        if (ev.op == WS_EV_CLOSE) break;
        if (wtk_handle(&W, &ev)) wtk_draw(&W);
    }
    ws_destroy_window(&c, win);
    return 0;
}
