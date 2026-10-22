// helpers/ui/remote_parse.c: responses to the touch UI's remote management requests.
#include "test.h"
#include "ui/remote_parse.h"

#include <cstring>
#include <string>

namespace {
void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }

// RepeaterStats as upstream's repeater sends it (56 bytes).
void stats(uint8_t *d) {
    memset(d, 0, 56);
    put16(&d[0], 4123); put16(&d[2], 2); put16(&d[4], (uint16_t)-118); put16(&d[6], (uint16_t)-91);
    put32(&d[8], 1000); put32(&d[12], 500); put32(&d[16], 77); put32(&d[20], 93784);
    put32(&d[24], 300); put32(&d[28], 200); put32(&d[32], 600); put32(&d[36], 400);
    put16(&d[40], 3); put16(&d[42], (uint16_t)-26); put16(&d[44], 5); put16(&d[46], 9);
    put32(&d[48], 1234); put32(&d[52], 17);
}
}

TEST(rparse_status_repeater, "UNIT-RPARSE-001", "Repeater status: every RepeaterStats field, signed ones signed") {
    uint8_t d[56];
    stats(d);
    remote_status s;
    CHECK(remote_status_parse(d, sizeof(d), false, &s));
    CHECK(s.batt_mv == 4123 && s.tx_queue == 2 && s.noise_floor == -118 && s.last_rssi == -91);
    CHECK(s.pkts_recv == 1000 && s.pkts_sent == 500 && s.air_time_s == 77 && s.uptime_s == 93784);
    CHECK(s.sent_flood == 300 && s.sent_direct == 200 && s.recv_flood == 600 && s.recv_direct == 400);
    CHECK(s.err_events == 3 && s.last_snr_x4 == -26 && s.direct_dups == 5 && s.flood_dups == 9);
    CHECK(s.rx_air_time_s == 1234 && s.recv_errors == 17 && s.posted == 0);
}

TEST(rparse_status_room, "UNIT-RPARSE-002", "Room server status: posts and pushes after the common 48 bytes; short replies keep what they carry") {
    uint8_t d[52];
    stats(d);
    put16(&d[48], 42); put16(&d[50], 7);
    remote_status s;
    CHECK(remote_status_parse(d, sizeof(d), true, &s));
    CHECK(s.posted == 42 && s.post_pushes == 7 && s.rx_air_time_s == 0 && s.uptime_s == 93784);
    CHECK(remote_status_parse(d, 8, false, &s));
    CHECK(s.batt_mv == 4123 && s.last_rssi == -91 && s.pkts_recv == 0 && s.len == 8);
    CHECK(!remote_status_parse(d, 3, false, &s));
}

TEST(rparse_neighbours, "UNIT-RPARSE-003", "Neighbours: request bytes, entries, total, and a truncated entry dropped") {
    uint8_t req[7];
    CHECK(remote_neighbours_request(req, 14) == 7);
    const uint8_t want[7] = {0x06, 0, 14, 0, 0, 0, 4};
    CHECK(memcmp(req, want, 7) == 0);

    uint8_t d[4 + 9 * 2 + 5] = {};
    put16(&d[0], 5); put16(&d[2], 3);
    const uint8_t a[4] = {0xAA, 0xBB, 0xCC, 0xDD}, b[4] = {1, 2, 3, 4};
    memcpy(&d[4], a, 4); put32(&d[8], 60); d[12] = (uint8_t)-20;
    memcpy(&d[13], b, 4); put32(&d[17], 3600); d[21] = 40;
    remote_neighbour n[8];
    int total = 0;
    CHECK(remote_neighbours_parse(d, sizeof(d), &total, n, 8) == 2);
    CHECK(total == 5);
    CHECK(memcmp(n[0].prefix, a, 4) == 0 && n[0].heard_secs_ago == 60 && n[0].snr_x4 == -20);
    CHECK(memcmp(n[1].prefix, b, 4) == 0 && n[1].heard_secs_ago == 3600 && n[1].snr_x4 == 40);
    CHECK(remote_neighbours_parse(d, 3, &total, n, 8) == -1);
}

TEST(rparse_owner, "UNIT-RPARSE-004", "Owner info splits firmware, name and owner; missing fields are empty") {
    const char *txt = "v1.9.0\nRoma-Nord\nLucio, IW0ABC\nextra";
    char buf[64];
    const char *fw, *name, *owner;
    remote_owner_info_parse((const uint8_t *)txt, (uint8_t)strlen(txt), buf, &fw, &name, &owner);
    CHECK(std::string(fw) == "v1.9.0" && std::string(name) == "Roma-Nord" &&
          std::string(owner) == "Lucio, IW0ABC\nextra");
    remote_owner_info_parse((const uint8_t *)"v1", 2, buf, &fw, &name, &owner);
    CHECK(std::string(fw) == "v1" && name[0] == '\0' && owner[0] == '\0');
}

TEST(rparse_duration, "UNIT-RPARSE-005", "Durations: two largest units") {
    char b[16];
    remote_fmt_duration(93784, b, sizeof(b)); CHECK(std::string(b) == "1d 2h");
    remote_fmt_duration(3725, b, sizeof(b));  CHECK(std::string(b) == "1h 2m");
    remote_fmt_duration(65, b, sizeof(b));    CHECK(std::string(b) == "1m 5s");
    remote_fmt_duration(0, b, sizeof(b));     CHECK(std::string(b) == "0s");
}
