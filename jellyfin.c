/*
 * jellyfin.dll - LCD Smartie plugin showing Jellyfin "now playing" info.
 *
 * A background thread polls  <Server>/Sessions  every PollInterval ms and
 * caches the active sessions; the exported functions only read that cache,
 * so they never block Smartie's display loop on the network.
 *
 * Config: jellyfin.ini next to the DLL (created with defaults on first load).
 *
 * Usage in Smartie:
 *   $dll(jellyfin,1,<field>,<filter>)   single field (see field_value())
 *   $dll(jellyfin,2,<width>,<filter>)   progress bar
 *   $dll(jellyfin,3,<filter>,)          number of sessions playing
 *   $dll(jellyfin,4,<format>,<filter>)  template, e.g. {artist} - {title}
 *   $dll(jellyfin,5,<command>,<filter>) remote control (for Actions, e.g. on
 *                                       LCD buttons): PlayPause Stop NextTrack
 *                                       PreviousTrack Seek+10 Seek-10 VolumeUp
 *                                       VolumeDown ToggleMute ...
 *   $dll(jellyfin,20,,)                 last error / status
 *
 * <filter> is optional: a Jellyfin user name, device name or client name.
 *
 * https://github.com/Spooky098529/lcdsmartie-jellyfin  (MIT License)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DLLEXPORT __declspec(dllexport)
#define TICKS_PER_SEC 10000000LL
#define MAX_SESSIONS 16
#define OUTSZ 1024

/* ------------------------------------------------------------------ */
/* Cached state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char user[64], device[96], client[64], type[32];
    char title[256], series[256], artist[256], album[256];
    char id[64];                /* Jellyfin session id, for remote control */
    int season, episode, year, paused, remote;
    long long pos, dur;         /* in ticks (100ns) */
    ULONGLONG at;               /* GetTickCount64() when pos was valid */
} Sess;

static CRITICAL_SECTION g_cs;
static Sess g_sess[MAX_SESSIONS];
static int g_nsess;
static int g_havedata;
static char g_err[256] = "Starting...";

/* Remote-control commands, queued by function5 and sent by the poll thread
   so Smartie's UI thread never waits on the network. */
#define MAX_CMDS 8
typedef struct { WCHAR path[256]; char name[32]; } Cmd;
static Cmd g_q[MAX_CMDS];
static int g_qhead, g_qlen;
static char g_cmderr[256];
static ULONGLONG g_cmderr_at;
#define CMDERR_SHOW_MS 30000    /* a command error is reported for 30 s */

/* config (guarded by g_cs) */
static char g_idle[128];
static char g_deffilter[96];
static char g_barempty[8] = "_";

static HANDLE g_thread, g_stop, g_wake;
static WCHAR g_inipath[MAX_PATH];

/* Return buffer must outlive the call; one per calling thread. */
static __thread char t_out[OUTSZ];

/* ------------------------------------------------------------------ */
/* Minimal JSON parser                                                 */
/* ------------------------------------------------------------------ */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct J {
    int t;
    double n;
    char *s;            /* J_STR value */
    char *key;          /* member name when inside an object */
    struct J *kids, *next;
} J;

static void jfree(J *j)
{
    while (j) {
        J *nx = j->next;
        jfree(j->kids);
        free(j->s);
        free(j->key);
        free(j);
        j = nx;
    }
}

static void jws(const char **p) { while (**p && isspace((unsigned char)**p)) (*p)++; }

static void put_utf8(char **o, unsigned cp)
{
    char *d = *o;
    if (cp < 0x80) *d++ = (char)cp;
    else if (cp < 0x800) { *d++ = 0xC0 | (cp >> 6); *d++ = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { *d++ = 0xE0 | (cp >> 12); *d++ = 0x80 | ((cp >> 6) & 0x3F); *d++ = 0x80 | (cp & 0x3F); }
    else { *d++ = 0xF0 | (cp >> 18); *d++ = 0x80 | ((cp >> 12) & 0x3F); *d++ = 0x80 | ((cp >> 6) & 0x3F); *d++ = 0x80 | (cp & 0x3F); }
    *o = d;
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0xFFFFFFFF;
    }
    return v;
}

static char *jparse_str(const char **p)
{
    const char *s = *p + 1;     /* skip opening quote */
    const char *e = s;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
    if (*e != '"') return NULL;
    char *out = malloc((e - s) + 1), *o = out;   /* decoded is never longer */
    if (!out) return NULL;
    while (s < e) {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        switch (*s) {
        case 'n': *o++ = '\n'; s++; break;
        case 't': *o++ = '\t'; s++; break;
        case 'r': *o++ = '\r'; s++; break;
        case 'b': *o++ = '\b'; s++; break;
        case 'f': *o++ = '\f'; s++; break;
        case 'u': {
            if (e - s < 5) { s = e; break; }
            unsigned cp = hex4(s + 1);
            s += 5;
            if (cp == 0xFFFFFFFF) break;
            if (cp >= 0xD800 && cp <= 0xDBFF && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                unsigned lo = hex4(s + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
            }
            put_utf8(&o, cp);
            break;
        }
        default: *o++ = *s++; break;   /* \" \\ \/ */
        }
    }
    *o = 0;
    *p = e + 1;
    return out;
}

static J *jparse_val(const char **p, int depth);

static J *jparse_val(const char **p, int depth)
{
    if (depth > 64) return NULL;
    jws(p);
    J *j = calloc(1, sizeof(J));
    if (!j) return NULL;
    char c = **p;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->t = c == '{' ? J_OBJ : J_ARR;
        (*p)++;
        J **tail = &j->kids;
        jws(p);
        if (**p == close) { (*p)++; return j; }
        for (;;) {
            char *key = NULL;
            jws(p);
            if (j->t == J_OBJ) {
                if (**p != '"' || !(key = jparse_str(p))) goto fail;
                jws(p);
                if (**p != ':') { free(key); goto fail; }
                (*p)++;
            }
            J *v = jparse_val(p, depth + 1);
            if (!v) { free(key); goto fail; }
            v->key = key;
            *tail = v;
            tail = &v->next;
            jws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == close) { (*p)++; return j; }
            goto fail;
        }
    } else if (c == '"') {
        j->t = J_STR;
        if (!(j->s = jparse_str(p))) goto fail;
    } else if (!strncmp(*p, "true", 4)) { j->t = J_BOOL; j->n = 1; *p += 4; }
    else if (!strncmp(*p, "false", 5)) { j->t = J_BOOL; *p += 5; }
    else if (!strncmp(*p, "null", 4)) { j->t = J_NULL; *p += 4; }
    else {
        char *end;
        j->t = J_NUM;
        j->n = strtod(*p, &end);
        if (end == *p) goto fail;
        *p = end;
    }
    return j;
fail:
    jfree(j);
    return NULL;
}

static J *jget(J *o, const char *key)
{
    if (!o || o->t != J_OBJ) return NULL;
    for (J *k = o->kids; k; k = k->next)
        if (k->key && !strcmp(k->key, key)) return k;
    return NULL;
}

static void jcpy(char *dst, size_t sz, J *o, const char *key)
{
    J *v = jget(o, key);
    snprintf(dst, sz, "%s", (v && v->t == J_STR) ? v->s : "");
}

static double jnum(J *o, const char *key)
{
    J *v = jget(o, key);
    return (v && (v->t == J_NUM || v->t == J_BOOL)) ? v->n : 0;
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static void ini_str(const WCHAR *key, const WCHAR *def, char *out, int outsz)
{
    WCHAR w[512];
    GetPrivateProfileStringW(L"Jellyfin", key, def, w, 512, g_inipath);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL);
    out[outsz - 1] = 0;
}

static void ensure_ini(void)
{
    if (GetFileAttributesW(g_inipath) != INVALID_FILE_ATTRIBUTES) return;
    WritePrivateProfileStringW(L"Jellyfin", L"Server", L"http://localhost:8096", g_inipath);
    WritePrivateProfileStringW(L"Jellyfin", L"ApiKey", L"", g_inipath);
    WritePrivateProfileStringW(L"Jellyfin", L"PollInterval", L"2000", g_inipath);
    WritePrivateProfileStringW(L"Jellyfin", L"DefaultFilter", L"", g_inipath);
    WritePrivateProfileStringW(L"Jellyfin", L"IdleText", L"", g_inipath);
    WritePrivateProfileStringW(L"Jellyfin", L"BarEmpty", L"_", g_inipath);
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static void set_err(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_cs);
    strcpy(g_err, buf);
    g_nsess = 0;            /* don't keep showing stale data */
    LeaveCriticalSection(&g_cs);
}

/* Performs METHOD <server><suffix>. Returns the malloc'd NUL-terminated body
   (possibly empty) on a 2xx status, or NULL with a message in err. */
static char *http_req(HINTERNET hs, const WCHAR *server, const WCHAR *apikey,
                      const WCHAR *method, const WCHAR *suffix, char *err, size_t errsz)
{
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    WCHAR host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(server, 0, 0, &uc)) {
        snprintf(err, errsz, "Bad Server URL in jellyfin.ini");
        return NULL;
    }
    /* strip trailing slashes from any base path (reverse-proxy setups) */
    size_t pl = wcslen(path);
    while (pl && path[pl - 1] == L'/') path[--pl] = 0;
    wcsncat(path, suffix, 1023 - pl);

    char *body = NULL;
    HINTERNET hc = NULL, hr = NULL;
    hc = WinHttpConnect(hs, host, uc.nPort, 0);
    if (!hc) { snprintf(err, errsz, "Connect failed (%lu)", GetLastError()); goto done; }
    hr = WinHttpOpenRequest(hc, method, path, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { snprintf(err, errsz, "OpenRequest failed (%lu)", GetLastError()); goto done; }

    WCHAR hdr[700];
    _snwprintf(hdr, 700,
        L"Authorization: MediaBrowser Client=\"LCDSmartie\", Device=\"LCDSmartie\", "
        L"DeviceId=\"lcdsmartie-jellyfin-plugin\", Version=\"1.1\", Token=\"%ls\"\r\n"
        L"Accept: application/json\r\n", apikey);
    hdr[699] = 0;

    if (!WinHttpSendRequest(hr, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hr, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_WINHTTP_TIMEOUT) snprintf(err, errsz, "Server timed out");
        else if (e == ERROR_WINHTTP_CANNOT_CONNECT) snprintf(err, errsz, "Cannot connect to server");
        else if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) snprintf(err, errsz, "Server name not resolved");
        else snprintf(err, errsz, "Request failed (%lu)", e);
        goto done;
    }

    DWORD status = 0, sl = sizeof status;
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status == 401 || status == 403) { snprintf(err, errsz, "Unauthorized - check ApiKey"); goto done; }
    if (status < 200 || status > 299) { snprintf(err, errsz, "HTTP %lu", status); goto done; }

    size_t len = 0, cap = 65536;
    body = malloc(cap);
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!body) { snprintf(err, errsz, "Out of memory"); goto done; }
        if (!WinHttpQueryDataAvailable(hr, &avail)) { snprintf(err, errsz, "Read failed"); free(body); body = NULL; goto done; }
        if (!avail) break;
        if (len + avail + 1 > cap) {
            while (len + avail + 1 > cap) cap *= 2;
            char *nb = realloc(body, cap);
            if (!nb) { free(body); body = NULL; continue; }
            body = nb;
        }
        if (!WinHttpReadData(hr, body + len, avail, &got)) { snprintf(err, errsz, "Read failed"); free(body); body = NULL; goto done; }
        len += got;
    }
    body[len] = 0;
done:
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    return body;
}

static void set_cmderr(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&g_cs);
    va_start(ap, fmt);
    vsnprintf(g_cmderr, sizeof g_cmderr, fmt, ap);
    va_end(ap);
    g_cmderr_at = GetTickCount64();
    LeaveCriticalSection(&g_cs);
}

static int pop_cmd(Cmd *c)
{
    int ok = 0;
    EnterCriticalSection(&g_cs);
    if (g_qlen) {
        *c = g_q[g_qhead];
        g_qhead = (g_qhead + 1) % MAX_CMDS;
        g_qlen--;
        ok = 1;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

/* Send every queued command. Returns how many were sent. */
static int send_cmds(HINTERNET hs, const WCHAR *server, const WCHAR *apikey)
{
    Cmd c;
    int n = 0;
    while (pop_cmd(&c)) {
        char err[256];
        char *body = http_req(hs, server, apikey, L"POST", c.path, err, sizeof err);
        if (body) {
            free(body);
            EnterCriticalSection(&g_cs);
            g_cmderr[0] = 0;
            LeaveCriticalSection(&g_cs);
        } else {
            set_cmderr("%s: %s", c.name, err);
        }
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Poll thread                                                         */
/* ------------------------------------------------------------------ */

static void parse_sessions(J *root, Sess *out, int *n)
{
    *n = 0;
    if (!root || root->t != J_ARR) return;
    for (J *s = root->kids; s && *n < MAX_SESSIONS; s = s->next) {
        J *npi = jget(s, "NowPlayingItem");
        if (!npi || npi->t != J_OBJ) continue;
        J *ps = jget(s, "PlayState");
        Sess *x = &out[(*n)++];
        memset(x, 0, sizeof *x);
        jcpy(x->id, sizeof x->id, s, "Id");
        x->remote = jnum(s, "SupportsRemoteControl") != 0;
        jcpy(x->user, sizeof x->user, s, "UserName");
        jcpy(x->device, sizeof x->device, s, "DeviceName");
        jcpy(x->client, sizeof x->client, s, "Client");
        jcpy(x->type, sizeof x->type, npi, "Type");
        jcpy(x->title, sizeof x->title, npi, "Name");
        jcpy(x->series, sizeof x->series, npi, "SeriesName");
        jcpy(x->album, sizeof x->album, npi, "Album");
        x->season = (int)jnum(npi, "ParentIndexNumber");
        x->episode = (int)jnum(npi, "IndexNumber");
        x->year = (int)jnum(npi, "ProductionYear");
        x->dur = (long long)jnum(npi, "RunTimeTicks");
        x->pos = (long long)jnum(ps, "PositionTicks");
        x->paused = jnum(ps, "IsPaused") != 0;

        /* Artists: join array; fall back to AlbumArtist */
        J *arts = jget(npi, "Artists");
        if (arts && arts->t == J_ARR) {
            for (J *a = arts->kids; a; a = a->next) {
                if (a->t != J_STR) continue;
                size_t l = strlen(x->artist);
                snprintf(x->artist + l, sizeof x->artist - l, "%s%s", l ? ", " : "", a->s);
            }
        }
        if (!x->artist[0]) jcpy(x->artist, sizeof x->artist, npi, "AlbumArtist");
    }
}

static DWORD WINAPI poll_thread(LPVOID arg)
{
    (void)arg;
    HINTERNET hs = WinHttpOpen(L"LCDSmartie-Jellyfin/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) /* pre-8.1 Windows lacks AUTOMATIC_PROXY */
        hs = WinHttpOpen(L"LCDSmartie-Jellyfin/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { set_err("WinHttpOpen failed"); return 1; }
    WinHttpSetTimeouts(hs, 3000, 3000, 3000, 3000);
    /* Windows 7's WinHTTP defaults to SSL3/TLS1.0 only. Ask for 1.1/1.2 too;
       this fails harmlessly on a Win7 without KB3140245 (then https servers
       that require TLS 1.2 won't work there, plain http is unaffected). */
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                   WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hs, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);

    static Sess tmp[MAX_SESSIONS];
    for (;;) {
        /* Re-read config every poll so edits apply without restarting Smartie. */
        WCHAR server[512], apikey[256];
        GetPrivateProfileStringW(L"Jellyfin", L"Server", L"", server, 512, g_inipath);
        GetPrivateProfileStringW(L"Jellyfin", L"ApiKey", L"", apikey, 256, g_inipath);
        int interval = GetPrivateProfileIntW(L"Jellyfin", L"PollInterval", 2000, g_inipath);
        if (interval < 500) interval = 500;

        char idle[128], deff[96], bare[8];
        ini_str(L"IdleText", L"", idle, sizeof idle);
        ini_str(L"DefaultFilter", L"", deff, sizeof deff);
        ini_str(L"BarEmpty", L"_", bare, sizeof bare);
        EnterCriticalSection(&g_cs);
        strcpy(g_idle, idle);
        strcpy(g_deffilter, deff);
        strcpy(g_barempty, bare[0] ? bare : " ");
        LeaveCriticalSection(&g_cs);

        if (!server[0] || !apikey[0]) {
            set_err("Set Server and ApiKey in plugins\\jellyfin.ini");
        } else {
            /* Commands first. Then give the client a moment to act on them, so
               the poll below sees the new state instead of undoing the
               optimistic update function5 made. */
            if (send_cmds(hs, server, apikey) &&
                WaitForSingleObject(g_stop, 600) == WAIT_OBJECT_0) break;
            char err[256];
            char *body = http_req(hs, server, apikey, L"GET",
                                  L"/Sessions?ActiveWithinSeconds=120", err, sizeof err);
            if (!body) set_err("%s", err);
            if (body) {
                const char *p = body;
                J *root = jparse_val(&p, 0);
                if (root && root->t == J_ARR) {
                    int n;
                    parse_sessions(root, tmp, &n);
                    ULONGLONG now = GetTickCount64();
                    for (int i = 0; i < n; i++) tmp[i].at = now;
                    EnterCriticalSection(&g_cs);
                    memcpy(g_sess, tmp, sizeof(Sess) * n);
                    g_nsess = n;
                    g_havedata = 1;
                    g_err[0] = 0;
                    LeaveCriticalSection(&g_cs);
                } else {
                    set_err("Unexpected response from server");
                }
                jfree(root);
                free(body);
            }
        }
        /* Sleep until the next poll, or until function5 queues a command. */
        HANDLE ev[2] = { g_stop, g_wake };
        if (WaitForMultipleObjects(2, ev, FALSE, interval) == WAIT_OBJECT_0) break;
    }
    WinHttpCloseHandle(hs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int found;          /* a session matched */
    Sess s;
    int count;          /* sessions matching the filter */
    char err[256];
    char cmderr[256];   /* recent remote-control failure */
    char idle[128];
    char barempty[8];
} View;

static int is_blank(const char *s) { return !s || !*s || !strcmp(s, "0"); }

static int sess_match(const Sess *s, const char *f)
{
    return !_stricmp(s->user, f) || !_stricmp(s->device, f) || !_stricmp(s->client, f);
}

/* Index of the session to show for filter (matching, playing preferred over
   paused), or -1. Call with g_cs held. */
static int pick_locked(const char *filter, int *count)
{
    const char *f = is_blank(filter) ? g_deffilter : filter;
    int best = -1, n = 0;
    for (int i = 0; i < g_nsess; i++) {
        if (*f && !sess_match(&g_sess[i], f)) continue;
        n++;
        if (best < 0 || (g_sess[best].paused && !g_sess[i].paused)) best = i;
    }
    if (count) *count = n;
    return best;
}

/* Snapshot the session to display. */
static void get_view(const char *filter, View *v)
{
    memset(v, 0, sizeof *v);
    EnterCriticalSection(&g_cs);
    int best = pick_locked(filter, &v->count);
    if (best >= 0) { v->found = 1; v->s = g_sess[best]; }
    if (!g_havedata || g_err[0]) strcpy(v->err, g_err);
    if (g_cmderr[0] && GetTickCount64() - g_cmderr_at < CMDERR_SHOW_MS) strcpy(v->cmderr, g_cmderr);
    strcpy(v->idle, g_idle);
    strcpy(v->barempty, g_barempty);
    LeaveCriticalSection(&g_cs);
}

/* Position, advanced by the time since it was last known while playing. */
static long long sess_pos(const Sess *s)
{
    long long p = s->pos;
    if (!s->paused)
        p += (long long)(GetTickCount64() - s->at) * 10000LL;
    if (s->dur > 0 && p > s->dur) p = s->dur;
    return p < 0 ? 0 : p;
}

static long long cur_pos(const View *v) { return sess_pos(&v->s); }

/* Time styles, picked with a suffix on the field name (pos_mm, time_hms, ...):
 *   TS_AUTO  (none)  M:SS, or H:MM:SS from 1 hour     1:30   1:05:09
 *   TS_MM    _mm     MM:SS, minutes keep counting      01:30  65:09
 *   TS_M     _m      M:SS,  minutes keep counting      1:30   65:09
 *   TS_HMS   _hms    H:MM:SS always                    0:01:30
 *   TS_HHMMSS _hhmmss HH:MM:SS always                  00:01:30
 */
enum { TS_AUTO, TS_MM, TS_M, TS_HMS, TS_HHMMSS };

static void fmt_time(char *o, size_t sz, long long ticks, int style)
{
    long long t = (ticks < 0 ? 0 : ticks) / TICKS_PER_SEC;
    long long h = t / 3600, m = (t / 60) % 60, sec = t % 60;
    switch (style) {
    case TS_MM:     snprintf(o, sz, "%02lld:%02lld", t / 60, sec); break;
    case TS_M:      snprintf(o, sz, "%lld:%02lld", t / 60, sec); break;
    case TS_HMS:    snprintf(o, sz, "%lld:%02lld:%02lld", h, m, sec); break;
    case TS_HHMMSS: snprintf(o, sz, "%02lld:%02lld:%02lld", h, m, sec); break;
    default:
        if (h) snprintf(o, sz, "%lld:%02lld:%02lld", h, m, sec);
        else snprintf(o, sz, "%lld:%02lld", m, sec);
    }
}

/* Time fields: pos dur remain time timeleft, with an optional style suffix.
 * Returns 0 if name isn't one of them. */
static int time_field(const View *v, const char *name, char *o, size_t sz)
{
    static const char *bases[] = { "pos", "dur", "remain", "time", "timeleft" };
    static const struct { const char *sfx; int style; } styles[] = {
        { "", TS_AUTO }, { "_mm", TS_MM }, { "_m", TS_M }, { "_hms", TS_HMS }, { "_hhmmss", TS_HHMMSS } };
    int base = -1, style = 0;
    for (int b = 0; b < 5 && base < 0; b++) {
        size_t bl = strlen(bases[b]);
        if (_strnicmp(name, bases[b], bl)) continue;
        for (int i = 0; i < 5; i++)
            if (!_stricmp(name + bl, styles[i].sfx)) { base = b; style = styles[i].style; break; }
    }
    if (base < 0) return 0;

    o[0] = 0;
    if (!v->found) return 1;
    const Sess *s = &v->s;
    int hasdur = s->dur > 0;
    char p[32], d[32], r[32];
    fmt_time(p, sizeof p, cur_pos(v), style);
    fmt_time(d, sizeof d, s->dur, style);
    fmt_time(r + 1, sizeof r - 1, s->dur - cur_pos(v), style);
    r[0] = '-';

    switch (base) {
    case 0: snprintf(o, sz, "%s", p); break;
    case 1: if (hasdur) snprintf(o, sz, "%s", d); break;
    case 2: if (hasdur) snprintf(o, sz, "%s", r); break;
    case 3: if (hasdur) snprintf(o, sz, "%s/%s", p, d); else snprintf(o, sz, "%s", p); break;
    case 4: if (hasdur) snprintf(o, sz, "%s/%s", r, d); break;
    }
    return 1;
}

static int pct(const View *v)
{
    if (v->s.dur <= 0) return 0;
    return (int)(cur_pos(v) * 100 / v->s.dur);
}

/* Write the value of a named field into o. Returns 0 for unknown field. */
static int field_value(const View *v, const char *name, char *o, size_t sz)
{
    const Sess *s = &v->s;
    o[0] = 0;

    if (!_stricmp(name, "state")) {
        snprintf(o, sz, "%s", !v->found ? "Stopped" : s->paused ? "Paused" : "Playing");
        return 1;
    }
    if (!_stricmp(name, "count")) { snprintf(o, sz, "%d", v->count); return 1; }
    if (!_stricmp(name, "error")) { snprintf(o, sz, "%s", v->err[0] ? v->err : v->cmderr); return 1; }

    /* Everything below is per-session; empty when nothing is playing. */
    int known = 1;
    if (!_stricmp(name, "title")) snprintf(o, sz, "%s", s->title);
    else if (!_stricmp(name, "artist")) snprintf(o, sz, "%s", s->artist);
    else if (!_stricmp(name, "album")) snprintf(o, sz, "%s", s->album);
    else if (!_stricmp(name, "series")) snprintf(o, sz, "%s", s->series);
    else if (!_stricmp(name, "season")) { if (s->season) snprintf(o, sz, "%d", s->season); }
    else if (!_stricmp(name, "episode")) { if (s->episode) snprintf(o, sz, "%d", s->episode); }
    else if (!_stricmp(name, "se")) { if (s->season || s->episode) snprintf(o, sz, "S%02dE%02d", s->season, s->episode); }
    else if (!_stricmp(name, "year")) { if (s->year) snprintf(o, sz, "%d", s->year); }
    else if (!_stricmp(name, "type")) snprintf(o, sz, "%s", s->type);
    else if (!_stricmp(name, "user")) snprintf(o, sz, "%s", s->user);
    else if (!_stricmp(name, "device")) snprintf(o, sz, "%s", s->device);
    else if (!_stricmp(name, "client")) snprintf(o, sz, "%s", s->client);
    else if (time_field(v, name, o, sz)) ;
    else if (!_stricmp(name, "pct")) { if (v->found) snprintf(o, sz, "%d", pct(v)); }
    else if (!_stricmp(name, "icon")) {
        /* simple ASCII play/pause indicator */
        snprintf(o, sz, "%s", !v->found ? "" : s->paused ? "||" : ">");
    }
    else if (!_stricmp(name, "display")) {
        if (!v->found) snprintf(o, sz, "%s", v->err[0] ? v->err : v->idle);
        else if (!_stricmp(s->type, "Episode") && s->series[0])
            snprintf(o, sz, "%s S%02dE%02d %s", s->series, s->season, s->episode, s->title);
        else if (!_stricmp(s->type, "Audio") && s->artist[0])
            snprintf(o, sz, "%s - %s", s->artist, s->title);
        else if (!_stricmp(s->type, "Movie") && s->year)
            snprintf(o, sz, "%s (%d)", s->title, s->year);
        else snprintf(o, sz, "%s", s->title);
    }
    else known = 0;

    if (!v->found && _stricmp(name, "display")) o[0] = 0;
    return known;
}

/* ------------------------------------------------------------------ */
/* Exports                                                             */
/* ------------------------------------------------------------------ */

static void start_thread(void)
{
    EnterCriticalSection(&g_cs);
    if (!g_thread) {
        ensure_ini();
        g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_thread = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
    }
    LeaveCriticalSection(&g_cs);
}

DLLEXPORT void __stdcall SmartieInit(void) { start_thread(); }

DLLEXPORT void __stdcall SmartieFini(void)
{
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    CloseHandle(g_wake);
    g_thread = g_stop = g_wake = NULL;
}

DLLEXPORT int __stdcall GetMinRefreshInterval(void) { return 250; }

DLLEXPORT char * __stdcall SmartieInfo(void)
{
    return "Developer:Spooky098529\r\nVersion:1.1.0\r\n"
           "Shows Jellyfin now-playing info. Configure plugins\\jellyfin.ini\r\n"
           "https://github.com/Spooky098529/lcdsmartie-jellyfin";
}

DLLEXPORT char * __stdcall SmartieDemo(void)
{
    return "Smart now-playing line\r\n"
           "$dll(jellyfin,1,display,)\r\n"
           "Single field (title artist album series se year user device client state pos dur remain time timeleft pct icon count)\r\n"
           "$dll(jellyfin,1,title,)\r\n"
           "Only a specific user/device/client\r\n"
           "$dll(jellyfin,1,title,alice)\r\n"
           "Time as MM:SS/MM:SS (suffixes: _mm _m _hms _hhmmss on pos dur remain time timeleft)\r\n"
           "$dll(jellyfin,1,time_mm,)\r\n"
           "Progress bar, 20 wide\r\n"
           "$dll(jellyfin,2,20,)\r\n"
           "Number of active sessions\r\n"
           "$dll(jellyfin,3,,)\r\n"
           "Custom format\r\n"
           "$dll(jellyfin,4,{icon} {artist} - {title},)\r\n"
           "Remote control, for Actions on LCD buttons (PlayPause Stop NextTrack PreviousTrack Seek+10 Seek-10 VolumeUp VolumeDown ToggleMute)\r\n"
           "$dll(jellyfin,5,PlayPause,)\r\n"
           "Last error\r\n"
           "$dll(jellyfin,20,,)";
}

/* 1: single field */
DLLEXPORT char * __stdcall function1(char *param1, char *param2)
{
    View v;
    start_thread();
    get_view(param2, &v);
    if (!field_value(&v, param1 ? param1 : "", t_out, OUTSZ))
        snprintf(t_out, OUTSZ, "[jellyfin: unknown field '%s']", param1 ? param1 : "");
    return t_out;
}

/* 2: progress bar of width param1 */
DLLEXPORT char * __stdcall function2(char *param1, char *param2)
{
    View v;
    start_thread();
    get_view(param2, &v);
    int w = param1 ? atoi(param1) : 0;
    if (w <= 0) w = 10;
    if (w > 100) w = 100;
    int filled = v.found ? (pct(&v) * w + 50) / 100 : 0;
    char *o = t_out;
    for (int i = 0; i < w; i++) {
        /* UTF-8 full block; Smartie maps it to the LCD's solid block (0xFF) */
        const char *c = i < filled ? "\xE2\x96\x88" : v.barempty;
        size_t l = strlen(c);
        if (o + l >= t_out + OUTSZ) break;
        memcpy(o, c, l);
        o += l;
    }
    *o = 0;
    return t_out;
}

/* 3: count of sessions playing (optionally filtered by param1) */
DLLEXPORT char * __stdcall function3(char *param1, char *param2)
{
    View v;
    (void)param2;
    start_thread();
    get_view(param1, &v);
    snprintf(t_out, OUTSZ, "%d", v.count);
    return t_out;
}

/* 4: template with {field} placeholders */
DLLEXPORT char * __stdcall function4(char *param1, char *param2)
{
    View v;
    start_thread();
    get_view(param2, &v);
    if (!v.found) {
        snprintf(t_out, OUTSZ, "%s", v.err[0] ? v.err : v.idle);
        return t_out;
    }
    const char *p = param1 ? param1 : "";
    char *o = t_out, *end = t_out + OUTSZ - 1;
    while (*p && o < end) {
        const char *close;
        if (*p == '{' && (close = strchr(p, '}')) && close - p < 32) {
            char name[32], val[512];
            memcpy(name, p + 1, close - p - 1);
            name[close - p - 1] = 0;
            if (field_value(&v, name, val, sizeof val)) {
                size_t l = strlen(val);
                if (l > (size_t)(end - o)) l = end - o;
                memcpy(o, val, l);
                o += l;
                p = close + 1;
                continue;
            }
        }
        *o++ = *p++;
    }
    *o = 0;
    return t_out;
}

/* 5: remote control. Meant for Actions (e.g. on LCD buttons):
 *   $MObutton(A) = 1  ->  $dll(jellyfin,5,PlayPause,)
 * Targets the session the display shows for <filter>. Returns "" because
 * Smartie runs a $dll action's result as another action. */
DLLEXPORT char * __stdcall function5(char *param1, char *param2)
{
    static const struct { const char *name; const char *path; int effect; } cmds[] = {
        /* effect: 1 toggle pause, 2 pause, 3 unpause (applied to the cache at once) */
        { "PlayPause",     "Playing/PlayPause",     1 },
        { "Pause",         "Playing/Pause",         2 },
        { "Unpause",       "Playing/Unpause",       3 },
        { "Play",          "Playing/Unpause",       3 },
        { "Stop",          "Playing/Stop",          0 },
        { "NextTrack",     "Playing/NextTrack",     0 },
        { "Next",          "Playing/NextTrack",     0 },
        { "PreviousTrack", "Playing/PreviousTrack", 0 },
        { "Previous",      "Playing/PreviousTrack", 0 },
        { "Prev",          "Playing/PreviousTrack", 0 },
        { "VolumeUp",      "Command/VolumeUp",      0 },
        { "VolumeDown",    "Command/VolumeDown",    0 },
        { "ToggleMute",    "Command/ToggleMute",    0 },
        { "Mute",          "Command/Mute",          0 },
        { "Unmute",        "Command/Unmute",        0 },
    };
    const char *name = param1 ? param1 : "";
    int ci = -1;
    long seek = 0;
    t_out[0] = 0;
    start_thread();

    for (int i = 0; i < (int)(sizeof cmds / sizeof *cmds); i++)
        if (!_stricmp(name, cmds[i].name)) { ci = i; break; }
    if (ci < 0) {
        /* Seek+N / Seek-N: relative seek by N seconds */
        char *end;
        if (_strnicmp(name, "Seek", 4) || !name[4] ||
            (seek = strtol(name + 4, &end, 10), *end) || !seek) {
            set_cmderr("Unknown command '%s'", name);
            return t_out;
        }
    }

    EnterCriticalSection(&g_cs);
    int i = pick_locked(param2, NULL);
    if (i < 0) {
        LeaveCriticalSection(&g_cs);
        set_cmderr("%s: nothing playing", name);
        return t_out;
    }
    Sess *s = &g_sess[i];
    if (!s->remote) {
        char client[64];
        strcpy(client, s->client);
        LeaveCriticalSection(&g_cs);
        set_cmderr("%s: %s has no remote control", name, client);
        return t_out;
    }
    if (g_qlen == MAX_CMDS) {
        LeaveCriticalSection(&g_cs);
        set_cmderr("%s: too many queued commands", name);
        return t_out;
    }

    Cmd *c = &g_q[(g_qhead + g_qlen) % MAX_CMDS];
    WCHAR wid[64];
    MultiByteToWideChar(CP_UTF8, 0, s->id, -1, wid, 64);
    snprintf(c->name, sizeof c->name, "%s", name);

    /* Update the cache now so the display reacts before the next poll. */
    ULONGLONG now = GetTickCount64();
    if (ci < 0) {
        long long target = sess_pos(s) + (long long)seek * TICKS_PER_SEC;
        if (target < 0) target = 0;
        if (s->dur > 0 && target > s->dur) target = s->dur;
        _snwprintf(c->path, 256, L"/Sessions/%ls/Playing/Seek?SeekPositionTicks=%lld", wid, target);
        s->pos = target;
        s->at = now;
    } else {
        WCHAR wpath[32];
        MultiByteToWideChar(CP_UTF8, 0, cmds[ci].path, -1, wpath, 32);
        _snwprintf(c->path, 256, L"/Sessions/%ls/%ls", wid, wpath);
        if (cmds[ci].effect) {
            s->pos = sess_pos(s);
            s->at = now;
            s->paused = cmds[ci].effect == 1 ? !s->paused : cmds[ci].effect == 2;
        }
    }
    c->path[255] = 0;
    g_qlen++;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);
    return t_out;
}

/* 20: status / last error */
DLLEXPORT char * __stdcall function20(char *param1, char *param2)
{
    View v;
    (void)param1; (void)param2;
    start_thread();
    get_view(NULL, &v);
    snprintf(t_out, OUTSZ, "%s", v.err[0] ? v.err : v.cmderr[0] ? v.cmderr : "OK");
    return t_out;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        /* jellyfin.ini lives next to jellyfin.dll */
        GetModuleFileNameW(h, g_inipath, MAX_PATH);
        WCHAR *dot = wcsrchr(g_inipath, L'.');
        if (dot) wcscpy(dot, L".ini");
    }
    return TRUE;
}
