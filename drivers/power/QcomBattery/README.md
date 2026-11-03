# PMI8998 battery and charging

`QcomBattery` supplies PMI8998 FG Gen3 telemetry through the native XNU
`IOPMPowerSource`. Version 1.1 adds protected SMB2 charge-enable control.
It preserves the retained charge voltage/current limits, JEITA, safety timers,
FG profile, USB input limits and OTG/Type-C ownership.

## Control

The board must supply both `qcom,battery-observer` and the little-endian
version cell `qcom,battery-charge-control = <1>`. The latter grants ownership
of **SID 2, CHARGING_ENABLE_CMD (0x1042), bit 0**, and **CHGR_CFG2
(0x1051), bits 7:6** after ExitBootServices.
Without that cell the driver remains read-only. Startup does not change the
inherited enable command or source. On the first request, pin-controlled
charging is handed to software using the original Xiaomi SMB2 initialization:
stage and verify an off command, clear only the source/polarity bits, verify
the complete configuration and off command, then apply the requested policy.
Each command checks the PMI8998 identity and mode; unrelated bits are preserved
and the result is read back. A failed handoff cannot proceed to an enable write.

Administrator-only `IORegistryEntrySetCFProperties` requests on either
`QcomBattery` or `QcomBatteryPowerSource` accept these keys:

| Key | Type | Meaning |
| --- | --- | --- |
| `ChargingEnabled` | Boolean | Manual permission; `false` inhibits charging |
| `IsCharging` | Boolean | Native system permission, independent of the manual vote |
| `PredictiveChargingInhibit` | Boolean | Native predictive-charging inhibit |
| `ChargeLimit` | Integer, 50–100 | Stop at the threshold; resume two percentage points below it; 100 removes this software limit |

All request fields are validated before changing state. Unknown fields are
rejected, including raw register access and adapter-current controls.
A request acknowledges the applied policy; it does not promise positive net
battery current. Enabling requires a fresh coherent sample, an installed
battery, external power, valid temperature in 0–45 °C and no reported charger
fault. Hardware protection remains active. Requests are combined: a manual
enable cannot cancel a system inhibit, predictive inhibit or capacity limit.

After the first control request, policy and telemetry run every second.
Unavailable telemetry inhibits charging. A transport/write/readback failure
latches a fault and attempts to prove the charger disabled. Later successful
polls cannot re-enable it. An explicit `ChargingEnabled` request can recover
only after a verified disable, followed by fresh telemetry and policy checks.
An unreadable or inaccessible PMIC may prevent even the disable command;
`ChargeControlFault=true` reports that condition without claiming it is off.

Suspend and stop inhibit a managed charger. Resume reads fresh telemetry
before applying the retained policy. These software votes are RAM state,
not persistent settings across reboot. This driver does not implement charge
while powered off, battery-profile provisioning, USB PD/QC negotiation or an
increase to the adapter input-current budget.

## Diagnostics and image integration

```sh
./project.sh build client battery
batteryctl
batteryctl --summary
batteryctl --system
batteryctl --limit 80
batteryctl --charging off
batteryctl --charging on --limit 100
```

The `batteryctl` commands run on the device. `--summary` avoids a full registry
dump on a slow console. `--system` uses the iOS
`IOPSCopyPowerSourcesInfoPrecise` API, as in Apple's
[pmset](https://github.com/apple-oss-distributions/PowerManagement/blob/main/pmset/pmset.m),
and reports the native status and source count when unavailable. An empty
coarse IOPS result alone does not establish failed powerd discovery.
The desktop complete-image pipeline
builds and signs `batteryctl`, installs it at `/usr/bin/batteryctl`, and includes
its signature in the existing System-overlay engineering trust cache.
Building the client alone does not install it or update the running kernel.
Ordinary device diagnostics use the project's USB console; UART is optional.
`batteryctl.dylib` provides the same entry point as a Bash loadable builtin
for an explicitly trusted development session.

`ChargeEnablePinControlled` identifies a retained hardware enable source.
`ChargeCommandEnabled` is the sampled software request, `IsCharging` describes
the charger phase, and `BatteryNetCharging` requires positive measured battery
current. A charger can be active while system load exceeds its input budget.
`InputCurrentLimited` and `ThermalChargingLimited` expose SMB2 status without
guessing an adapter rating. `BatteryProfileVerified` remains false.

`ChargeControlError`: 0 success, 1 inactive/not ready, 2 unsupported
chip/control mode, 3 I/O failure, 4 readback mismatch, 5 latched fault.
`ChargeInhibitReasons` is a bitmask: 1 manual, 2 system, 4 predictive,
8 capacity, 16 unavailable telemetry/suspended, 32 absent input/battery,
invalid temperature or protection fault, 64 latched transport fault.
Inspect the current telemetry and fault flag after any failed request.
`ChargeControlReadbackValid` and `ChargeControlCommandEnabled` distinguish
a verified off command from an unconfirmed disable attempt.

## iOS 18 evidence

The original 22H374 j171ap kernelcache has SHA-256
`aa65c36c486a8b5cb1d7a2d565d1d6b3cc228b3c172f8c69c5da664ee19832cf`.
Its `AppleARMPMUCharger` inherits `AppleARMPMUPowerSource`, which in turn
uses `IOPMPowerSource`. Native `setProperties` at `0xfffffff005e5ea68`
dispatches through the workloop. The gated charger handler at
`0xfffffff005e54a64` processes `IsCharging` at `0xfffffff005e55858`
and `PredictiveChargingInhibit` at `0xfffffff005e55898` as independent
inhibit bits. This implementation uses those property semantics and the
project's native XNU superclass, without binding Apple PMU functions to
Qualcomm registers.

The original `/System/Library/CoreServices/powerd.bundle/powerd` has SHA-256
`872fd84e4d8a112d15f6f1caec8d9696fd7c8540d379043e1921b8a5558c3eb4`.
Its first-match registration at `0x1000364b0` matches `IOPMPowerSource`;
callback `0x100038d70` registers general-interest notifications and reads
the power source. The reader at `0x100008fa4` checks `BatteryInstalled`,
`CurrentCapacity` and `MaxCapacity`. The control path at `0x100048a2c`
writes `IsCharging` with `IORegistryEntrySetCFProperty`. These are static
observations, not proof that the currently running powerd has consumed a
new sample. `batteryctl --system` checks that separately.

Qualcomm register evidence is pinned by
`local/reference/qcom/msm8998/battery/sources.json`: `smb-reg.h` and
`smblib_chg_disable_vote_callback` in Xiaomi's `smb-lib.c` identify 0x1042
bit 0. `qpnp-smb2.c::smb2_init_hw` in the pinned USB references clears
`CHGR_CFG2` bits 7:6 for active-high software control. The remaining charger
configuration is retained here.

## Verification

```sh
PYTHONPATH=tools python3 -m unittest tools.project.tests.test_battery_control -v
./project.sh build drivers --driver GenericClockController --driver QcomPowerController --driver QcomSPMI --driver QcomBattery
./project.sh build kernelcache --collection battery --output /absolute/path/kernelcache.battery
```

Tests execute the production register reader/control logic and the actual
`QcomBattery.cpp` in an IOKit host harness under ASan/UBSan. They cover votes,
capacity hysteresis, lost ACKs, read/write failures, readback mismatch,
coherent-sample races, privilege/type validation, delayed source publication,
notification deduplication, sleep/wake, stop and partial-start rollback.
The real board DTS is also compiled and converted to Apple DT to check the
permission cell's byte order. Kext compilation and collection verification
use the real kernel SDK and original firmware provider.

These checks do not establish real charge-current gain, thermal behavior,
USB reconnection, or iOS system power-source consumption. The running device
must load the new driver and DT before those can be measured.

## Device validation, 2026-11-02

A temporary Fastboot bridge boot retained the device's current 99-Kext image,
including its newer GPU, display and touch modules. Only the battery module,
its two version strings and the ARMv8 scan metadata changed. The resulting
cache passed the structural, dependency, scalar-constant, ISA and crypto checks.
The boot partition was not rewritten.

The retained enable source was pin-controlled. A protected enable request
successfully selected software control and verified the command on. Off/on
requests both returned success and reported the corresponding command readback.
The source reported positive net current of 214 mA, 3741 mV and 37.1 °C in one
settled sample. SOC progressed from 4% to 5% across the test sessions.

This is not sustained charging qualification: the input-limit flag remained
set, and later desktop-load samples again showed negative net current with the
enable command on. USB sink-current allocation has not been implemented here.
The native iOS Precise query returned `0xe00002bc` with no sources. System-level
battery reporting remains unverified; hardware control success does not prove
powerd/SpringBoard integration. Rebooting returns to the retained boot image.
