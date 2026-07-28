# 07 — Build and flash

Verified on Windows 11 with Zephyr **4.4.1**, its bundled SDK, and a Nordic
J-Link OB (the debugger on an nRF5x DK) wired to the tag's SWD pads.

## Toolchain

A Zephyr workspace. The `ruuvi/ruuvitag` board is in-tree, so nothing extra is
needed for the board itself.

```bash
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.1 zephyr-ws
cd zephyr-ws && west update && west zephyr-export
west sdk install
```

Then, per shell:

```bash
export ZEPHYR_BASE=/path/to/zephyr-ws/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-ws/sdk
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
```

## Build

```bash
west build -p always -b ruuvi_ruuvitag -d build-rfa /path/to/this/repo/firmware
```

Expected footprint on the nRF52832:

```
FLASH:  159840 B / 512 KB   30.5%
RAM:     54108 B /  64 KB   82.6%
```

RAM is the tight one. The BLE peripheral role, the 247-byte ATT MTU, the extra
ACL TX buffers and the 512-sample raw ring account for most of it. A second
connection or a larger ring will not fit.

## Flash

`west flash` defaults to the `nrfutil` runner. If that is not installed, use
`nrfjprog` directly:

```bash
nrfjprog --program build-rfa/zephyr/zephyr.hex --chiperase --verify --reset
```

**`--chiperase` erases the Ruuvi factory firmware and its DFU bootloader.** After
this the tag is SWD-only; there is no over-the-air path back until Ruuvi's
bootloader is restored. They publish it, and SWD is the recovery path either way.

## Reading the tag's logs

There is no exposed UART. Logging goes over SEGGER RTT through the same SWD
link:

```bash
JLinkRTTLogger -Device NRF52832_XXAA -If SWD -Speed 4000 -RTTChannel 0 rtt.log
```

Logging is **deferred** (`CONFIG_LOG_MODE_DEFERRED`), because the software Link
Layer forbids immediate logging — it would block in ISR context and wreck radio
timing. Deferred logging means anything emitted before the logger attaches is
lost, so reset the tag after attaching if you want the boot banner.

It is worth the trouble. RTT is what showed that an advertising mode switch was
succeeding and then never coming back —
[ADR-0004](adr/0004-advertising-is-reasserted-every-slot.md) — two lines of log
that ended a long guess.

## Make sure you are talking to the tag

An nRF5x DK carries its own nRF52832, and so does the tag. If the J-Link has not
switched to the external target you will cheerfully erase the wrong one. Read
the chip's BLE address and compare it against what is on air:

```bash
nrfjprog --memrd 0x100000A4 --n 8    # FICR DEVICEADDR[0..1]
```

The advertised random static address is `DEVICEADDR[1][15:0] : DEVICEADDR[0]`
with the top two bits of the most significant byte set. Other useful reads:

| Address | Meaning |
|---------|---------|
| `0x10001208` | APPROTECT. `0xFFFFFFFF` = debug access open |
| `0x10001014` | UICR NRFFW[0], bootloader address. `0xFFFFFFFF` = no bootloader present |
| `0x10000100` | FICR INFO.PART — `0x00052832` for the nRF52832 |

`nrfjprog --readcode` dumps flash to Intel hex; strings in the image identify
the firmware directly (`*** Booting Zephyr OS build v4.4.1 ***`).

## Noise from JLinkARM

`nrfjprog` prints `[error] [SeggerBackend] - JLinkARM.dll reported error -256`
repeatedly when the DLL version does not match what it expects. The operations
still succeed and the data is still correct. Filter those lines rather than
chasing them.
