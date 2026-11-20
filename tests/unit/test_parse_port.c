#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../../src/server.h"

static void test_valid_ports(void) {
    uint16_t port = 0;

    assert(parse_port("1", &port) == 0 && port == 1);
    assert(parse_port("8080", &port) == 0 && port == 8080);
    assert(parse_port("65535", &port) == 0 && port == 65535);
}

static void test_invalid_ports(void) {
    uint16_t port = 0;

    assert(parse_port(NULL, &port) == -1);
    assert(parse_port("", &port) == -1);
    assert(parse_port("0", &port) == -1);
    assert(parse_port("-1", &port) == -1);
    assert(parse_port("65536", &port) == -1);
    assert(parse_port("99999999999999999999", &port) == -1);
    assert(parse_port("80abc", &port) == -1);
    assert(parse_port("abc", &port) == -1);
    assert(parse_port("+8080", &port) == -1);
    assert(parse_port(" 8080", &port) == -1);
}

int main(void) {
    test_valid_ports();
    test_invalid_ports();
    printf("[PASS] test_parse_port\n");
    return 0;
}
