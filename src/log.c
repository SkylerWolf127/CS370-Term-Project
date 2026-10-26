#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SESSION_PREFIX "session_"
#define SESSION_SUFFIX ".txt"
#define SESSION_STAMP_LEN 15  /* YYYYMMDD_HHMMSS */

static void iso_time(char *buf, size_t len, time_t when)
{
    struct tm tm;
    if (!localtime_r(&when, &tm) || strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm) == 0)
        snprintf(buf, len, "time=%lld", (long long)when);
}

int log_open(struct session_log *log, const char *dir, time_t started)
{
    log->fp = NULL;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;

    struct tm tm;
    char stamp[32];
    if (!localtime_r(&started, &tm) || strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm) == 0) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(log->path, sizeof log->path, "%s/" SESSION_PREFIX "%s" SESSION_SUFFIX,
                 dir, stamp) >= (int)sizeof log->path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    log->fp = fopen(log->path, "a");
    return log->fp ? 0 : -1;
}

int log_close(struct session_log *log)
{
    if (!log->fp)
        return 0;
    int rc = fclose(log->fp);
    log->fp = NULL;
    return rc == 0 ? 0 : -1;
}

void log_format_reading(char *buf, size_t len, time_t when, fixed4_t level,
                        fixed4_t avg, enum light_verdict v)
{
    char ts[32], lv[FIXED4_STR_LEN], av[FIXED4_STR_LEN];
    iso_time(ts, sizeof ts, when);
    snprintf(buf, len, "%s level=%s avg=%s %s", ts,
             fixed4_format(level, lv, sizeof lv),
             fixed4_format(avg, av, sizeof av), light_verdict_str(v));
}

void log_format_failure(char *buf, size_t len, time_t when, const char *reason)
{
    char ts[32];
    iso_time(ts, sizeof ts, when);
    snprintf(buf, len, "%s ERROR capture failed: %s", ts, reason);
}

int log_write_line(struct session_log *log, const char *line)
{
    if (!log->fp) {
        errno = EBADF;
        return -1;
    }
    if (fputs(line, log->fp) == EOF || fputc('\n', log->fp) == EOF)
        return -1;
    /* fflush reaches the kernel; fsync reaches the SD card. A power cut
     * then loses at most the reading in flight. */
    if (fflush(log->fp) != 0 || fsync(fileno(log->fp)) != 0)
        return -1;
    return 0;
}

static int is_session_name(const char *name)
{
    size_t pre = strlen(SESSION_PREFIX), suf = strlen(SESSION_SUFFIX);
    return strlen(name) == pre + SESSION_STAMP_LEN + suf
        && strncmp(name, SESSION_PREFIX, pre) == 0
        && strcmp(name + pre + SESSION_STAMP_LEN, SESSION_SUFFIX) == 0;
}

int log_find_newest(const char *dir, char *out, size_t len)
{
    DIR *d = opendir(dir);
    if (!d)
        return errno == ENOENT ? 1 : -1;

    /* Timestamps are zero-padded, so the largest name is the newest. */
    char best[64] = "";
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d)) != NULL) {
        /* is_session_name pins the length, so this always fits in best. */
        if (is_session_name(e->d_name) && strcmp(e->d_name, best) > 0)
            memcpy(best, e->d_name, strlen(e->d_name) + 1);
    }
    int read_err = errno;
    closedir(d);
    if (read_err)
        return -1;
    if (best[0] == '\0')
        return 1;
    if (snprintf(out, len, "%s/%s", dir, best) >= (int)len)
        return -1;
    return 0;
}

int log_print_file(const char *path, FILE *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char buf[4096];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(f))
        rc = -1;
    fclose(f);
    return rc;
}
