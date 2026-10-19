#include "halo/screens.hpp"

namespace halo {
namespace {

// Up to four lines on any screen the device owns. More than that and it is not a
// device screen any more, it is a document.
constexpr uint8_t kMaxLines = 4;
const char* g_lines[kMaxLines];
char g_battery_line[kLocalMaxLineChars + 1];

void set(ScreenModel& out, const char* status, const char* const* lines, uint8_t count,
         const char* hint = nullptr) {
    out.status_line = status;
    out.lines = lines;
    out.line_count = count;
    out.hint = hint;
}

// Writes "Battery at NN%" without pulling in stdio for one number.
const char* battery_line(uint8_t pct) {
    const char* prefix = "Battery at ";
    size_t n = 0;
    while (prefix[n] != '\0' && n + 1 < sizeof(g_battery_line)) {
        g_battery_line[n] = prefix[n];
        ++n;
    }
    char digits[4];
    int d = 0;
    uint8_t v = pct;
    do {
        digits[d++] = static_cast<char>('0' + (v % 10));
        v = static_cast<uint8_t>(v / 10);
    } while (v != 0 && d < 3);
    while (d-- > 0 && n + 1 < sizeof(g_battery_line)) g_battery_line[n++] = digits[d];
    if (n + 1 < sizeof(g_battery_line)) g_battery_line[n++] = '%';
    g_battery_line[n] = '\0';
    return g_battery_line;
}

}  // namespace

bool local_screen(State s, const DeviceStatus& st, ScreenModel& out) {
    out.state = s;
    switch (s) {
        case State::BootRecovery:
            if (st.pending_execution) {
                g_lines[0] = "Checking something you";
                g_lines[1] = "confirmed earlier";
                set(out, "Starting up", g_lines, 2);
            } else {
                g_lines[0] = "Starting up";
                set(out, "Starting up", g_lines, 1);
            }
            return true;

        case State::Ready:
            g_lines[0] = "Hold the side button";
            g_lines[1] = "and say what you need";
            set(out, st.phone_linked ? "Ready" : "Ready, phone not linked", g_lines, 2,
                "Try: block 3 to 4 tomorrow");
            return true;

        case State::Wallpaper:
            // No status bar: a wallpaper is the whole screen. The picture the user
            // chose replaces this in F1; until it is in flash there is nothing to
            // show, and a name is better than a blank panel that reads as dead.
            g_lines[0] = "HaloAI";
            set(out, nullptr, g_lines, 1);
            return true;

        case State::PhoneCheck:
            g_lines[0] = "Reaching your phone";
            set(out, "Connecting", g_lines, 1);
            return true;

        case State::PhoneDisconnected:
            g_lines[0] = "Your phone is not";
            g_lines[1] = "connected";
            set(out, "Not connected", g_lines, 2, "Open HaloAI, then press to go back");
            return true;

        case State::Recording:
            g_lines[0] = "Listening...";
            set(out, "Listening", g_lines, 1, "Let go when you are done");
            return true;

        case State::RecordAddition:
            g_lines[0] = "Listening...";
            g_lines[1] = "Adding to your request";
            set(out, "Listening", g_lines, 2, "Let go when you are done");
            return true;

        case State::FinalizeAudio:
            g_lines[0] = "Sending what you said";
            set(out, "Sending", g_lines, 1);
            return true;

        case State::CriticalBattery:
            g_lines[0] = battery_line(st.battery_pct);
            g_lines[1] = "Charge the device";
            if (st.pending_execution) {
                // Never let a low battery hide an action whose outcome nobody
                // knows yet. It is reconciled before the device returns to READY.
                g_lines[2] = "One action is unconfirmed";
                set(out, "Battery critical", g_lines, 3);
            } else {
                set(out, "Battery critical", g_lines, 2);
            }
            return true;

        default:
            // The phone owns this screen. Inventing words for it would put text
            // on the glass that the backend never wrote and never checked.
            return false;
    }
}

}  // namespace halo
