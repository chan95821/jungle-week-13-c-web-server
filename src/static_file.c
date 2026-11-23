#define _POSIX_C_SOURCE 200809L
#include "static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int open_static_root(const char *path) {
    /* open/openat/fstat/close는 POSIX API이고 이 함수는 우리가 만든 준비 함수다.
     * 작업 디렉터리를 chdir하지 않고 루트 FD를 보관하므로 이후 파일 찾기의 기준이
     * 다른 코드의 현재 디렉터리나 클라이언트 입력으로 바뀌지 않는다. */
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static unsigned char lower_ascii(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower_ascii(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int decode_relative_path(struct http_slice path, char *out) {
    const size_t prefix = sizeof(STATIC_URL_PREFIX) - 1;
    if (path.length < prefix || path.length > REQ_HEADER_MAX_BYTES ||
        !path.data || memcmp(path.data, STATIC_URL_PREFIX, prefix) != 0) return 400;
    size_t used = 0;
    for (size_t i = prefix; i < path.length; ++i) {
        unsigned char c = path.data[i];
        if (c == '%') {
            if (path.length - i < 3) return 400;
            int high = hex_value(path.data[i + 1]);
            int low = hex_value(path.data[i + 2]);
            if (high < 0 || low < 0) return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        }
        /* 쿼리의 +→공백 규칙을 경로에 적용하지 않는다. +는 파일명의 + 그대로다.
         * NUL을 허용하면 검사한 길이와 openat이 사용하는 문자열 범위가 달라져 거부한다. */
        if (c < 32 || c == 127 || c == '\\') return 400;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    if (used == 0) strcpy(out, "index.html");
    return 0;
}

static int validate_relative_path(const char *path) {
    /* 디코딩 후 검사해야 %2e%2e와 %2f를 이용한 우회도 같은 규칙으로 처리된다.
     * ..를 제거해서 정규화하는 대신 .과 .. 요소 자체를 거부한다.
     * 먼저 전체 경로를 검사하므로 missing/../secret도 파일 존재 여부와 무관하게 거부한다. */
    size_t length = strlen(path);
    if (!length) return 400;
    if (path[length - 1] == '/') return 403; /* 하위 디렉터리의 자동 index/목록은 제외. */
    const char *part = path;
    for (;;) {
        const char *slash = strchr(part, '/');
        size_t count = slash ? (size_t)(slash - part) : strlen(part);
        if (!count) return 400; /* 선행 /, 반복 //를 상대 파일 경로로 수용하지 않음. */
        if ((count == 1 && part[0] == '.') ||
            (count == 2 && part[0] == '.' && part[1] == '.')) return 403;
        if (!slash) return 0;
        part = slash + 1;
    }
}

static int file_error_status(int directory, const char *part, int error) {
    /* Linux에서 중간 symlink + O_DIRECTORY는 ELOOP 대신 ENOTDIR가 될 수 있다.
     * 실패한 요소를 따라가지 않고 확인해서 symlink 거부(403)와 없는 경로(404)를 구분한다.
     * 이 확인 결과를 근거로 파일을 다시 열지는 않으므로 검사/열기 경합을 만들지 않는다. */
    struct stat info;
    if (fstatat(directory, part, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(info.st_mode)) return 403;
    switch (error) {
    case EACCES: case EPERM: case ELOOP: case ENXIO: return 403;
    case ENOENT: case ENOTDIR: return 404;
    case ENAMETOOLONG: return 400;
    default: return 500;
    }
}

static int suffix_equal(const char *suffix, const char *expected) {
    while (*suffix && *expected) {
        if (lower_ascii((unsigned char)*suffix++) != (unsigned char)*expected++) return 0;
    }
    return *suffix == '\0' && *expected == '\0';
}

const char *mime_from_path(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *extension = strrchr(name, '.');
    if (!extension) return "application/octet-stream";
    /* HTML/CSS 샘플은 UTF-8로 준비하고 이미지에는 charset을 붙이지 않는다.
     * 확장자 매핑만 수행하므로 파일 내용의 유효성까지 검증한다는 의미는 아니다. */
    if (suffix_equal(extension, ".html") || suffix_equal(extension, ".htm")) return "text/html; charset=utf-8";
    if (suffix_equal(extension, ".css")) return "text/css; charset=utf-8";
    if (suffix_equal(extension, ".js")) return "text/javascript; charset=utf-8";
    if (suffix_equal(extension, ".png")) return "image/png";
    if (suffix_equal(extension, ".jpg") || suffix_equal(extension, ".jpeg")) return "image/jpeg";
    if (suffix_equal(extension, ".gif")) return "image/gif";
    if (suffix_equal(extension, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

int prepare_static_file(int rootfd, struct http_slice path, struct static_file *out) {
    *out = (struct static_file){.fd = -1};
    char relative[REQ_HEADER_MAX_BYTES + 1];
    int status = decode_relative_path(path, relative);
    if (status) return status;
    status = validate_relative_path(relative);
    if (status) return status;
    out->content_type = mime_from_path(relative);

    /* 루트 FD 자체를 복제해 우리가 소유하는 탐색용 FD를 얻는다.
     * 어떤 분기에서도 main의 rootfd를 닫거나 변경하지 않는다. */
    int directory = fcntl(rootfd, F_DUPFD_CLOEXEC, 0);
    if (directory < 0) return 500;
    char *part = relative;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash) *slash = '\0';
        /* 모든 중간 요소도 O_NOFOLLOW로 열어야 root/link/file 경로로 탈출할 수 없다.
         * 마지막 요소는 O_NONBLOCK으로 열어 FIFO 같은 특수 파일에서 멈추지 않게 하고,
         * 곧바로 fstat하여 일반 파일만 허용한다. 이 옵션은 일반 파일 read에는 영향이 없다. */
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
        flags |= slash ? O_DIRECTORY : O_NONBLOCK;
        int fd = openat(directory, part, flags);
        if (fd < 0) {
            int error = errno;
            status = file_error_status(directory, part, error);
            close(directory);
            return status;
        }
        close(directory);
        if (slash) {
            directory = fd;
            part = slash + 1;
            continue;
        }
        struct stat info;
        if (fstat(fd, &info) < 0 || info.st_size < 0) {
            close(fd);
            return 500;
        }
        if (!S_ISREG(info.st_mode)) {
            close(fd);
            return 403;
        }
        /* 경로를 stat한 뒤 다시 여는 대신, 열린 바로 그 FD의 크기를 확인한다.
         * 이후 같은 FD를 읽으므로 파일명 교체로 다른 파일을 전송하는 경합을 피한다. */
        out->fd = fd;
        out->length = (uintmax_t)info.st_size;
        return 0;
    }
}
