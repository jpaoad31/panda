#!/usr/bin/env python3
# Flash the signed RoadStud NCM bridge app (board/obj/panda_bridge.bin.signed).
# reconnect=False is mandatory: after flashing the panda reboots into the NCM app,
# which does not speak the panda USB protocol, so the default reconnect would hang.
#
# Pi usage:  ~/pandaenv/bin/python board/bridge/tools/flash_app.py
import os, sys

_repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, os.path.dirname(_repo))            # so `import panda` = this repo
_odbc = os.path.expanduser("~/opendbc")
if os.path.isdir(_odbc):
    sys.path.insert(0, _odbc)

from panda import Panda  # noqa: E402

APP = os.path.join(_repo, "board/obj/panda_bridge.bin.signed")
assert os.path.isfile(APP), f"missing {APP} — build first (pi_build.sh)"

print(f"flashing {APP} ...")
Panda().flash(APP, reconnect=False)
print("app flashed. Panda is rebooting into the NCM bridge (watch the LED ladder).")
