"""Find the board's serial port.

The number is not stable across replugs, and a hard coded one fails in a way that
looks like a dead board rather than a moved port. Override with HALO_PORT.
"""
import glob
import os


def find_port() -> str:
    override = os.environ.get("HALO_PORT")
    if override:
        return override
    ports = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.wchusbserial*"))
    if not ports:
        raise SystemExit("no board found: no /dev/cu.usbserial-* port. "
                         "Check the cable is data capable and the board is plugged in.")
    if len(ports) > 1:
        print(f"several ports found, using {ports[0]} (set HALO_PORT to choose)")
    return ports[0]
