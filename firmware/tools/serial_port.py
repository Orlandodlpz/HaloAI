"""Find the board's serial port.

The number is not stable across replugs, and a hard coded one fails in a way that
looks like a dead board rather than a moved port. Override with HALO_PORT.

The two boards do not even enumerate alike. The development board's CH340 bridge
appears as /dev/cu.usbserial-* (or wchusbserial on some driver versions); the
badge has no bridge at all, so the ESP32-S3's own USB-Serial/JTAG peripheral
appears as /dev/cu.usbmodem*. Globbing only the first pair is why every bench
tool reported "no board found: check the cable is data capable" with the badge
plugged in and working.
"""
import glob
import os

# Bridges first: if both boards are plugged in at once, the development board is
# the one being used deliberately, and the badge is reachable by name anyway.
PATTERNS = (
    "/dev/cu.usbserial-*",
    "/dev/cu.wchusbserial*",
    "/dev/cu.usbmodem*",
)


def find_port() -> str:
    override = os.environ.get("HALO_PORT")
    if override:
        return override
    ports: list[str] = []
    for pattern in PATTERNS:
        ports.extend(sorted(glob.glob(pattern)))
    if not ports:
        raise SystemExit(
            "no board found: nothing matching /dev/cu.usbserial-*, "
            "/dev/cu.wchusbserial* or /dev/cu.usbmodem*. Check the cable is data "
            "capable and the board is plugged in. The badge's port can also "
            "disappear on its own when no battery is attached, because its PMU "
            "drops the system rail and the USB peripheral is inside the chip: "
            "give it a minute before blaming the cable.")
    if len(ports) > 1:
        print(f"several ports found, using {ports[0]} (set HALO_PORT to choose)")
    return ports[0]
