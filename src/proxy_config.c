#define _POSIX_C_SOURCE 200809L
#include "proxy_config.h"
#include "proxy_selection.h"
#include "server.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/* 설정 파일 한 줄의 최대 길이. 이보다 긴 줄은 형식 오류로 처리한다. */
#define PROXY_CONFIG_LINE_MAX 256
/* 숫자 IPv4·IPv6 문자열을 담을 크기(INET6_ADDRSTRLEN보다 여유를 둔다). */
#define PROXY_CONFIG_ADDRESS_MAX 64

/* 파싱 중인 임시 결과. 파일 전체가 유효할 때만 확정 설정으로 복사한다. */
struct parsed_routes {
    char submission_host[PROXY_CONFIG_ADDRESS_MAX];
    struct proxy_backend submission;
    size_t submission_count;
    char lookup_hosts[PROXY_LOOKUP_MAX_BACKENDS][PROXY_CONFIG_ADDRESS_MAX];
    struct proxy_backend lookups[PROXY_LOOKUP_MAX_BACKENDS];
    size_t lookup_count;
};

/* 확정된 설정. 파싱 성공 후에는 읽기만 하며 프로세스 수명 동안 유지된다. */
static char submission_host[PROXY_CONFIG_ADDRESS_MAX];
static struct proxy_backend submission_backends[1];
static size_t submission_count;
static char lookup_hosts[PROXY_LOOKUP_MAX_BACKENDS][PROXY_CONFIG_ADDRESS_MAX];
static struct proxy_backend lookup_backends[PROXY_LOOKUP_MAX_BACKENDS];
static size_t lookup_count;

/* 조회 그룹의 순번·제외 만료 시각은 요청이 끝나도 유지해야 하므로 정적 저장소를 쓴다. */
static struct timespec lookup_excluded_until[PROXY_LOOKUP_MAX_BACKENDS];
static struct proxy_group_state lookup_group;

/* 라우트 핸들러가 그대로 빌려 쓰는 확정 설정. init 전에는 backend_count가 0이라 500이 된다. */
static struct proxy_upstream submission_upstream;
static struct proxy_upstream lookup_upstream;

/* 줄 앞뒤 공백을 잘라내고 포인터를 앞으로 이동한다(원본 버퍼 내부만 수정). */
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') ++text;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
    return text;
}

/* 공백으로 구분된 다음 단어를 꺼내고 커서를 전진시킨다. 없으면 NULL. */
static char *next_word(char **cursor) {
    char *p = *cursor;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') { *cursor = p; return NULL; }

    char *word = p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    if (*p) *p++ = '\0';
    *cursor = p;
    return word;
}

/* "host:port" 또는 "[IPv6]:port"를 숫자 주소로 검증해 임시 결과에 담는다.
 * 이름 조회(DNS)는 연결 단계에서도 하지 않으므로 설정 단계에서도 숫자 주소만 받는다. */
static int parse_backend_address(const char *text, char *host_storage,
                                 struct proxy_backend *backend) {
    const char *host_start = text;
    const char *host_end;
    const char *port_text;

    if (*text == '[') {
        const char *close = strchr(text + 1, ']');
        if (!close || close[1] != ':') return -1;
        host_start = text + 1;
        host_end = close;
        port_text = close + 2;
    } else {
        const char *colon = strrchr(text, ':');
        if (!colon) return -1;
        host_end = colon;
        port_text = colon + 1;
    }

    size_t host_length = (size_t)(host_end - host_start);
    if (host_length == 0 || host_length >= PROXY_CONFIG_ADDRESS_MAX) return -1;
    memcpy(host_storage, host_start, host_length);
    host_storage[host_length] = '\0';

    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (inet_pton(AF_INET, host_storage, &ipv4) != 1 &&
        inet_pton(AF_INET6, host_storage, &ipv6) != 1) return -1;

    uint16_t port;
    if (parse_port(port_text, &port) < 0) return -1;

    backend->host = host_storage;
    backend->port = port;
    return 0;
}

/* 한 줄의 key·주소를 해석해 임시 결과에 추가한다. 성공 0, 형식 오류 -1. */
static int apply_config_line(char *line, unsigned int line_number, const char *path,
                             struct parsed_routes *routes) {
    char *cursor = trim(line);
    if (*cursor == '\0' || *cursor == '#') return 0;

    char *key = next_word(&cursor);
    char *address = next_word(&cursor);
    char *extra = next_word(&cursor);

    int status = -1;
    const char *reason = NULL;
    if (!address) {
        reason = "missing address";
    } else if (extra) {
        reason = "unexpected extra token";
    } else if (strcmp(key, "submit") == 0) {
        if (routes->submission_count == 1) reason = "duplicate submit";
        else if (parse_backend_address(address, routes->submission_host,
                                       &routes->submission) < 0) reason = "invalid submit address";
        else {
            routes->submission_count = 1;
            status = 0;
        }
    } else if (strcmp(key, "lookup") == 0) {
        if (routes->lookup_count >= PROXY_LOOKUP_MAX_BACKENDS) reason = "too many lookup backends";
        else if (parse_backend_address(address, routes->lookup_hosts[routes->lookup_count],
                                       &routes->lookups[routes->lookup_count]) < 0)
            reason = "invalid lookup address";
        else {
            ++routes->lookup_count;
            status = 0;
        }
    } else {
        reason = "unknown key";
    }

    if (status < 0) {
        fprintf(stderr, "%s:%u: %s\n", path, line_number, reason ? reason : "invalid line");
    }
    return status;
}

/* 설정 파일을 처음부터 끝까지 읽는다. 마지막 줄에 개행이 없어도 처리한다. */
static int read_config_file(const char *path, struct parsed_routes *routes) {
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "routes config: %s: cannot open\n", path);
        return -1;
    }

    char line[PROXY_CONFIG_LINE_MAX];
    unsigned int line_number = 0;
    int status = 0;
    while (fgets(line, sizeof(line), file)) {
        ++line_number;
        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n' && !feof(file)) {
            fprintf(stderr, "%s:%u: line too long\n", path, line_number);
            status = -1;
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (apply_config_line(line, line_number, path, routes) < 0) {
            status = -1;
            break;
        }
    }
    if (status == 0 && ferror(file)) {
        fprintf(stderr, "routes config: %s: read error\n", path);
        status = -1;
    }

    fclose(file);
    return status;
}

/* 내장 기본값. 설정 파일이 없을 때 표준 검증 구성(제출 1 + 조회 2)을 그대로 쓴다. */
static int use_builtin_defaults(struct parsed_routes *routes) {
    static const char *const defaults[] = {
        "127.0.0.1:8001", "127.0.0.1:8002", "127.0.0.1:8003"
    };
    if (parse_backend_address(defaults[0], routes->submission_host, &routes->submission) < 0)
        return -1;
    routes->submission_count = 1;
    for (size_t i = 1; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        if (parse_backend_address(defaults[i], routes->lookup_hosts[routes->lookup_count],
                                  &routes->lookups[routes->lookup_count]) < 0) return -1;
        ++routes->lookup_count;
    }
    return 0;
}

/* 검증된 임시 결과를 확정 저장소로 옮기고 라우트별 업스트림을 만든다.
 * 주소를 가리키는 포인터도 확정 저장소를 다시 가리키게 해 임시 버퍼 수명과 분리한다. */
static void commit_routes(const struct parsed_routes *routes) {
    submission_count = routes->submission_count;
    memcpy(submission_host, routes->submission_host, sizeof(submission_host));
    submission_backends[0] = routes->submission;
    submission_backends[0].host = submission_host;

    lookup_count = routes->lookup_count;
    memset(lookup_excluded_until, 0, sizeof(lookup_excluded_until));
    for (size_t i = 0; i < lookup_count; ++i) {
        memcpy(lookup_hosts[i], routes->lookup_hosts[i], sizeof(lookup_hosts[i]));
        lookup_backends[i] = routes->lookups[i];
        lookup_backends[i].host = lookup_hosts[i];
    }

    submission_upstream = (struct proxy_upstream){
        .backends = submission_backends,
        .backend_count = submission_count,
        .max_attempts = 1
    };

    lookup_group = (struct proxy_group_state){
        .next = 0,
        .excluded_until = lookup_excluded_until
    };
    lookup_upstream = (struct proxy_upstream){
        .backends = lookup_backends,
        .backend_count = lookup_count,
        .max_attempts = lookup_count > 1 ? 2 : 1,
        .select_backend = select_available_round_robin_backend,
        .on_connection_result = record_backend_connection_result,
        .group_state = &lookup_group
    };
}

int proxy_config_init(const char *path) {
    struct parsed_routes routes;
    memset(&routes, 0, sizeof(routes));

    int status = path ? read_config_file(path, &routes) : use_builtin_defaults(&routes);
    if (status < 0) return -1;

    if (routes.submission_count != 1) {
        fprintf(stderr, "routes config: exactly one 'submit' line is required\n");
        return -1;
    }
    if (routes.lookup_count == 0) {
        fprintf(stderr, "routes config: at least one 'lookup' line is required\n");
        return -1;
    }

    commit_routes(&routes);
    return 0;
}

const struct proxy_upstream *proxy_config_submission(void) {
    return &submission_upstream;
}

const struct proxy_upstream *proxy_config_lookup(void) {
    return &lookup_upstream;
}
