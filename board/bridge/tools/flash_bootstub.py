#!/usr/bin/env python3
# Install the RoadStud dev bootstub (board/obj/bootstub.panda_bridge.bin) via ST DFU.
# This is the always-recoverable bootstub (15 s flash window every boot). Run once.
#
# Pi usage:  ~/pandaenv/bin/python board/bridge/tools/flash_bootstub.py
import os, sys, time

_repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, os.path.dirname(_repo))            # so `import panda` = this repo
_odbc = os.path.expanduser("~/opendbc")
if os.path.isdir(_odbc):
    sys.path.insert(0, _odbc)                         # comma opendbc clone

from panda import Panda, PandaDFU  # noqa: E402

BOOTSTUB = os.path.join(_repo, "board/obj/bootstub.panda_bridge.bin")
assert os.path.isfile(BOOTSTUB), f"missing {BOOTSTUB} — build first (pi_build.sh)"

# If a normal panda is running, ask it to enter the ST ROM DFU bootloader.
try:
    p = Panda()
    print("panda running; entering DFU...")
    p.reset(enter_bootstub=True)
    p.reset(enter_bootloader=True)
except Exception as e:
    print(f"(no running panda to command into DFU — maybe already in DFU: {e})")

time.sleep(1.5)
dfus = PandaDFU.list()
print("DFU devices:", dfus)
assert dfus, "no DFU device found — hold/short the BOOT pin or check the cable"

d = PandaDFU(dfus[0])
d.program_bootstub(open(BOOTSTUB, "rb").read())
d.reset()
print("dev bootstub installed + reset. It now opens a ~15 s flash window every boot.")
