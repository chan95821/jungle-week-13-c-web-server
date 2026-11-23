#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stdint.h>
#include "http.h"

#define STATIC_URL_PREFIX "/static/"

/* 파일 내용 전체를 보관하지 않는다. 열린 파일, 전송할 바이트 수, MIME만 보관한다.
 * fd >= 0이면 호출한 handle_client가 소유한다. 전송 성공/실패와 관계없이 한 번 닫는다.
 * rootfd는 main에서 빌리므로 이 구조체나 하위 함수가 닫으면 안 된다. */
struct static_file {
    int fd;
    uintmax_t length;
    const char *content_type;
};

/* main이 시작 시 호출한다. 성공하면 루트 디렉터리 FD, 실패하면 -1과 errno.
 * 루트 열기 실패는 서버 설정 오류이며 HTTP 404 응답을 보내는 상황이 아니다. */
int open_static_root(const char *path);

/* 완성된 URL path slice를 받는다. 문자열 종료 문자를 요구하지 않고 원본을 바꾸지 않는다.
 * 성공 0, 실패 HTTP 상태 코드. 실패 시 out->fd는 -1이며 중간 FD는 모두 회수한다. */
int prepare_static_file(int rootfd, struct http_slice path, struct static_file *out);

/* 디코딩된 파일명의 마지막 확장자를 사용한다. 반환값은 해제하지 않는 문자열 상수다. */
const char *mime_from_path(const char *path);

#endif
