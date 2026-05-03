/*
 * onopkg - Package manager for custom distro, borrowing Arch repos
 *
 * Usage:
 *   onopkg <package>        Download package + dependencies
 *   onopkg --sync           Scan system and populate installed DB
 *   onopkg -R <package>     Remove a package from installed DB
 *   onopkg --list           List all packages in installed DB
 *
 * Installed DB: ~/.local/share/onopkg/installed.txt
 * DB cache:     ~/.cache/onopkg/  (1-hour TTL)
 *
 * Requires: libcurl, zlib, libarchive, pthreads
 * Build:    gcc -O2 -o onopkg onopkg.c -lcurl -lz -larchive -lm -lpthread
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dirent.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <curl/curl.h>
#include <zlib.h>
#include <archive.h>
#include <archive_entry.h>

/* ──────────────────────────── colours ──────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_CYAN    "\033[36m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_RED     "\033[31m"
#define C_MAGENTA "\033[35m"

/* ──────────────────────────── tuning ───────────────────────────── */
#define MAX_MIRRORS       256
#define MAX_MATCHES        64
#define URL_MAX           512
#define PKG_NAME_MAX      128
#define PING_THREADS       64
#define FETCH_THREADS      32
#define PING_TIMEOUT_MS  2000
#define FETCH_TIMEOUT_MS 6000
#define DB_CACHE_TTL_SEC 3600   /* re-download repo DBs after 1 hour */
#define MAX_DEPS           64   /* max direct deps per package       */
#define MAX_PKG_QUEUE    1024   /* recursive download queue          */

#define MIRRORLIST_URL \
    "https://archlinux.org/mirrorlist/?country=all&protocol=https" \
    "&ip_version=4&use_mirror_status=on"

static const char *REPOS[] = {
    "core/os/x86_64/core.db",
    "extra/os/x86_64/extra.db",
    "multilib/os/x86_64/multilib.db",
    NULL
};
#define REPO_COUNT 3

/*
 * Directories to scan during --sync.
 * Covers both legacy paths (/bin, /lib) and modern (/usr/bin, /usr/lib).
 */
static const char *SCAN_DIRS[] = {
    "/bin", "/sbin",
    "/usr/bin", "/usr/sbin",
    "/lib", "/lib64",
    "/usr/lib", "/usr/lib64",
    "/usr/local/bin", "/usr/local/lib",
    NULL
};

/* ──────────────────────────── types ────────────────────────────── */
typedef struct {
    char   url[URL_MAX];
    char   host[256];
    double latency_ms;
    bool   reachable;
    bool   has_package;
    char   matched_pkg[PKG_NAME_MAX];
    char   matched_repo[32];
} Mirror;

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} Buf;

typedef struct WorkItem WorkItem;
struct WorkItem {
    void     (*fn)(void *);
    void      *arg;
    WorkItem  *next;
};

typedef struct {
    WorkItem       *head, *tail;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             workers;
    atomic_int      pending;
    bool            shutdown;
} Pool;

/* ──────────────────────────── globals ──────────────────────────── */
static Mirror  mirrors[MAX_MIRRORS];
static int     mirror_count = 0;

static Mirror *matches[MAX_MATCHES];
static int     match_count = 0;
static pthread_mutex_t match_mu = PTHREAD_MUTEX_INITIALIZER;

static atomic_int g_ping_done  = 0;
static atomic_int g_fetch_done = 0;
static int        g_ping_total  = 0;
static int        g_fetch_total = 0;
static pthread_mutex_t g_print_mu = PTHREAD_MUTEX_INITIALIZER;

/* in-session queue: avoids processing the same package twice */
static char     session_done[MAX_PKG_QUEUE][PKG_NAME_MAX];
static int      session_done_count = 0;
static pthread_mutex_t session_mu = PTHREAD_MUTEX_INITIALIZER;

/* per-package file tracking: /var/lib/onopkg/<pkg>/files.txt */
#define PKGDB_DIR "/var/lib/onopkg"

/* set true by main when -S is active */
static bool g_do_install = false;

/* forward declarations (defined later, after install/remove section) */
static int install_pkg(const char *pkg_name, const char *tarball_path,
                       char deps[][PKG_NAME_MAX], int dep_count);
static int remove_pkg(const char *pkg_name);
static int remove_pkg_with_deps(const char *pkg_name);

/* ═══════════════════════════════════════════════════════════════════
 * Installed-package database
 * File: ~/.local/share/onopkg/installed.txt
 * Format: one package name per line (plain text, no versions)
 * ═══════════════════════════════════════════════════════════════════ */

static bool db_path(char *out, size_t len) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dir[512];
    snprintf(dir, sizeof dir, "%s/.local", home);        mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.local/share", home);  mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.local/share/onopkg", home); mkdir(dir, 0755);
    snprintf(out, len, "%s/.local/share/onopkg/installed.txt", home);
    return true;
}

/*
 * Normalize a raw filename/soname into a package-like name:
 *   libz.so.1.2.11  →  libz
 *   libcurl.so.4    →  libcurl
 *   curl            →  curl   (unchanged)
 */
static void normalize_name(const char *raw, char *out, size_t out_len) {
    strncpy(out, raw, out_len - 1);
    out[out_len - 1] = '\0';

    /* strip .so and anything after it */
    char *so = strstr(out, ".so");
    if (so) *so = '\0';

    /* strip any trailing version like -1.2.3 */
    char *dash = strrchr(out, '-');
    if (dash) {
        /* only strip if what follows looks like a version (starts with digit) */
        if (dash[1] >= '0' && dash[1] <= '9') *dash = '\0';
    }
}

/* Returns true if pkg_name (or a fuzzy prefix match) is in installed.txt */
static bool is_installed(const char *pkg_name) {
    char path[512];
    if (!db_path(path, sizeof path)) return false;

    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char norm_query[PKG_NAME_MAX];
    normalize_name(pkg_name, norm_query, sizeof norm_query);
    size_t qlen = strlen(norm_query);

    char line[PKG_NAME_MAX];
    bool found = false;
    while (fgets(line, sizeof line, fp)) {
        /* strip newline */
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
            line[--ll] = '\0';
        if (ll == 0) continue;

        char norm_line[PKG_NAME_MAX];
        normalize_name(line, norm_line, sizeof norm_line);

        /* exact match OR installed entry starts with query (e.g. "curl" matches "curl") */
        if (strcmp(norm_line, norm_query) == 0 ||
            (strncmp(norm_line, norm_query, qlen) == 0 &&
             (norm_line[qlen] == '\0' || norm_line[qlen] == '-'))) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* Append pkg_name to installed.txt (no-op if already present) */
static void db_add(const char *pkg_name) {
    if (is_installed(pkg_name)) return;

    char path[512];
    if (!db_path(path, sizeof path)) return;

    FILE *fp = fopen(path, "a");
    if (!fp) return;
    fprintf(fp, "%s\n", pkg_name);
    fclose(fp);
}

/* Remove pkg_name from installed.txt */
static int db_remove(const char *pkg_name) {
    char path[512];
    if (!db_path(path, sizeof path)) return 1;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, C_RED "  error:" C_RESET
                " installed.txt not found. Run --sync first.\n");
        return 1;
    }

    /* read all lines except the one to remove */
    char  **lines   = NULL;
    size_t  n_lines = 0, cap = 0;
    char    line[PKG_NAME_MAX];
    bool    removed = false;

    while (fgets(line, sizeof line, fp)) {
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
            line[--ll] = '\0';
        if (ll == 0) continue;

        if (strcmp(line, pkg_name) == 0) { removed = true; continue; }

        if (n_lines >= cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, cap * sizeof *lines);
        }
        lines[n_lines++] = strdup(line);
    }
    fclose(fp);

    if (!removed) {
        fprintf(stderr, C_YELLOW "  warn:" C_RESET
                " '%s' not found in installed DB.\n", pkg_name);
        for (size_t i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        return 1;
    }

    fp = fopen(path, "w");
    if (!fp) {
        for (size_t i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        return 1;
    }
    for (size_t i = 0; i < n_lines; i++) {
        fprintf(fp, "%s\n", lines[i]);
        free(lines[i]);
    }
    free(lines);
    fclose(fp);

    printf(C_GREEN "  ::" C_RESET " Removed " C_BOLD "%s" C_RESET
           " from installed DB.\n", pkg_name);
    return 0;
}

/* Print all entries in installed.txt */
static int db_list(void) {
    char path[512];
    if (!db_path(path, sizeof path)) return 1;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, C_YELLOW "  ::" C_RESET
                " No installed DB found. Run --sync first.\n");
        return 1;
    }

    char line[PKG_NAME_MAX];
    int  count = 0;
    printf(C_BOLD "  :: Installed packages:\n" C_RESET "\n");
    while (fgets(line, sizeof line, fp)) {
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
            line[--ll] = '\0';
        if (ll == 0) continue;
        printf("     %s\n", line);
        count++;
    }
    fclose(fp);
    printf("\n  " C_BOLD "%d" C_RESET " package(s) total.\n\n", count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * --sync: scan filesystem and populate installed DB
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Packages that ship with virtually every Linux system.
 * Seeding these prevents onopkg from re-downloading fundamental deps
 * that are already present but whose names never appear as filenames.
 */
static const char *BASELINE_PKGS[] = {
    /* C runtime / dynamic linker */
    "glibc", "musl", "gcc-libs",
    /* compression */
    "zlib", "xz", "bzip2", "zstd", "lz4",
    /* crypto / TLS */
    "openssl", "ca-certificates", "ca-certificates-utils",
    "libgcrypt", "libgpg-error", "p11-kit",
    /* core utils */
    "coreutils", "bash", "dash", "sh", "busybox",
    "util-linux", "findutils", "grep", "gawk", "sed",
    "tar", "gzip", "file",
    /* system libs everyone has */
    "pam", "libcap", "libacl", "attr",
    "systemd-libs", "dbus", "expat", "libffi",
    "pcre", "pcre2",
    /* networking */
    "curl", "libcurl", "wget",
    "iputils", "iproute2", "net-tools",
    /* filesystems / init */
    "e2fsprogs", "shadow", "sysfsutils",
    /* package-adjacent */
    "archlinux-keyring", "pacman-mirrorlist",
    NULL
};

static int cmd_sync(void) {
    char db[512];
    db_path(db, sizeof db);

    printf(C_BOLD "  ::" C_RESET " Scanning system for installed packages…\n\n");

    /* wipe and rebuild */
    FILE *fp = fopen(db, "w");
    if (!fp) {
        fprintf(stderr, C_RED "  error:" C_RESET
                " Cannot open %s for writing: %s\n", db, strerror(errno));
        return 1;
    }

    /* seed with universal baseline packages */
    int total = 0;
    for (int i = 0; BASELINE_PKGS[i] != NULL; i++) {
        fprintf(fp, "%s\n", BASELINE_PKGS[i]);
        total++;
    }
    fclose(fp);

    printf("  " C_DIM "Seeded %d baseline packages." C_RESET "\n\n", total);
    total = 0;

    for (int di = 0; SCAN_DIRS[di] != NULL; di++) {
        DIR *d = opendir(SCAN_DIRS[di]);
        if (!d) continue;   /* directory may not exist on this distro */

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            char norm[PKG_NAME_MAX];
            normalize_name(ent->d_name, norm, sizeof norm);
            if (norm[0] == '\0') continue;

            /* skip generic noise */
            if (strcmp(norm, "lib") == 0 ||
                strcmp(norm, "lib64") == 0) continue;

            if (!is_installed(norm)) {
                FILE *f = fopen(db, "a");
                if (f) { fprintf(f, "%s\n", norm); fclose(f); }
                total++;
            }
        }
        closedir(d);
        printf("     Scanned " C_CYAN "%s" C_RESET "\n", SCAN_DIRS[di]);
    }

    printf("\n" C_GREEN C_BOLD "  :: Sync complete." C_RESET
           " Added " C_BOLD "%d" C_RESET " entries to installed DB.\n"
           "     Location: " C_DIM "%s" C_RESET "\n\n", total, db);
    return 0;
}

/* ─────────────────────── dynamic buffer ───────────────────────── */
static Buf *buf_new(void) {
    Buf *b  = calloc(1, sizeof *b);
    b->cap  = 65536;
    b->data = malloc(b->cap);
    return b;
}

static void buf_free(Buf *b) {
    if (b) { free(b->data); free(b); }
}

static size_t curl_write_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    Buf   *b   = ud;
    size_t len = sz * nmemb;
    if (b->size + len + 1 > b->cap) {
        while (b->cap < b->size + len + 1) b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->size, ptr, len);
    b->size += len;
    b->data[b->size] = 0;
    return len;
}

/* ─────────────────────── HTTP fetch ────────────────────────────── */
static Buf *http_fetch(const char *url, long timeout_ms) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    Buf *buf = buf_new();

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "onopkg/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { buf_free(buf); return NULL; }
    return buf;
}

/* ─────────────────── extract host from URL ─────────────────────── */
static void extract_host(const char *url, char *host, size_t hlen) {
    const char *p = strstr(url, "://");
    if (!p) { strncpy(host, url, hlen - 1); return; }
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n >= hlen) n = hlen - 1;
    memcpy(host, p, n);
    host[n] = 0;
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;
}

/* ─────────────────── parse mirrorlist ──────────────────────────── */
static void parse_mirrorlist(const char *text) {
    const char *p = text;
    while ((p = strstr(p, "Server = ")) != NULL) {
        p += 9;
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        size_t len = (size_t)(end - p);
        if (len < 10 || mirror_count >= MAX_MIRRORS) { p = end; continue; }

        Mirror *m = &mirrors[mirror_count];
        memset(m, 0, sizeof *m);

        char raw[URL_MAX];
        if (len >= URL_MAX) len = URL_MAX - 1;
        memcpy(raw, p, len);
        raw[len] = 0;

        char *dollar = strstr(raw, "$repo");
        if (dollar) *dollar = 0;
        size_t rlen = strlen(raw);
        if (rlen > 0 && raw[rlen - 1] != '/') { raw[rlen] = '/'; raw[rlen + 1] = 0; }

        strncpy(m->url, raw, URL_MAX - 1);
        extract_host(raw, m->host, sizeof m->host);
        mirror_count++;
        p = end;
    }
}

/* ──────────────── TCP-connect latency (ms) ─────────────────────── */
static double tcp_ping(const char *host, int port, int timeout_ms) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1.0;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1.0; }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    connect(fd, res->ai_addr, res->ai_addrlen);

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv = { .tv_sec  = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };

    double latency = -1.0;
    if (select(fd + 1, NULL, &wset, NULL, &tv) == 1) {
        int err = 0; socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            latency = (t1.tv_sec - t0.tv_sec) * 1000.0
                    + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        }
    }
    close(fd);
    freeaddrinfo(res);
    return latency;
}

/* ───────────────────── desc field helpers ──────────────────────── */

static bool desc_get_field(const char *desc, size_t desc_len,
                            const char *key,
                            char *out_val, size_t out_len) {
    size_t klen = strlen(key);
    const char *p   = desc;
    const char *end = desc + desc_len;
    while (p < end) {
        if ((size_t)(end - p) >= klen && memcmp(p, key, klen) == 0) {
            p += klen;
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            const char *eol = p;
            while (eol < end && *eol != '\n' && *eol != '\r') eol++;
            size_t vlen = (size_t)(eol - p);
            if (vlen > 0 && vlen < out_len) {
                memcpy(out_val, p, vlen);
                out_val[vlen] = 0;
                return true;
            }
            return false;
        }
        while (p < end && *p != '\n') p++;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
    return false;
}

static int desc_get_depends(const char *desc, size_t desc_len,
                             char deps[][PKG_NAME_MAX], int max_deps) {
    const char *p   = desc;
    const char *end = desc + desc_len;
    int count = 0;
    while (p < end) {
        if ((size_t)(end - p) >= 10 && memcmp(p, "%DEPENDS%\n", 10) == 0) {
            p += 10;
            while (p < end && *p != '%') {
                const char *eol = p;
                while (eol < end && *eol != '\n' && *eol != '\r') eol++;
                size_t len = (size_t)(eol - p);
                if (len > 0 && count < max_deps) {
                    size_t nlen = len;
                    for (size_t i = 0; i < len; i++) {
                        if (p[i] == '<' || p[i] == '>' || p[i] == '=') {
                            nlen = i; break;
                        }
                    }
                    if (nlen >= PKG_NAME_MAX) nlen = PKG_NAME_MAX - 1;
                    memcpy(deps[count], p, nlen);
                    deps[count][nlen] = '\0';
                    count++;
                }
                while (p < end && *p != '\n') p++;
                while (p < end && (*p == '\n' || *p == '\r')) p++;
            }
            break;
        }
        while (p < end && *p != '\n') p++;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
    return count;
}

/* ─────────────────────── libarchive helpers ─────────────────────── */

static char *archive_read_entry_text(struct archive *a, size_t *out_len) {
    size_t  cap   = 4096;
    char   *buf   = malloc(cap);
    size_t  total = 0;
    char    chunk[4096];
    ssize_t n;
    while ((n = archive_read_data(a, chunk, sizeof chunk)) > 0) {
        if (total + (size_t)n + 1 > cap) {
            while (cap < total + (size_t)n + 1) cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + total, chunk, (size_t)n);
        total += (size_t)n;
    }
    buf[total] = 0;
    if (out_len) *out_len = total;
    return buf;
}

static bool gunzip_buf(const uint8_t *src, size_t src_len,
                       uint8_t **out, size_t *out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;

    size_t   cap = src_len > 0 ? src_len * 2 : 65536;
    uint8_t *buf = malloc(cap);
    if (!buf) { inflateEnd(&zs); return false; }

    zs.next_in   = (Bytef *)src;
    zs.avail_in  = (uInt)src_len;
    zs.next_out  = buf;
    zs.avail_out = (uInt)cap;

    int rc;
    while ((rc = inflate(&zs, Z_NO_FLUSH)) != Z_STREAM_END) {
        if (rc != Z_OK) { free(buf); inflateEnd(&zs); return false; }
        if (zs.avail_out == 0) {
            size_t used = zs.total_out;
            cap *= 2;
            uint8_t *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); inflateEnd(&zs); return false; }
            buf = tmp;
            zs.next_out  = buf + used;
            zs.avail_out = (uInt)(cap - used);
        }
    }
    *out     = buf;
    *out_len = zs.total_out;
    inflateEnd(&zs);
    return true;
}

typedef struct { uint8_t *buf; size_t size; size_t pos; } MemArchive;

static int     ma_open (struct archive *a, void *ud) { (void)a;(void)ud; return ARCHIVE_OK; }
static ssize_t ma_read (struct archive *a, void *ud, const void **buff) {
    (void)a;
    MemArchive *m = ud;
    if (m->pos >= m->size) return 0;
    *buff = m->buf + m->pos;
    size_t n = m->size - m->pos;
    m->pos = m->size;
    return (ssize_t)n;
}
static int ma_close(struct archive *a, void *ud) {
    (void)a;
    MemArchive *m = ud;
    free(m->buf); free(m);
    return ARCHIVE_OK;
}

static struct archive *db_open_archive(const uint8_t *gz, size_t gz_size) {
    uint8_t *tar      = NULL;
    size_t   tar_size = 0;
    if (!gunzip_buf(gz, gz_size, &tar, &tar_size)) return NULL;

    MemArchive *md = malloc(sizeof *md);
    if (!md) { free(tar); return NULL; }
    md->buf = tar; md->size = tar_size; md->pos = 0;

    struct archive *a = archive_read_new();
    archive_read_support_format_tar(a);
    if (archive_read_open(a, md, ma_open, ma_read, ma_close) != ARCHIVE_OK) {
        archive_read_free(a);
        free(tar); free(md);
        return NULL;
    }
    return a;
}

/* ───────────────────── repo DB cache ───────────────────────────── */

static bool cache_dir(char *out, size_t len) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s/.cache", home);       mkdir(tmp, 0755);
    snprintf(out, len, "%s/.cache/onopkg", home);       mkdir(out, 0755);
    return true;
}

static Buf *cache_load(const char *repo_path) {
    char dir[512], path[640];
    if (!cache_dir(dir, sizeof dir)) return NULL;

    const char *slash = strrchr(repo_path, '/');
    const char *fname = slash ? slash + 1 : repo_path;
    snprintf(path, sizeof path, "%s/%s", dir, fname);

    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (time(NULL) - st.st_mtime > DB_CACHE_TTL_SEC) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    Buf *b = buf_new();
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (b->size + n + 1 > b->cap) {
            while (b->cap < b->size + n + 1) b->cap *= 2;
            b->data = realloc(b->data, b->cap);
        }
        memcpy(b->data + b->size, chunk, n);
        b->size += n;
    }
    fclose(fp);
    return b;
}

static void cache_save(const char *repo_path, const Buf *b) {
    char dir[512], path[640];
    if (!cache_dir(dir, sizeof dir)) return;
    const char *slash = strrchr(repo_path, '/');
    const char *fname = slash ? slash + 1 : repo_path;
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(b->data, 1, b->size, fp);
    fclose(fp);
}

static Buf *db_fetch(const char *mirror_base, const char *repo_path,
                     long timeout_ms) {
    Buf *cached = cache_load(repo_path);
    if (cached) return cached;

    char url[URL_MAX];
    snprintf(url, sizeof url, "%s%s", mirror_base, repo_path);
    Buf *fresh = http_fetch(url, timeout_ms);
    if (fresh && fresh->size > 0)
        cache_save(repo_path, fresh);
    return fresh;
}

/* ──────────────── DB search / info helpers ─────────────────────── */

static bool db_contains(const uint8_t *gz, size_t gz_size, const char *query,
                         char *out_name, size_t out_len) {
    struct archive *a = db_open_archive(gz, gz_size);
    if (!a) return false;

    size_t qlen = strlen(query);
    char lquery[PKG_NAME_MAX];
    for (size_t i = 0; i <= qlen; i++)
        lquery[i] = (char)((query[i] >= 'A' && query[i] <= 'Z')
                           ? query[i] + 32 : query[i]);

    bool found = false, exact = false;
    struct archive_entry *entry;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *path = archive_entry_pathname(entry);
        size_t plen = strlen(path);
        if (plen < 6 || strcmp(path + plen - 5, "/desc") != 0) {
            archive_read_data_skip(a);
            continue;
        }
        size_t desc_len = 0;
        char *desc = archive_read_entry_text(a, &desc_len);
        char name[PKG_NAME_MAX] = {0};
        if (!desc_get_field(desc, desc_len, "%NAME%\n", name, sizeof name)) {
            free(desc); continue;
        }
        free(desc);

        char lname[PKG_NAME_MAX];
        size_t nlen = strlen(name);
        for (size_t i = 0; i <= nlen; i++)
            lname[i] = (char)((name[i] >= 'A' && name[i] <= 'Z')
                              ? name[i] + 32 : name[i]);

        bool is_exact  = (strcmp(lname, lquery) == 0);
        bool is_prefix = (!is_exact &&
                          strncmp(lname, lquery, qlen) == 0 &&
                          lname[qlen] == '-');

        if (is_exact || (is_prefix && !found)) {
            strncpy(out_name, name, out_len - 1);
            out_name[out_len - 1] = 0;
            found = true; exact = is_exact;
            if (exact) break;
        }
    }
    (void)exact;
    archive_read_free(a);
    return found;
}

static bool db_get_info(const uint8_t *gz, size_t gz_size,
                        const char *pkg_name,
                        char *out_fn, size_t fn_len,
                        char deps[][PKG_NAME_MAX], int *dep_count) {
    struct archive *a = db_open_archive(gz, gz_size);
    if (!a) return false;

    bool found = false;
    struct archive_entry *entry;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *path = archive_entry_pathname(entry);
        size_t plen = strlen(path);
        if (plen < 6 || strcmp(path + plen - 5, "/desc") != 0) {
            archive_read_data_skip(a);
            continue;
        }
        size_t desc_len = 0;
        char *desc = archive_read_entry_text(a, &desc_len);
        char name[PKG_NAME_MAX] = {0};
        if (!desc_get_field(desc, desc_len, "%NAME%\n", name, sizeof name) ||
            strcmp(name, pkg_name) != 0) {
            free(desc); continue;
        }
        found = desc_get_field(desc, desc_len, "%FILENAME%\n", out_fn, fn_len);
        if (dep_count)
            *dep_count = desc_get_depends(desc, desc_len, deps, MAX_DEPS);
        free(desc);
        break;
    }
    archive_read_free(a);
    return found;
}

/* ─────────────────────── thread pool ───────────────────────────── */
static void *pool_worker(void *arg) {
    Pool *pool = arg;
    while (1) {
        pthread_mutex_lock(&pool->mu);
        while (!pool->head && !pool->shutdown)
            pthread_cond_wait(&pool->cv, &pool->mu);
        if (pool->shutdown && !pool->head) {
            pthread_mutex_unlock(&pool->mu);
            return NULL;
        }
        WorkItem *item = pool->head;
        pool->head = item->next;
        if (!pool->head) pool->tail = NULL;
        pthread_mutex_unlock(&pool->mu);
        item->fn(item->arg);
        free(item);
        atomic_fetch_sub(&pool->pending, 1);
    }
}

static Pool *pool_create(int nworkers) {
    Pool *p = calloc(1, sizeof *p);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->workers = nworkers;
    pthread_t *threads = malloc((size_t)nworkers * sizeof(pthread_t));
    for (int i = 0; i < nworkers; i++)
        pthread_create(&threads[i], NULL, pool_worker, p);
    free(threads);
    return p;
}

static void pool_submit(Pool *p, void (*fn)(void *), void *arg) {
    WorkItem *item = malloc(sizeof *item);
    item->fn = fn; item->arg = arg; item->next = NULL;
    atomic_fetch_add(&p->pending, 1);
    pthread_mutex_lock(&p->mu);
    if (p->tail) p->tail->next = item; else p->head = item;
    p->tail = item;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

static void pool_wait(Pool *p) {
    while (atomic_load(&p->pending) > 0) sched_yield();
}

static void pool_destroy(Pool *p) {
    pthread_mutex_lock(&p->mu);
    p->shutdown = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    usleep(50000);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p);
}

/* ─────────────────────── progress bars ─────────────────────────── */
static void redraw_progress(int done, int total, const char *phase) {
    int bar_w  = 28;
    int filled = total ? (done * bar_w / total) : 0;
    pthread_mutex_lock(&g_print_mu);
    printf("\r  " C_DIM "[");
    for (int i = 0; i < bar_w; i++)
        fputs(i < filled ? "#" : "-", stdout);
    printf(C_DIM "] " C_RESET C_BOLD "%3d" C_RESET "/" C_BOLD "%d" C_RESET
           "  %s     ", done, total, phase);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mu);
}

typedef struct { curl_off_t total; curl_off_t last_printed; } DlProgress;

static int dl_progress_cb(void *ud, curl_off_t total, curl_off_t now,
                           curl_off_t dltotal, curl_off_t dlnow) {
    (void)dltotal; (void)dlnow;
    DlProgress *dp = ud;
    dp->total = total;
    if (now - dp->last_printed < 65536 && now < total) return 0;
    dp->last_printed = now;

    int bar_w  = 28;
    int filled = (total > 0) ? (int)((now * bar_w) / total) : 0;

    char now_s[16], tot_s[16];
    if (now   < 1024*1024) snprintf(now_s, sizeof now_s, "%.0f KB", (double)now   / 1024);
    else                   snprintf(now_s, sizeof now_s, "%.1f MB", (double)now   / (1024*1024));
    if (total < 1024*1024) snprintf(tot_s, sizeof tot_s, "%.0f KB", (double)total / 1024);
    else                   snprintf(tot_s, sizeof tot_s, "%.1f MB", (double)total / (1024*1024));

    printf("\r  " C_DIM "[");
    for (int i = 0; i < bar_w; i++)
        fputs(i < filled ? "#" : "-", stdout);
    printf(C_DIM "] " C_RESET C_BOLD "%s" C_RESET " / " C_BOLD "%s" C_RESET "     ",
           now_s, tot_s);
    fflush(stdout);
    return 0;
}

/* ──────────────────── ping / fetch tasks ───────────────────────── */
static void ping_task(void *arg) {
    Mirror *m  = arg;
    double lat = tcp_ping(m->host, 443, PING_TIMEOUT_MS);
    m->latency_ms = lat;
    m->reachable  = (lat > 0);
    redraw_progress(atomic_fetch_add(&g_ping_done, 1) + 1, g_ping_total, "pinging");
}

typedef struct { Mirror *mirror; int repo_idx; const char *query; } FetchArg;

static void fetch_task(void *arg) {
    FetchArg   *fa    = arg;
    Mirror     *m     = fa->mirror;
    int         ri    = fa->repo_idx;
    const char *query = fa->query;
    free(fa);

    char repo_short[32] = "?";
    sscanf(REPOS[ri], "%31[^/]", repo_short);
    redraw_progress(atomic_fetch_add(&g_fetch_done, 1) + 1, g_fetch_total, m->host);

    Buf *db = db_fetch(m->url, REPOS[ri], FETCH_TIMEOUT_MS);
    if (!db || db->size == 0) { buf_free(db); return; }

    char pkg_name[PKG_NAME_MAX] = {0};
    if (db_contains(db->data, db->size, query, pkg_name, sizeof pkg_name)) {
        pthread_mutex_lock(&match_mu);
        if (match_count < MAX_MATCHES && !m->has_package) {
            m->has_package = true;
            strncpy(m->matched_pkg,  pkg_name,   PKG_NAME_MAX - 1);
            strncpy(m->matched_repo, repo_short, 31);
            matches[match_count++] = m;
        }
        pthread_mutex_unlock(&match_mu);
    }
    buf_free(db);
}

/* ─────────────────────── mirror helpers ────────────────────────── */
static int cmp_mirror(const void *a, const void *b) {
    const Mirror *ma = *(const Mirror * const *)a;
    const Mirror *mb = *(const Mirror * const *)b;
    if (ma->latency_ms < 0) return  1;
    if (mb->latency_ms < 0) return -1;
    return (ma->latency_ms > mb->latency_ms) - (ma->latency_ms < mb->latency_ms);
}

static Mirror *find_best_mirror_for(const char *pkg_name) {
    pthread_mutex_lock(&match_mu);
    for (int i = 0; i < match_count; i++) {
        matches[i]->has_package = false;
        matches[i] = NULL;
    }
    match_count = 0;
    pthread_mutex_unlock(&match_mu);

    atomic_store(&g_fetch_done, 0);
    int reachable = 0;
    for (int i = 0; i < mirror_count; i++)
        if (mirrors[i].reachable) reachable++;
    g_fetch_total = reachable * REPO_COUNT;

    Pool *pool = pool_create(FETCH_THREADS);
    for (int i = 0; i < mirror_count; i++) {
        if (!mirrors[i].reachable) continue;
        for (int r = 0; REPOS[r] != NULL; r++) {
            FetchArg *fa = malloc(sizeof *fa);
            fa->mirror = &mirrors[i]; fa->repo_idx = r; fa->query = pkg_name;
            pool_submit(pool, fetch_task, fa);
        }
    }
    pool_wait(pool);
    pool_destroy(pool);
    printf("\n");

    if (match_count == 0) return NULL;
    qsort(matches, match_count, sizeof(Mirror *), cmp_mirror);
    return matches[0];
}

/* ═══════════════════════════════════════════════════════════════════
 * INSTALL / REMOVE  (-S / -R / -Rd)
 * ═══════════════════════════════════════════════════════════════════ */

/* Arch package metadata entries — never extracted to / */
static bool is_meta_entry(const char *name) {
    /* strip leading ./ */
    if (name[0] == '.' && name[1] == '/') name += 2;
    return (strcmp(name, ".PKGINFO")   == 0 ||
            strcmp(name, ".BUILDINFO") == 0 ||
            strcmp(name, ".MTREE")     == 0 ||
            strcmp(name, ".INSTALL")   == 0 ||
            strcmp(name, ".CHANGELOG") == 0 ||
            strcmp(name, ".CHECKSUMS") == 0);
}

/* recursively mkdir every component of path */
static void mkdirs_p(const char *path) {
    char tmp[1024];
    strncpy(tmp, path, sizeof tmp - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* /var/lib/onopkg/<pkg>/files.txt */
static void pkgdb_files_path(const char *pkg, char *out, size_t len) {
    snprintf(out, len, PKGDB_DIR "/%s/files.txt", pkg);
}

/* /var/lib/onopkg/<pkg>/deps.txt */
static void pkgdb_deps_path(const char *pkg, char *out, size_t len) {
    snprintf(out, len, PKGDB_DIR "/%s/deps.txt", pkg);
}

static bool pkgdb_is_installed(const char *pkg) {
    char path[512];
    pkgdb_files_path(pkg, path, sizeof path);
    struct stat st;
    return stat(path, &st) == 0;
}

static void pkgdb_write_deps(const char *pkg,
                              char deps[][PKG_NAME_MAX], int count) {
    char path[512];
    pkgdb_deps_path(pkg, path, sizeof path);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < count; i++) fprintf(fp, "%s\n", deps[i]);
    fclose(fp);
}

static int pkgdb_read_deps(const char *pkg,
                            char out[][PKG_NAME_MAX], int max) {
    char path[512];
    pkgdb_deps_path(pkg, path, sizeof path);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int n = 0;
    char line[PKG_NAME_MAX];
    while (n < max && fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (l > 0) strncpy(out[n++], line, PKG_NAME_MAX - 1);
    }
    fclose(fp);
    return n;
}

/* returns true if any installed package (other than exclude_pkg) lists dep */
static bool pkgdb_any_needs(const char *dep, const char *exclude_pkg) {
    DIR *d = opendir(PKGDB_DIR);
    if (!d) return false;
    struct dirent *ent;
    char rdeps[MAX_DEPS][PKG_NAME_MAX];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strcmp(ent->d_name, exclude_pkg) == 0) continue;
        int n = pkgdb_read_deps(ent->d_name, rdeps, MAX_DEPS);
        for (int i = 0; i < n; i++)
            if (strcmp(rdeps[i], dep) == 0) { closedir(d); return true; }
    }
    closedir(d);
    return false;
}

/* extract tarball to /, log every path (files + dirs) to files.txt */
static int install_pkg(const char *pkg_name, const char *tarball_path,
                       char deps[][PKG_NAME_MAX], int dep_count) {
    struct archive       *a   = archive_read_new();
    struct archive       *out = archive_write_disk_new();
    struct archive_entry *entry;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    archive_write_disk_set_options(out,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS |
        ARCHIVE_EXTRACT_OWNER);
    archive_write_disk_set_standard_lookup(out);

    if (archive_read_open_filename(a, tarball_path, 65536) != ARCHIVE_OK) {
        fprintf(stderr, C_RED "  error:" C_RESET " Cannot open %s: %s\n",
                tarball_path, archive_error_string(a));
        archive_read_free(a);
        archive_write_free(out);
        return 1;
    }

    /* prepare pkgdb entry */
    char dbdir[512], fileslog[512];
    snprintf(dbdir, sizeof dbdir, PKGDB_DIR "/%s", pkg_name);
    mkdirs_p(PKGDB_DIR);
    mkdir(PKGDB_DIR, 0755);
    mkdir(dbdir, 0755);
    pkgdb_files_path(pkg_name, fileslog, sizeof fileslog);

    FILE *log = fopen(fileslog, "w");
    if (!log) {
        fprintf(stderr, C_RED "  error:" C_RESET
                " Cannot open file log %s: %s\n", fileslog, strerror(errno));
        archive_read_free(a); archive_write_free(out);
        return 1;
    }

    printf(C_BOLD "  ::" C_RESET " Installing " C_CYAN C_BOLD "%s"
           C_RESET " to " C_BOLD "/" C_RESET "…\n", pkg_name);

    int nfiles = 0;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (is_meta_entry(name)) { archive_read_data_skip(a); continue; }

        /* rewrite path: strip ./ prefix, prepend / */
        const char *rel = (name[0] == '.' && name[1] == '/') ? name + 2 : name;
        char dest[1024];
        snprintf(dest, sizeof dest, "/%s", rel);
        archive_entry_set_pathname(entry, dest);

        fprintf(log, "%s\n", dest);
        nfiles++;

        /* ensure parent exists */
        char parent[1024];
        strncpy(parent, dest, sizeof parent - 1);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) { *sl = 0; mkdirs_p(parent); }

        if (archive_write_header(out, entry) == ARCHIVE_OK) {
            char buf[65536]; ssize_t n;
            while ((n = archive_read_data(a, buf, sizeof buf)) > 0)
                archive_write_data(out, buf, (size_t)n);
        }
    }
    fclose(log);
    archive_read_free(a);
    archive_write_free(out);

    pkgdb_write_deps(pkg_name, deps, dep_count);
    db_add(pkg_name);   /* also record in flat installed.txt */

    printf(C_GREEN "  ::" C_RESET " Installed " C_BOLD "%d" C_RESET
           " files for " C_CYAN "%s" C_RESET ".\n\n", nfiles, pkg_name);
    return 0;
}

/* remove a single package's files from the system */
/* files/paths that must never be removed — would break the running system */
static bool is_protected(const char *path) {
    /* exact paths */
    static const char *exact[] = {
        "/usr/lib/libcurl.so.4",
        "/usr/lib/libcurl.so",
        "/usr/lib/libz.so.1",
        "/usr/lib/libz.so",
        "/usr/lib/libarchive.so.13",
        "/usr/lib/libarchive.so",
        "/usr/lib/libssl.so.3",
        "/usr/lib/libssl.so",
        "/usr/lib/libcrypto.so.3",
        "/usr/lib/libcrypto.so",
        "/usr/lib/libpthread.so.0",
        "/usr/lib/libpthread.so",
        "/usr/lib/libm.so.6",
        "/usr/lib/libm.so",
        "/usr/lib/libc.so.6",
        "/usr/lib/libc.so",
        "/usr/lib/libdl.so.2",
        "/usr/lib/libdl.so",
        "/usr/lib/libgcc_s.so.1",
        "/lib/libcurl.so.4",
        "/lib/libz.so.1",
        "/lib/libc.so.6",
        "/lib/libm.so.6",
        "/lib/libpthread.so.0",
        "/lib64/ld-linux-x86-64.so.2",
        "/usr/lib/ld-linux-x86-64.so.2",
        NULL
    };
    for (int i = 0; exact[i]; i++)
        if (strcmp(path, exact[i]) == 0) return true;

    /* protect anything that looks like a core shared lib by pattern */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    /* libc, libm, libpthread, ld-linux, libgcc — never touch these */
    if (strncmp(base, "libc.",        5) == 0) return true;
    if (strncmp(base, "libc-",        5) == 0) return true;
    if (strncmp(base, "libm.",        5) == 0) return true;
    if (strncmp(base, "libm-",        5) == 0) return true;
    if (strncmp(base, "libpthread",  10) == 0) return true;
    if (strncmp(base, "libdl.",       6) == 0) return true;
    if (strncmp(base, "libgcc_s",     8) == 0) return true;
    if (strncmp(base, "ld-linux",     8) == 0) return true;
    if (strncmp(base, "ld-",          3) == 0 &&
        strstr(base, ".so")               != NULL) return true;

    /* protect the libs onopkg itself links against */
    if (strncmp(base, "libcurl",  7) == 0) return true;
    if (strncmp(base, "libz.",    5) == 0) return true;
    if (strncmp(base, "libz-",    5) == 0) return true;
    if (strncmp(base, "libarchive", 10) == 0) return true;
    if (strncmp(base, "libssl.",   7) == 0) return true;
    if (strncmp(base, "libcrypto", 9) == 0) return true;

    return false;
}

static int remove_pkg(const char *pkg_name) {
    char fileslog[512];
    pkgdb_files_path(pkg_name, fileslog, sizeof fileslog);

    FILE *fp = fopen(fileslog, "r");
    if (!fp) {
        fprintf(stderr, C_RED "  error:" C_RESET
                " No file log for '%s'. Was it installed with -S?\n", pkg_name);
        return 1;
    }

    /* split into files and dirs */
    char  files[8192][512];
    char  dirs [1024][512];
    int   nf = 0, nd = 0;
    char  line[512];

    while (fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (l == 0) continue;
        struct stat st;
        if (stat(line, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (nd < 1024) strncpy(dirs[nd++],  line, 511); }
        else                     { if (nf < 8192) strncpy(files[nf++], line, 511); }
    }
    fclose(fp);

    printf(C_BOLD "  ::" C_RESET " Removing " C_CYAN C_BOLD "%s" C_RESET "…\n",
           pkg_name);

    int removed = 0, protected = 0;
    for (int i = 0; i < nf; i++) {
        if (is_protected(files[i])) {
            printf(C_YELLOW "  skip:" C_RESET " %s " C_DIM "(protected)" C_RESET "\n",
                   files[i]);
            protected++;
            continue;
        }
        if (unlink(files[i]) == 0) removed++;
        else fprintf(stderr, C_DIM "  skip: %s (%s)\n" C_RESET,
                     files[i], strerror(errno));
    }
    if (protected > 0)
        printf(C_YELLOW "  ::" C_RESET " Skipped " C_BOLD "%d" C_RESET
               " protected system file(s).\n", protected);
    /* dirs deepest-first */
    for (int i = nd - 1; i >= 0; i--)
        rmdir(dirs[i]);   /* silently ignore non-empty */

    /* clean pkgdb entry */
    char depslog[512], dbdir[512];
    pkgdb_deps_path(pkg_name, depslog, sizeof depslog);
    snprintf(dbdir, sizeof dbdir, PKGDB_DIR "/%s", pkg_name);
    unlink(fileslog);
    unlink(depslog);
    rmdir(dbdir);

    db_remove(pkg_name);  /* also remove from flat installed.txt */

    printf(C_GREEN "  ::" C_RESET " Removed " C_BOLD "%d" C_RESET
           " files for " C_CYAN "%s" C_RESET ".\n\n", removed, pkg_name);
    return 0;
}

/* remove package + any deps that are now orphaned */
static int remove_pkg_with_deps(const char *pkg_name) {
    char deps[MAX_DEPS][PKG_NAME_MAX];
    int  dep_count = pkgdb_read_deps(pkg_name, deps, MAX_DEPS);

    int ret = remove_pkg(pkg_name);
    if (ret != 0) return ret;

    for (int i = 0; i < dep_count; i++) {
        if (!pkgdb_is_installed(deps[i])) continue;
        if (pkgdb_any_needs(deps[i], pkg_name)) {
            printf(C_DIM "  :: Keeping %s (still needed).\n" C_RESET, deps[i]);
            continue;
        }
        printf(C_BOLD "  ::" C_RESET " Removing orphan " C_CYAN "%s" C_RESET "…\n",
               deps[i]);
        remove_pkg_with_deps(deps[i]);   /* recurse for transitive orphans */
    }
    return 0;
}

/* ─────────────────────── download one pkg ──────────────────────── */

static bool mark_session(const char *pkg) {
    pthread_mutex_lock(&session_mu);
    for (int i = 0; i < session_done_count; i++) {
        if (strcmp(session_done[i], pkg) == 0) {
            pthread_mutex_unlock(&session_mu);
            return false;
        }
    }
    if (session_done_count < MAX_PKG_QUEUE)
        strncpy(session_done[session_done_count++], pkg, PKG_NAME_MAX - 1);
    pthread_mutex_unlock(&session_mu);
    return true;
}

static int download_one(Mirror *best, const char *pkg_name,
                         char deps[][PKG_NAME_MAX], int *dep_count) {
    char repo_path[URL_MAX];
    snprintf(repo_path, sizeof repo_path, "%s/os/x86_64/%s.db",
             best->matched_repo, best->matched_repo);

    Buf *db = db_fetch(best->url, repo_path, FETCH_TIMEOUT_MS);
    if (!db || db->size == 0) {
        fprintf(stderr, C_RED "  error:" C_RESET " Could not load DB for '%s'.\n", pkg_name);
        buf_free(db);
        return 1;
    }

    char filename[256] = {0};
    char local_deps[MAX_DEPS][PKG_NAME_MAX];
    int  local_dep_count = 0;

    if (!db_get_info(db->data, db->size, pkg_name,
                     filename, sizeof filename,
                     local_deps, &local_dep_count)) {
        fprintf(stderr, C_RED "  error:" C_RESET " Package '%s' not found in DB.\n", pkg_name);
        buf_free(db);
        return 1;
    }
    buf_free(db);

    if (dep_count) {
        *dep_count = local_dep_count;
        for (int i = 0; i < local_dep_count; i++)
            strncpy(deps[i], local_deps[i], PKG_NAME_MAX - 1);
    }

    printf(C_GREEN "  ::" C_RESET " File: " C_CYAN C_BOLD "%s" C_RESET "\n\n", filename);

    char dl_url[URL_MAX];
    snprintf(dl_url, sizeof dl_url, "%s%s/os/x86_64/%s",
             best->url, best->matched_repo, filename);

    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dest[512];
    snprintf(dest, sizeof dest, "%s/%s", home, filename);

    printf(C_BOLD "  ::" C_RESET " Downloading to " C_BOLD "%s" C_RESET "…\n", dest);

    FILE *fp = fopen(dest, "wb");
    if (!fp) {
        fprintf(stderr, C_RED "  error:" C_RESET " Cannot open %s: %s\n",
                dest, strerror(errno));
        return 1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(fp); return 1; }

    DlProgress dp = {0};
    curl_easy_setopt(curl, CURLOPT_URL,              dl_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,        "onopkg/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &dp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          120L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);
    printf("\n");

    if (rc != CURLE_OK) {
        fprintf(stderr, C_RED "  error:" C_RESET " Download failed: %s\n",
                curl_easy_strerror(rc));
        unlink(dest);
        return 1;
    }

    printf(C_GREEN C_BOLD "  :: Done!" C_RESET "  Saved to " C_CYAN "%s" C_RESET "\n\n", dest);

    if (g_do_install) {
        int ir = install_pkg(pkg_name, dest, local_deps, local_dep_count);
        unlink(dest);  /* remove tarball after extraction */
        if (ir != 0) return ir;
    } else {
        /* record in flat installed DB (download-only mode) */
        db_add(pkg_name);
    }

    return 0;
}

/*
 * Returns true if the dep string is a soname / file provision rather than
 * a real package name — e.g. "libncursesw.so=6", "libm.so.6", "ld-linux.so.2".
 * These are satisfied by packages already on the system and can't be looked
 * up by name in the repo DB, so we skip them entirely.
 */
static bool is_soname_dep(const char *dep) {
    return strstr(dep, ".so") != NULL;
}

static int download_recursive(const char *pkg_name, Mirror *hint, int depth) {
    /* filter soname provisions before doing anything — they're not packages */
    if (depth > 0 && is_soname_dep(pkg_name)) {
        printf(C_DIM "  :: Skipping " C_RESET C_BOLD "%s" C_RESET
               C_DIM " (soname provision)" C_RESET "\n", pkg_name);
        return 0;
    }

    if (!mark_session(pkg_name)) return 0;

    /* skip deps that are already present on the system */
    if (depth > 0 && is_installed(pkg_name)) {
        printf(C_DIM "  :: Skipping " C_RESET C_BOLD "%s" C_RESET
               C_DIM " (already installed)" C_RESET "\n", pkg_name);
        return 0;
    }

    printf(C_BOLD "  ::" C_RESET " Resolving " C_CYAN C_BOLD "%s" C_RESET "%s…\n",
           pkg_name, depth > 0 ? " (dependency)" : "");

    Mirror *best = hint;
    if (!best) {
        best = find_best_mirror_for(pkg_name);
        if (!best) {
            printf(C_YELLOW "  warn:" C_RESET
                   " '%s' not found in any mirror "
                   "(may be virtual or already installed).\n", pkg_name);
            return 0;
        }
    }

    char deps[MAX_DEPS][PKG_NAME_MAX];
    int  dep_count = 0;
    int  ret = download_one(best, pkg_name, deps, &dep_count);
    if (ret != 0) return ret;

    if (dep_count > 0) {
        printf(C_BOLD "  :: Dependencies of %s:\n" C_RESET, pkg_name);
        for (int i = 0; i < dep_count; i++)
            printf("     %s\n", deps[i]);
        printf("\n");
        for (int i = 0; i < dep_count; i++) {
            ret = download_recursive(deps[i], NULL, depth + 1);
            if (ret != 0) return ret;
        }
    }

    return 0;
}

/* ──────────────────────────── main ─────────────────────────────── */
static void print_usage(void) {
    fprintf(stderr,
        C_BOLD "Usage:" C_RESET "\n"
        "  onopkg <package>       Download package + dependencies\n"
        "  onopkg -S <package>    Search, download and install to /  (requires root)\n"
        "  onopkg --sync          Scan system and populate installed DB\n"
        "  onopkg -R <package>    Remove package from system         (requires root)\n"
        "  onopkg -Rd <package>   Remove package + orphaned deps     (requires root)\n"
        "  onopkg --list          List all packages in installed DB\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(); return 1; }

    /* ── simple commands that don't need network ── */
    if (strcmp(argv[1], "--sync") == 0)
        return cmd_sync();

    if (strcmp(argv[1], "--list") == 0)
        return db_list();

    /* ── -S: install (requires root) ── */
    if (strcmp(argv[1], "-S") == 0) {
        if (argc < 3) {
            fprintf(stderr, C_RED "  error:" C_RESET " -S requires a package name.\n");
            return 1;
        }
        if (getuid() != 0) {
            fprintf(stderr, C_RED C_BOLD "  error:" C_RESET
                    " onopkg -S requires root. Run with sudo.\n");
            return 1;
        }
        mkdir(PKGDB_DIR, 0755);
        g_do_install = true;
        /* fall through to network search below */
        argv[1] = argv[2];   /* shift so query = argv[1] */
        argc--;
    }

    /* ── -R: remove package only (requires root) ── */
    if (strcmp(argv[1], "-R") == 0 || strcmp(argv[1], "--remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, C_RED "  error:" C_RESET " -R requires a package name.\n");
            return 1;
        }
        if (getuid() != 0) {
            fprintf(stderr, C_RED C_BOLD "  error:" C_RESET
                    " onopkg -R requires root. Run with sudo.\n");
            return 1;
        }
        return remove_pkg(argv[2]);
    }

    /* ── -Rd: remove package + orphaned deps (requires root) ── */
    if (strcmp(argv[1], "-Rd") == 0) {
        if (argc < 3) {
            fprintf(stderr, C_RED "  error:" C_RESET " -Rd requires a package name.\n");
            return 1;
        }
        if (getuid() != 0) {
            fprintf(stderr, C_RED C_BOLD "  error:" C_RESET
                    " onopkg -Rd requires root. Run with sudo.\n");
            return 1;
        }
        return remove_pkg_with_deps(argv[2]);
    }

    /* ── default / -S: download (and optionally install) a package ── */
    const char *query = argv[1];
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* 1. fetch mirrorlist */
    printf(C_BOLD "  ::" C_RESET " Fetching mirror list…\n");
    Buf *ml = http_fetch(MIRRORLIST_URL, 10000);
    if (!ml || ml->size == 0) {
        fprintf(stderr, C_RED "  error:" C_RESET " Could not fetch mirrorlist.\n");
        return 1;
    }
    parse_mirrorlist((char *)ml->data);
    buf_free(ml);
    printf(C_GREEN "  ::" C_RESET " Found " C_BOLD "%d" C_RESET " mirrors.\n\n",
           mirror_count);

    /* 2. parallel ping */
    printf(C_BOLD "  ::" C_RESET " Pinging all mirrors in parallel…\n");
    g_ping_total = mirror_count;
    Pool *ping_pool = pool_create(PING_THREADS);
    for (int i = 0; i < mirror_count; i++)
        pool_submit(ping_pool, ping_task, &mirrors[i]);
    pool_wait(ping_pool);
    pool_destroy(ping_pool);
    printf("\n");

    int reachable = 0;
    for (int i = 0; i < mirror_count; i++)
        if (mirrors[i].reachable) reachable++;
    printf(C_GREEN "  ::" C_RESET " " C_BOLD "%d" C_RESET " / " C_BOLD "%d"
           C_RESET " mirrors reachable.\n\n", reachable, mirror_count);

    /* 3. parallel DB search */
    printf(C_BOLD "  ::" C_RESET " Searching for " C_CYAN C_BOLD "\"%s\""
           C_RESET " across repos…\n", query);
    g_fetch_total = reachable * REPO_COUNT;
    atomic_store(&g_fetch_done, 0);

    Pool *fetch_pool = pool_create(FETCH_THREADS);
    for (int i = 0; i < mirror_count; i++) {
        if (!mirrors[i].reachable) continue;
        for (int r = 0; REPOS[r] != NULL; r++) {
            FetchArg *fa = malloc(sizeof *fa);
            fa->mirror = &mirrors[i]; fa->repo_idx = r; fa->query = query;
            pool_submit(fetch_pool, fetch_task, fa);
        }
    }
    pool_wait(fetch_pool);
    pool_destroy(fetch_pool);
    printf("\n\n");

    if (match_count == 0) {
        printf(C_YELLOW "  ::" C_RESET " No mirrors found with a package matching "
               C_BOLD "\"%s\"" C_RESET ".\n\n", query);
        curl_global_cleanup();
        return 0;
    }

    /* 4. sort & display results */
    qsort(matches, match_count, sizeof(Mirror *), cmp_mirror);
    int display_count = match_count < 5 ? match_count : 5;

    printf(C_BOLD "  :: Top %d results for \"" C_CYAN "%s" C_RESET C_BOLD
           "\"  —  " C_GREEN "%d" C_RESET C_BOLD " mirror(s) found\n\n" C_RESET,
           display_count, query, match_count);
    printf("  %-6s  %-40s  %-12s  %-14s  %s\n",
           "RANK", "MIRROR", "REPO", "PACKAGE", "LATENCY");
    printf("  ");
    for (int i = 0; i < 92; i++) printf("-");
    printf("\n");

    for (int i = 0; i < display_count; i++) {
        Mirror *m = matches[i];
        const char *colour = (i == 0) ? C_GREEN C_BOLD :
                             (i == 1) ? C_YELLOW :
                             (i == 2) ? C_MAGENTA : C_DIM;
        printf("  %s#%-5d" C_RESET "  %-40s  %-12s  " C_CYAN "%-14s" C_RESET "  ",
               colour, i + 1, m->host, m->matched_repo, m->matched_pkg);
        if      (m->latency_ms <  50) printf(C_GREEN  "%.1f ms\n" C_RESET, m->latency_ms);
        else if (m->latency_ms < 150) printf(C_YELLOW "%.1f ms\n" C_RESET, m->latency_ms);
        else                          printf(C_RED    "%.1f ms\n" C_RESET, m->latency_ms);
    }

    /* 5. best mirror summary */
    Mirror *best = matches[0];
    printf("\n" C_BOLD "  :: Best mirror:" C_RESET "\n\n");
    printf("     " C_GREEN C_BOLD "%s" C_RESET "\n", best->url);
    printf("     Package : " C_CYAN C_BOLD "%s" C_RESET " (%s)\n",
           best->matched_pkg, best->matched_repo);
    printf("     Latency : " C_GREEN C_BOLD "%.1f ms" C_RESET "\n\n", best->latency_ms);

    /* 6. download package + all deps */
    int ret = download_recursive(best->matched_pkg, best, 0);

    curl_global_cleanup();
    return ret;
}
