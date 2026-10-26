// The wallpaper receiver: the phone's chunks in, an image in storage out.
//
// Worth testing here rather than on the board because every failure path is a
// path the phone depends on: an answer per lost chunk would drown the Events
// characteristic, and an image committed without its CRC checked is a picture
// nobody chose on the glass.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "halo/audio.hpp"
#include "halo/base64.hpp"
#include "halo/wallpaper.hpp"
#include "harness.hpp"

using namespace halo;

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const uint8_t* data, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < len ? data[i + 1] : 0;
        const uint32_t c = i + 2 < len ? data[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < len ? kAlphabet[(v >> 6) & 63] : '=';
        out += i + 2 < len ? kAlphabet[v & 63] : '=';
    }
    return out;
}

// A sink that remembers everything, including the order: the test cares that the
// old image is still there until commit() lands.
struct FakeSink : wp::Sink {
    std::vector<uint8_t> staging, committed;
    uint16_t committed_w = 0, committed_h = 0, committed_crc = 0;
    int begins = 0, commits = 0, aborts = 0;
    bool fail_begin = false, fail_write = false, fail_commit = false;

    bool begin(uint16_t, uint16_t) override {
        ++begins;
        if (fail_begin) return false;
        staging.assign(wp::kImageBytes, 0);
        return true;
    }
    bool write(uint32_t off, const uint8_t* data, size_t len) override {
        if (fail_write) return false;
        if (off + len > staging.size()) return false;
        std::memcpy(staging.data() + off, data, len);
        return true;
    }
    bool commit(uint16_t w, uint16_t h, uint16_t crc) override {
        ++commits;
        if (fail_commit) return false;
        committed = staging;
        committed_w = w;
        committed_h = h;
        committed_crc = crc;
        return true;
    }
    void abort() override { ++aborts; }
};

std::vector<uint8_t> an_image() {
    std::vector<uint8_t> img(wp::kImageBytes);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>((i * 31 + (i >> 7)) & 0xFF);
    return img;
}

std::string chunk(int wp, int w, int h, uint32_t off, const uint8_t* data, size_t len,
                  const int* crc = nullptr) {
    std::string s = "{\"wp\":" + std::to_string(wp) + ",\"w\":" + std::to_string(w) +
                    ",\"h\":" + std::to_string(h) + ",\"off\":" + std::to_string(off) +
                    ",\"data\":\"" + b64_encode(data, len) + "\"";
    if (crc != nullptr) s += ",\"crc\":" + std::to_string(*crc);
    return s + "}";
}

wp::Answer feed(wp::Receiver& r, const std::string& js) { return r.feed(js.c_str(), js.size()); }

// The whole image in `per` byte chunks, the way the phone sends it. Returns the
// last answer and counts how many answers came back on the way.
wp::Answer send_image(wp::Receiver& r, const std::vector<uint8_t>& img, size_t per, int wp_id,
                      int* answers_on_the_way, uint16_t crc_override = 0, bool override_crc = false) {
    const uint16_t crc = override_crc ? crc_override : crc16_ccitt_false(img.data(), img.size());
    wp::Answer last;
    if (answers_on_the_way != nullptr) *answers_on_the_way = 0;
    for (size_t off = 0; off < img.size(); off += per) {
        const size_t n = off + per > img.size() ? img.size() - off : per;
        const bool final_chunk = off + n == img.size();
        const int crc_value = crc;
        last = feed(r, chunk(wp_id, 800, 480, static_cast<uint32_t>(off), img.data() + off, n,
                             final_chunk ? &crc_value : nullptr));
        if (last.send && !final_chunk && answers_on_the_way != nullptr) ++*answers_on_the_way;
    }
    return last;
}

}  // namespace

// ------------------------------------------------------------------- base64

HALO_TEST(base64_decodes_what_the_phone_encodes) {
    const uint8_t bytes[] = {0x00, 0xFF, 0x10, 0x7A, 0x5C, 0x01, 0xAB};
    for (size_t len = 1; len <= sizeof(bytes); ++len) {
        const std::string encoded = b64_encode(bytes, len);
        uint8_t out[16] = {};
        const size_t n = base64_decode(encoded.c_str(), encoded.size(), out, sizeof(out));
        CHECK_EQ(n, len);
        CHECK(std::memcmp(out, bytes, len) == 0);
    }
}

HALO_TEST(base64_refuses_rubbish_rather_than_decoding_half_of_it) {
    uint8_t out[16] = {};
    CHECK_EQ(base64_decode("AAAA", 3, out, sizeof(out)), size_t(0));       // not a multiple of 4
    CHECK_EQ(base64_decode("AA A", 4, out, sizeof(out)), size_t(0));       // invalid character
    CHECK_EQ(base64_decode("A===", 4, out, sizeof(out)), size_t(0));       // too much padding
    CHECK_EQ(base64_decode("=AAA", 4, out, sizeof(out)), size_t(0));       // padding first
    CHECK_EQ(base64_decode("AA=AAAAA", 8, out, sizeof(out)), size_t(0));   // padding mid string
    CHECK_EQ(base64_decode("", 0, out, sizeof(out)), size_t(0));
    CHECK_EQ(base64_decode("AAAAAAAA", 8, out, 2), size_t(0));             // would not fit
}

// ------------------------------------------------------------------- sizes

HALO_TEST(only_the_two_full_canvas_sizes_are_accepted) {
    CHECK(wp::size_is_supported(800, 480));
    CHECK(wp::size_is_supported(480, 800));
    CHECK(!wp::size_is_supported(792, 272));   // the development panel, not the canvas
    CHECK(!wp::size_is_supported(800, 272));
    CHECK(!wp::size_is_supported(0, 0));
}

// ------------------------------------------------------------- the happy path

HALO_TEST(a_whole_image_arrives_is_checked_and_is_stored) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    int on_the_way = -1;
    const wp::Answer a = send_image(r, img, 141, 7, &on_the_way);  // 141 bytes: MTU 256

    CHECK(a.send);
    CHECK(a.ok);
    CHECK_EQ(int(a.wp), 7);
    CHECK_EQ(on_the_way, 0);  // the phone hears nothing until the end
    CHECK_EQ(sink.commits, 1);
    CHECK_EQ(sink.committed.size(), size_t(wp::kImageBytes));
    CHECK(sink.committed == img);
    CHECK_EQ(int(sink.committed_w), 800);
    CHECK_EQ(int(sink.committed_h), 480);
    CHECK(!r.busy());
}

HALO_TEST(the_image_is_stored_exactly_as_sent_not_inverted) {
    // 1 means white on the wire, in flash and in the phone's preview. The display
    // list's BitmapCmd is the opposite, and that inversion happens at draw time,
    // in the backend, so these bytes stay comparable with what the phone sent.
    FakeSink sink;
    wp::Receiver r(sink);
    std::vector<uint8_t> img(wp::kImageBytes, 0xFF);  // all white
    send_image(r, img, 300, 1, nullptr);
    CHECK_EQ(int(sink.committed[0]), 0xFF);
    CHECK_EQ(int(sink.committed[wp::kImageBytes - 1]), 0xFF);
}

HALO_TEST(a_portrait_wallpaper_is_accepted_too) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    const uint16_t crc = crc16_ccitt_false(img.data(), img.size());
    for (size_t off = 0; off < img.size(); off += 480) {
        const size_t n = off + 480 > img.size() ? img.size() - off : 480;
        const int c = crc;
        const bool last = off + n == img.size();
        feed(r, chunk(3, 480, 800, static_cast<uint32_t>(off), img.data() + off, n, last ? &c : nullptr));
    }
    CHECK_EQ(sink.commits, 1);
    CHECK_EQ(int(sink.committed_w), 480);
    CHECK_EQ(int(sink.committed_h), 800);
}

// ------------------------------------------------------------------ failures

HALO_TEST(a_lost_chunk_is_asked_for_once_and_then_the_receiver_stays_quiet) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    const size_t per = 141;

    feed(r, chunk(9, 800, 480, 0, img.data(), per));
    // chunk at `per` never arrives; the next six land at the wrong offset.
    int answers = 0;
    for (int i = 2; i <= 7; ++i) {
        const uint32_t off = static_cast<uint32_t>(per * i);
        const wp::Answer a = feed(r, chunk(9, 800, 480, off, img.data() + off, per));
        if (a.send) {
            ++answers;
            CHECK(!a.ok);
            CHECK(a.resend);
            CHECK_EQ(a.resend_from, uint32_t(per));  // where we actually are
        }
    }
    CHECK_EQ(answers, 1);
}

HALO_TEST(the_phone_resends_from_where_we_are_and_the_image_still_completes) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    const size_t per = 141;
    const uint16_t crc = crc16_ccitt_false(img.data(), img.size());

    feed(r, chunk(9, 800, 480, 0, img.data(), per));
    const uint32_t lost = static_cast<uint32_t>(per);
    const wp::Answer asked = feed(r, chunk(9, 800, 480, lost + per, img.data() + lost + per, per));
    CHECK(asked.resend);

    for (size_t off = asked.resend_from; off < img.size(); off += per) {
        const size_t n = off + per > img.size() ? img.size() - off : per;
        const int c = crc;
        const bool last = off + n == img.size();
        feed(r, chunk(9, 800, 480, static_cast<uint32_t>(off), img.data() + off, n, last ? &c : nullptr));
    }
    CHECK_EQ(sink.commits, 1);
    CHECK(sink.committed == img);
}

HALO_TEST(a_wrong_crc_restarts_the_transfer_and_never_commits) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    const wp::Answer a = send_image(r, img, 300, 2, nullptr, 0x1234, true);
    CHECK(a.send);
    CHECK(!a.ok);
    CHECK(a.resend);
    CHECK_EQ(a.resend_from, uint32_t(0));  // the CRC covers everything, so start over
    CHECK_EQ(sink.commits, 0);
    CHECK(sink.committed.empty());

    // Starting over at offset 0 works, with the same transfer ID.
    int on_the_way = -1;
    const wp::Answer again = send_image(r, img, 300, 2, &on_the_way);
    CHECK(again.ok);
    CHECK_EQ(on_the_way, 0);
    CHECK(sink.committed == img);
}

HALO_TEST(an_image_that_ends_short_is_not_committed) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    const int crc = crc16_ccitt_false(img.data(), img.size());
    feed(r, chunk(4, 800, 480, 0, img.data(), 300));
    const wp::Answer a = feed(r, chunk(4, 800, 480, 300, img.data() + 300, 300, &crc));
    CHECK(a.send);
    CHECK(!a.ok);
    CHECK(a.resend);
    CHECK_EQ(a.resend_from, uint32_t(600));
    CHECK_EQ(sink.commits, 0);
}

HALO_TEST(a_size_the_panel_cannot_show_is_refused_for_good_with_no_resend) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    const wp::Answer a = feed(r, chunk(5, 792, 272, 0, img.data(), 100));
    CHECK(a.send);
    CHECK(!a.ok);
    CHECK(!a.resend);   // no amount of resending makes it the right size
    CHECK_EQ(sink.begins, 0);
}

HALO_TEST(a_chunk_for_a_transfer_the_device_never_started_asks_for_the_whole_image) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    const wp::Answer a = feed(r, chunk(6, 800, 480, 4230, img.data(), 141));
    CHECK(a.send);
    CHECK(!a.ok);
    CHECK(a.resend);
    CHECK_EQ(a.resend_from, uint32_t(0));
    CHECK_EQ(sink.begins, 0);
}

HALO_TEST(a_new_transfer_id_abandons_the_one_in_flight) {
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    feed(r, chunk(1, 800, 480, 0, img.data(), 300));
    CHECK(r.busy());
    int on_the_way = -1;
    const wp::Answer a = send_image(r, img, 300, 2, &on_the_way);
    CHECK(a.ok);
    CHECK_EQ(sink.aborts, 1);
    CHECK_EQ(sink.begins, 2);
    CHECK_EQ(on_the_way, 0);
}

HALO_TEST(a_disconnect_abandons_a_half_written_image) {
    // Transfer IDs start over on a reconnect, so a half written image must never
    // be finished by the next connection's chunks.
    FakeSink sink;
    wp::Receiver r(sink);
    const std::vector<uint8_t> img = an_image();
    feed(r, chunk(1, 800, 480, 0, img.data(), 300));
    r.reset();
    CHECK(!r.busy());
    CHECK_EQ(sink.aborts, 1);
    const int crc = crc16_ccitt_false(img.data(), img.size());
    const wp::Answer a = feed(r, chunk(1, 800, 480, 300, img.data() + 300, 300, &crc));
    CHECK(a.resend);
    CHECK_EQ(a.resend_from, uint32_t(0));
    CHECK_EQ(sink.commits, 0);
}

HALO_TEST(storage_that_fails_is_reported_and_never_half_commits) {
    const std::vector<uint8_t> img = an_image();
    {
        FakeSink sink;
        sink.fail_begin = true;
        wp::Receiver r(sink);
        const wp::Answer a = feed(r, chunk(1, 800, 480, 0, img.data(), 300));
        CHECK(a.send);
        CHECK(!a.ok);
        CHECK(!a.resend);
        CHECK(!r.busy());
    }
    {
        FakeSink sink;
        wp::Receiver r(sink);
        feed(r, chunk(1, 800, 480, 0, img.data(), 300));
        sink.fail_write = true;
        const wp::Answer a = feed(r, chunk(1, 800, 480, 300, img.data() + 300, 300));
        CHECK(a.send);
        CHECK(!a.ok);
        CHECK(!r.busy());
    }
    {
        FakeSink sink;
        sink.fail_commit = true;
        wp::Receiver r(sink);
        const wp::Answer a = send_image(r, img, 300, 1, nullptr);
        CHECK(a.send);
        CHECK(!a.ok);
        CHECK(sink.committed.empty());
    }
}

HALO_TEST(a_chunk_without_the_fields_the_contract_names_is_refused) {
    FakeSink sink;
    wp::Receiver r(sink);
    const char* no_wp = "{\"w\":800,\"h\":480,\"off\":0,\"data\":\"AAAA\"}";
    const wp::Answer a = r.feed(no_wp, std::strlen(no_wp));
    CHECK(!a.send);   // nobody to answer: the phone matches answers by wp

    const char* no_off = "{\"wp\":1,\"w\":800,\"h\":480,\"data\":\"AAAA\"}";
    const wp::Answer b = r.feed(no_off, std::strlen(no_off));
    CHECK(b.send);
    CHECK(!b.ok);
    CHECK(!b.resend);

    const char* no_data = "{\"wp\":1,\"w\":800,\"h\":480,\"off\":0}";
    const wp::Answer c = r.feed(no_data, std::strlen(no_data));
    CHECK(c.send);
    CHECK(!c.ok);
}
