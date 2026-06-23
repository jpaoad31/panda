# Welcome to panda

panda speaks CAN and CAN FD, and it runs on the [STM32H725](https://www.st.com/resource/en/reference_manual/rm0468-stm32h723733-stm32h725735-and-stm32h730-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf).

## Fork: RoadStud USB-NCM bridge

This is a fork of comma's panda with one addition: a dedicated **USB-CDC-NCM firmware**
(`board/bridge/`) that turns a panda into a direct USB-C CAN interface for the
**RoadStud** iOS driver-assistance app. The panda
enumerates as a USB Ethernet device, runs a small lwIP/DHCP stack, and streams CAN over
UDP (the panda packet format) to `192.168.4.1:5555`, so the app can communicate with a car
over an existing native iOS interface. It
also tunnels the panda's **native control transfers** over the same socket (an "RSCP"
request/response routed through `comms_control_handler`), so the host gets the full
control surface — health, set-safety-mode, heartbeat, CAN speed, OBD mux — that the
proprietary USB driver exposed, including actuation (a 1 Hz safety/heartbeat tick gates
controls just like stock).

**Status: streaming validated on hardware; actuation in progress.** A red panda flashed
with this firmware streams live CAN over USB-NCM to a host. The important target is a
**2017 Toyota RAV4** (a supported car): read-only is validated — CAN decodes to the correct
vehicle state at ~3k frames/s — and the RSCP control plane (health round-trip, safety-mode
+ heartbeat gating) works. **End-to-end actuation on the RAV4 isn't working yet**; the
remaining issue appears to be app-side. (A Hyundai was also smoke-tested over OBD at ~2k frames/s.) Build,
flash (incl. a recoverable dev bootstub with a boot-time flash window), and comms-test
tooling are documented in **[`board/bridge/README.md`](board/bridge/README.md)**.

Everything else is stock panda — the safety model and standard firmware are unchanged;
the bridge is a separate, optional build target.

## Directory structure

```
.
├── board           # Code that runs on the STM32
│   └── bridge      # RoadStud USB-NCM bridge firmware (this fork's addition)
├── python          # Stock Python userspace library (USB driver — unused by the bridge)
├── tests           # Tests for panda
├── scripts         # Miscellaneous used for panda development and debugging
├── examples        # Example scripts for using a panda in a car
```

## Safety Model

panda is compiled with vehicle-specific safety logic provided by [opendbc](https://github.com/commaai/opendbc). See details about the car safety models, safety testing, and code rigor in that repository.

## Code Rigor

The panda firmware is written for its use in conjunction with [openpilot](https://github.com/commaai/openpilot). The panda firmware, through its safety model, provides and enforces the
[openpilot safety](https://github.com/commaai/openpilot/blob/master/docs/SAFETY.md). Due to its critical function, it's important that the application code rigor within the `board` folder is held to high standards.

These are the [CI regression tests](https://github.com/commaai/panda/actions) we have in place:
* A generic static code analysis is performed by [cppcheck](https://github.com/danmar/cppcheck/).
* In addition, [cppcheck](https://github.com/danmar/cppcheck/) has a specific addon to check for [MISRA C:2012](https://misra.org.uk/) violations. See [current coverage](https://github.com/commaai/panda/blob/master/tests/misra/coverage_table).
* Compiler options are strict: the flags `-Wall -Wextra -Wstrict-prototypes -Werror` are enforced.
* The [safety logic](https://github.com/commaai/panda/tree/master/opendbc/safety) is tested and verified by [unit tests](https://github.com/commaai/panda/tree/master/opendbc/safety/tests) for each supported car variant.
to ensure that the behavior remains unchanged.
* A hardware-in-the-loop test verifies panda's functionalities on all active panda variants, including:
  * additional safety model checks
  * compiling and flashing the bootstub and app code
  * receiving, sending, and forwarding CAN messages on all buses
  * CAN loopback and latency tests through SPI

The above tests are themselves tested by:
* a [mutation test](tests/misra/test_mutation.py) on the MISRA coverage
* a [mutation test]([tests/misra/test_mutation.py](https://github.com/commaai/opendbc/blob/master/opendbc/safety/tests/mutation.sh)) on the vehicle-specific safety logic

## Connecting over the network

The bridge firmware makes the panda **plug-and-play over USB-C** — no custom USB driver,
libusb, udev rules, or Python library. Plugged into a host, it enumerates as a **USB
Ethernet (CDC-NCM)** adapter and brings up a small lwIP + DHCP stack:

- The panda is **`192.168.4.1`** and hands the host a `192.168.4.2`–`.4` DHCP lease. The
  lease advertises **no gateway or DNS**, so the host keeps its own WiFi/cellular as the
  default route — the panda is just an extra local link.
- **CAN streams as UDP** to **`192.168.4.1:5555`**. Each datagram is a batch of frames in
  a compact 6-byte-header wire format; to send CAN to the car the host sends datagrams
  back to the same socket, and the panda safety model gates what actually transmits.
- **Control rides the same socket** via **RSCP** (request magic `"RSCP"`, response
  `"RSCR"`), routed through the panda's own `comms_control_handler` — health, version,
  set-safety-mode, heartbeat, CAN speed, OBD mux. Health is also **pushed** unsolicited at
  ~2 Hz so the host never has to poll for it.

So a host connects with nothing more than a UDP socket: take the DHCP lease, open a socket
to `192.168.4.1:5555`, and you're reading and writing CAN. Because it's plain UDP over a
standard network interface, it's also trivially observable — `tcpdump`/Wireshark, or a
few-line probe in any language.

The wire format, the RSCP opcodes, the health-push layout, the OBD-vs-NORMAL CAN routing,
and how to **build + flash** (the bridge builds separately from the stock firmware) are all
documented in **[`board/bridge/README.md`](board/bridge/README.md)**.

## Licensing

panda software is released under the MIT license unless otherwise specified.
