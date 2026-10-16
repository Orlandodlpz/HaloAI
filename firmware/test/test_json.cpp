// The BLE messages from BRIEFING section 2, built and read on a computer.
#include "halo/json.hpp"
#include "harness.hpp"

#include <cstring>

using namespace halo;

HALO_TEST(json_reads_an_audio_ack) {
    const char* ack = R"({"req": 12, "seg": 0, "upto": 57})";
    int32_t v = 0;
    CHECK(json::get_int(ack, strlen(ack), "req", &v) && v == 12);
    CHECK(json::get_int(ack, strlen(ack), "seg", &v) && v == 0);
    CHECK(json::get_int(ack, strlen(ack), "upto", &v) && v == 57);
    CHECK(!json::get_int(ack, strlen(ack), "resend_from", &v));
}

HALO_TEST(json_reads_a_negative_upto) {
    // upto is -1 before chunk 0 has arrived. Reading that as 1, or failing to
    // read it, would make the device think the phone had the first chunk.
    const char* ack = R"({"req":1,"seg":0,"upto":-1})";
    int32_t v = 99;
    CHECK(json::get_int(ack, strlen(ack), "upto", &v));
    CHECK(v == -1);
}

HALO_TEST(json_reads_resend_from_and_final) {
    const char* ack = R"({"req":12,"seg":0,"upto":140,"final":true})";
    bool f = false;
    int32_t v = 0;
    CHECK(json::get_bool(ack, strlen(ack), "final", &f) && f);
    CHECK(json::get_int(ack, strlen(ack), "upto", &v) && v == 140);

    const char* gap = R"({"req":12,"seg":0,"upto":57,"resend_from":58})";
    CHECK(json::get_int(gap, strlen(gap), "resend_from", &v) && v == 58);
    CHECK(!json::get_bool(gap, strlen(gap), "final", &f));
}

HALO_TEST(json_reads_settings) {
    const char* s = R"({"phone_ready":true,"orientation":"landscape","tz":"America/Chicago"})";
    bool ready = false;
    char buf[32];
    CHECK(json::get_bool(s, strlen(s), "phone_ready", &ready) && ready);
    CHECK(json::get_str(s, strlen(s), "orientation", buf, sizeof(buf)));
    CHECK(strcmp(buf, "landscape") == 0);
    CHECK(json::get_str(s, strlen(s), "tz", buf, sizeof(buf)));
    CHECK(strcmp(buf, "America/Chicago") == 0);
}

HALO_TEST(json_does_not_confuse_a_key_with_a_value_that_looks_like_one) {
    const char* s = R"({"status_line":"upto: 9","upto":4})";
    int32_t v = 0;
    CHECK(json::get_int(s, strlen(s), "upto", &v));
    CHECK(v == 4);
}

HALO_TEST(json_truncates_a_long_string_instead_of_overflowing) {
    const char* s = R"({"a":"0123456789abcdef"})";
    char buf[8];
    CHECK(json::get_str(s, strlen(s), "a", buf, sizeof(buf)));
    CHECK(strlen(buf) == 7);
    CHECK(strcmp(buf, "0123456") == 0);
}

HALO_TEST(json_rejects_an_unterminated_string) {
    const char* s = "{\"a\":\"never closed";
    char buf[32];
    CHECK(!json::get_str(s, strlen(s), "a", buf, sizeof(buf)));
}

HALO_TEST(json_writes_a_hello_event) {
    char buf[192];
    json::Writer w(buf, sizeof(buf));
    w.key_str("type", "hello");
    w.key_str("fw", "f0-dev");
    w.key_int("boot", 123456);
    w.key_str("state", "READY");
    w.key_int("req", 7);
    w.key_null("sid");
    w.key_null("pending_key");
    w.key_int("battery", 82);
    const char* out = w.finish();
    CHECK(out != nullptr);
    CHECK(strcmp(out,
                 "{\"type\":\"hello\",\"fw\":\"f0-dev\",\"boot\":123456,\"state\":\"READY\","
                 "\"req\":7,\"sid\":null,\"pending_key\":null,\"battery\":82}") == 0);
}

HALO_TEST(json_writes_negative_numbers_and_escapes) {
    char buf[128];
    json::Writer w(buf, sizeof(buf));
    w.key_int("n", -42);
    w.key_bool("b", false);
    w.key_str("s", "a\"b\\c");
    const char* out = w.finish();
    CHECK(out != nullptr);
    CHECK(strcmp(out, R"({"n":-42,"b":false,"s":"a\"b\\c"})") == 0);
}

HALO_TEST(json_reports_overflow_instead_of_writing_broken_json) {
    char buf[16];
    json::Writer w(buf, sizeof(buf));
    w.key_str("a_very_long_key", "and a long value too");
    CHECK(w.finish() == nullptr);
    CHECK(w.overflowed());
}

HALO_TEST(json_counts_and_reads_array_elements) {
    const char* doc = R"({"sid":1,"screen_lines":["a","b","c"],"candidates":[]})";
    const size_t n = strlen(doc);
    CHECK(json::array_len(doc, n, "screen_lines") == 3);
    CHECK(json::array_len(doc, n, "candidates") == 0);
    CHECK(json::array_len(doc, n, "missing") == 0);
    CHECK(json::array_len(doc, n, "sid") == 0);  // not an array

    char out[16] = {};
    CHECK(json::get_str_at(doc, n, "screen_lines", 0, out, sizeof(out)) && !strcmp(out, "a"));
    CHECK(json::get_str_at(doc, n, "screen_lines", 2, out, sizeof(out)) && !strcmp(out, "c"));
    CHECK(!json::get_str_at(doc, n, "screen_lines", 3, out, sizeof(out)));
    CHECK(!json::get_str_at(doc, n, "candidates", 0, out, sizeof(out)));
    CHECK(!json::get_str_at(doc, n, "missing", 0, out, sizeof(out)));
}

HALO_TEST(json_refuses_an_array_that_was_never_closed) {
    // A write truncated in flight must read as nothing, not as the lines that did
    // arrive: a half delivered REVIEW screen is one the user never saw whole.
    const char* cut = R"({"sid":1,"screen_lines":["a","b")";
    CHECK(json::array_len(cut, strlen(cut), "screen_lines") == 0);
    const char* cut_mid = R"({"sid":1,"screen_lines":["a","unterm)";
    CHECK(json::array_len(cut_mid, strlen(cut_mid), "screen_lines") == 0);
    char out[16] = {};
    CHECK(!json::get_str_at(cut_mid, strlen(cut_mid), "screen_lines", 1, out, sizeof(out)));
}

HALO_TEST(json_array_whitespace_does_not_change_what_is_read) {
    const char* doc = "{\"screen_lines\" : [ \"a\" , \"b\" ] }";
    CHECK(json::array_len(doc, strlen(doc), "screen_lines") == 2);
    char out[16] = {};
    CHECK(json::get_str_at(doc, strlen(doc), "screen_lines", 1, out, sizeof(out)));
    CHECK(!strcmp(out, "b"));
}
