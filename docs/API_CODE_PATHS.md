# API Code Paths

This document traces the execution path of each public `SMX_*` API function through the SDK internals.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Application Layer                                │
│                                                                         │
│   SMX_Start / SMX_Stop / SMX_GetInputState / SMX_GetConfig / ...        │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         SMXManager (singleton)                           │
│                                                                         │
│   - Owns SMXDevice[2] (one per pad slot)                                │
│   - Spawns USB polling thread + main I/O thread                         │
│   - Handles device discovery and ordering                               │
│   - Routes API calls to the correct SMXDevice                           │
└───────────┬─────────────────────────────────┬───────────────────────────┘
            │                                 │
            ▼                                 ▼
┌───────────────────────────┐   ┌───────────────────────────────────────┐
│   USB Polling Thread       │   │   Main I/O Thread                     │
│   (~1ms cycle)             │   │   (~50ms cycle)                       │
│                            │   │                                       │
│   PollUSBData()            │   │   AttemptConnections()                │
│   ├─ Read HID packets      │   │   Update() per device                 │
│   ├─ Report 3 → atomic     │   │   ├─ CheckReads() [Report 6]         │
│   │  m_iInputState update  │   │   ├─ CheckWrites() [send commands]   │
│   └─ Report 6 → buffer     │   │   ├─ HandlePackets() [config/data]   │
│      for main thread       │   │   ├─ SendConfig() [rate-limited]     │
│                            │   │   ├─ UpdateSensorTestMode()           │
│                            │   │   └─ SendPendingLightsCommands()      │
│   Fires input callback     │   │   CorrectDeviceOrder()               │
│   immediately on change    │   │   Fire Connected callbacks            │
└───────────────────────────┘   └───────────────────────────────────────┘
            │                                 │
            └────────────────┬────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     SMXDeviceConnection                                  │
│                                                                         │
│   - Owns IHIDDevice (one open USB connection)                           │
│   - Command queue (fragment, send, await response)                      │
│   - Report 6 buffer (USB thread → main thread handoff)                  │
│   - Atomic m_iInputState (USB thread writes, anyone reads)              │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     IHIDDevice (abstraction)                             │
│                                                                         │
│   Read() / Write() / Close()                                            │
│   Production: HIDAPIDevice (wraps hidapi)                               │
│   Testing: FakeHIDDevice / ReplayHIDDevice                              │
└─────────────────────────────────────────────────────────────────────────┘
```

## Threading Model

```
┌──────────────────┐         ┌──────────────────┐         ┌──────────────┐
│  Application     │         │  USB Polling      │         │  Main I/O    │
│  Thread          │         │  Thread           │         │  Thread      │
│                  │         │                   │         │              │
│  SMX_GetInput    │◄────────│  atomic store     │         │              │
│  State() reads   │  (lock  │  m_iInputState    │         │              │
│  atomic directly │  free)  │                   │         │              │
│                  │         │                   │         │              │
│  SMX_GetConfig() │─────────│─────────────────────────────│► lock        │
│  acquires lock   │         │                   │         │  m_pLock     │
│                  │         │                   │         │              │
│  SMX_SetConfig() │─────────│─────────────────────────────│► queues      │
│  acquires lock   │         │                   │         │  command     │
│                  │         │                   │         │              │
│  SMX_SetLights2()│─────────│─────────────────────────────│► queues      │
│  acquires lock   │         │                   │         │  lights cmds │
│                  │         │                   │         │              │
│  Callback fires  │◄────────│  input change     │         │              │
│  from USB thread │         │  callback         │         │              │
│                  │         │                   │         │              │
│  Callback fires  │◄────────│─────────────────────────────│  connection/ │
│  from I/O thread │         │                   │         │  config      │
└──────────────────┘         └──────────────────┘         └──────────────┘
```

## API Function Code Paths

### SMX_Start

Initializes the SDK and begins device discovery.

```
SMX_Start(callback, pUser)
│
├─ Guard: if g_pSMX already exists, log warning and return
│
└─ Create SMXManager(callback)
   ├─ CreateHIDAPIEnumerator() → m_pEnumerator
   ├─ m_pEnumerator->Init() (calls hid_init())
   ├─ Initialize SMXDevice[2] with lock, pad index, callbacks
   ├─ Spawn main I/O thread → ThreadMain()
   │   └─ Loop until shutdown:
   │       ├─ AttemptConnections()
   │       │   ├─ Skip if both slots occupied
   │       │   ├─ Rate-limit enumeration to 1/sec
   │       │   ├─ Enumerate(VID=0x2341, PID=0x8037)
   │       │   ├─ Filter by product string "StepManiaX"
   │       │   ├─ Skip already-open paths
   │       │   ├─ Open() → IHIDDevice
   │       │   └─ SMXDevice::OpenDevice(path, device)
   │       │       └─ SMXDeviceConnection::Open()
   │       │           └─ RequestDeviceInfo() [queues device info command]
   │       ├─ Update() each device
   │       ├─ CorrectDeviceOrder() [swap if P2 in slot 0]
   │       ├─ Fire Connected callbacks for newly-connected devices
   │       └─ Wait on condition variable (50ms timeout or Report 6 signal)
   │
   └─ Spawn USB polling thread → USBPollingThreadMain()
       └─ Loop until shutdown:
           ├─ Lock m_Lock
           ├─ PollUSBData() on each device
           │   ├─ Read HID packets (non-blocking)
           │   ├─ Report 3: parse inline, atomic store m_iInputState
           │   └─ Report 6: buffer for main thread
           ├─ Unlock
           ├─ If Report 6 data found → notify main thread condition variable
           └─ Sleep m_iUSBPollingSleepUs (default 1000µs)
```

### SMX_Stop

Shuts down the SDK and disconnects all devices.

```
SMX_Stop()
│
└─ g_pSMX.reset() → ~SMXManager()
   ├─ Deadlock check: abort if called from callback thread
   ├─ Set m_bShutdown = true
   ├─ Notify condition variable (wake main thread)
   ├─ Join main I/O thread
   ├─ Join USB polling thread
   └─ m_pEnumerator->Exit() (calls hid_exit())
```

### SMX_GetInputState

Returns the current panel press bitmask. Lock-free, lowest possible latency.

```
SMX_GetInputState(pad)
│
├─ Guard: if !g_pSMX, return 0
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::GetInputState()
   └─ m_Connection.GetInputState()
      └─ return m_iInputState.load()  ← atomic, no lock
```

### SMX_GetInfo

Returns connection status, serial number, firmware version, player ID.

```
SMX_GetInfo(pad, info)
│
├─ Guard: if !g_pSMX, return
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::GetInfo(info)
   ├─ Lock m_pLock
   ├─ Check IsConnectedLocked() (has device + device info + config)
   ├─ Copy from SMXDeviceInfo: m_bP2, m_Serial, m_iFirmwareVersion
   └─ Determine m_bHasSerialNumber (not all-zeros or all-F's)
```

### SMX_GetConfig

Returns the current device configuration (or pending config if a write is queued).

```
SMX_GetConfig(pad, config)
│
├─ Guard: if !g_pSMX or !config, return false
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::GetConfig(config)
   ├─ Lock m_pLock
   ├─ Check IsConnectedLocked()
   └─ Return m_bSendConfig ? m_WantedConfig : m_Config
       (optimistic read: return pending config if write queued)
```

### SMX_SetConfig

Queues a configuration write to the device (rate-limited to 1/sec).

```
SMX_SetConfig(pad, config)
│
├─ Guard: if !g_pSMX or !config, return
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::SetConfig(config)
   ├─ Lock m_pLock
   ├─ Check IsConnectedLocked()
   ├─ m_WantedConfig = config
   └─ m_bSendConfig = true
       │
       └─ [Later, in main I/O thread: SendConfig()]
          ├─ Check rate limit (1 write/sec for EEPROM protection)
          ├─ Build command:
          │   ├─ fw >= 5: "W" + [size:1] + [SMXConfig:250 bytes]
          │   └─ fw < 5:  "w" + [size:1] + [OldSMXConfig converted]
          ├─ SendCommand(data, callback)
          ├─ Update cached config optimistically
          └─ SendCommand("G" or "g\n") to read back and verify
```

### SMX_FactoryReset

Resets device to factory defaults and re-reads config.

```
SMX_FactoryReset(pad)
│
├─ Guard: if !g_pSMX, return
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::FactoryReset()
   ├─ Lock m_pLock
   ├─ Check IsConnected()
   ├─ SendCommand("f\n")  ← factory reset command
   └─ SendCommand("G" or "g\n")  ← re-read config
       │
       └─ [Response handled in HandlePackets()]
          └─ Parse config response → m_Config, m_bHaveConfig = true
             └─ Fire ConfigUpdated callback
```

### SMX_ForceRecalibration

Triggers immediate sensor recalibration.

```
SMX_ForceRecalibration(pad)
│
├─ Guard: if !g_pSMX, return
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::ForceRecalibration()
   ├─ Lock m_pLock
   ├─ Check IsConnected()
   └─ SendCommand("C\n")  ← recalibration command
```

### SMX_SetSerialNumbers

Assigns random serial numbers to all connected devices.

```
SMX_SetSerialNumbers()
│
├─ Guard: if !g_pSMX, return
└─ SMXManager::SetSerialNumbers()
   ├─ Lock m_Lock
   └─ For each device:
      ├─ GenerateSerial() → 16 random bytes
      └─ SendCommand("s" + [serial:16] + "\n")
```

### SMX_ReenableAutoLights

Re-enables automatic panel lighting on both pads.

```
SMX_ReenableAutoLights()
│
├─ Guard: if !g_pSMX, return
└─ SMXManager::ReenableAutoLights()
   ├─ Lock m_Lock
   └─ For each device:
      └─ SendCommand("S 1\n")
```

### SMX_SetLights2

Sets panel LED colors for both pads (rate-limited to 30 FPS).

```
SMX_SetLights2(lightData, lightDataSize)
│
├─ Guard: if !g_pSMX or !lightData, return
├─ Validate lightDataSize (must be 864 or 1350)
│
└─ SMXManager::SetLights(lightData, lightDataSize)
   ├─ Lock m_Lock
   ├─ Skip if panel test mode is active
   ├─ Determine per-pad byte size from lightDataSize
   ├─ For each pad:
   │   ├─ Point into raw buffer at pad offset (no copy)
   │   ├─ For each of 9 panels:
   │   │   ├─ Outer 4×4 (16 LEDs): lookup-table scale, split into:
   │   │   │   ├─ Top 2 rows (8 LEDs × 3 RGB) → command '2'
   │   │   │   └─ Bottom 2 rows (8 LEDs × 3 RGB) → command '3'
   │   │   └─ Inner 3×3 (9 LEDs): lookup-table scale → command '4'
   │   │       (zeros if 16-LED mode)
   │   └─ Append '\n' to each command
   │
   ├─ Rate limiting (30 FPS):
   │   ├─ If pending queue already has 3 commands, replace data in-place
   │   └─ Otherwise, schedule 3 new PendingLightsCommands:
   │       ├─ Firmware ≥ v4: all at fNow (queue immediately)
   │       └─ Firmware < v4: cmd[1] at fSendCommandAt, cmd[2] at +1/60s
   │
   ├─ Per-pad command assignment:
   │   ├─ masterVersion >= 4: fill '4' command slot
   │   └─ masterVersion < 4: leave '4' command slot empty
   │
   └─ Notify condition variable (wake main thread)
       │
       └─ [In main I/O thread: SendPendingLightsCommands()]
          └─ While queue not empty and fTimeToSend <= now:
             ├─ For each pad: SendCommand(sPadCommand[iPad])
             └─ Erase sent command from queue
```

### SMX_SetLights (deprecated)

Equivalent to `SMX_SetLights2(lightData, 864)`.

### SMX_SetPlatformLights

Sets platform edge LED strip colors (88 LEDs × RGB = 264 bytes total).

```
SMX_SetPlatformLights(pLightData)
│
├─ Guard: if !g_pSMX or !pLightData, return
└─ SMXManager::SetPlatformLights(pLightData)
   ├─ Lock m_Lock
   └─ For each connected device with masterVersion >= 4:
      └─ SendCommand("L" + [strip_index:0] + [num_leds:44] + [RGB data: 132 bytes])
         (pad 0 gets bytes 0-131, pad 1 gets bytes 132-263)
```

### SMX_SetPanelTestMode

Activates panel-side diagnostic lighting (auto-refreshed every ~1 sec).

```
SMX_SetPanelTestMode(mode)
│
├─ Guard: if !g_pSMX, return
└─ SMXManager::SetPanelTestMode(mode)
   ├─ m_PanelTestMode = mode
   └─ Notify condition variable (wake main thread)
       │
       └─ [In main I/O thread: UpdatePanelTestMode()]
          ├─ Skip if mode unchanged and sent < 1 sec ago
          └─ For each device:
             └─ SendCommand("t " + mode_char + "\n")
                (e.g., "t 1\n" for pressure test, "t 0\n" for off)
```

### SMX_SetTestMode / SMX_GetTestData

Enables sensor diagnostics and retrieves raw/calibrated sensor values.

```
SMX_SetTestMode(pad, mode)
│
├─ Guard: if !g_pSMX, return
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::SetSensorTestMode(mode)
   ├─ Lock m_pLock
   └─ m_SensorTestMode = mode
       │
       └─ [In main I/O thread: UpdateSensorTestMode()]
          ├─ Skip if mode == Off
          ├─ Skip if waiting for response (with 2s timeout)
          └─ SendCommand("y" + mode_char + "\n")
             (e.g., "y0\n" for uncalibrated, "y1\n" for calibrated)

SMX_GetTestData(pad, data)
│
├─ Guard: if !g_pSMX or !data, return false
├─ GetDevice(pad) → SMXDevice*
└─ SMXDevice::GetTestData(data)
   ├─ Lock m_pLock
   ├─ Check m_bHaveSensorTestData
   └─ Copy m_SensorTestData → data
```

Sensor test response handling:

```
[Device sends 'y' response packet]
│
└─ HandlePackets() → HandleSensorTestDataResponse(buf)
   ├─ Parse: buf[1] = mode, buf[2] = size
   ├─ Extract interleaved uint16_t data (9 panels multiplexed per bit)
   ├─ For each panel (0-8):
   │   ├─ De-interleave bits into detail_data struct
   │   ├─ Validate signature bits (sig1=0, sig2=1, sig3=0)
   │   ├─ Extract: sensor levels[4], bad sensor flags, DIP switch, bad jumpers
   │   └─ Disable bad sensor flags for FSR (fw >= 5)
   ├─ m_bHaveSensorTestData = true
   └─ Fire SensorTestData callback
```

### SMX_SetPollingRate

Configures thread sleep intervals.

```
SMX_SetPollingRate(iMainThreadMs, iUSBPollingUs)
│
├─ Warn if mainThreadMs > 100 (may delay connections)
└─ SMXManager::SetPollingRate()
   ├─ m_iMainThreadSleepMs.store(iMainThreadMs)  ← atomic
   └─ m_iUSBPollingSleepUs.store(iUSBPollingUs)   ← atomic
```

### SMX_SetInputStateMode

Controls whether input callback fires on every packet or only on state change.

```
SMX_SetInputStateMode(bAlwaysFire)
│
├─ Guard: if !g_pSMX, return
└─ SMXManager::SetInputStateMode(bAlwaysFire)
   └─ For each device:
      └─ m_Connection.SetAlwaysFireInputCallback(bAlwaysFire)
         └─ m_bAlwaysFireInputCallback.store(b)  ← atomic
```

### SMX_SetLogCallback

Sets custom log handler (can be called before SMX_Start).

```
SMX_SetLogCallback(callback)
│
└─ g_LogCallback.store(callback)  ← atomic
```

### SMX_Version / SMX_GetMonotonicTime

Simple getters with no I/O.

```
SMX_Version()        → returns SMX_VERSION string constant
SMX_GetMonotonicTime() → returns chrono::steady_clock elapsed seconds since first call
```

### SMX_LightsAnimation_Load

Loads a GIF as a panel animation for one pad and animation type.

```
SMX_LightsAnimation_Load(gif, size, pad, type, error)
│
├─ Validate inputs (null check, pad range, type enum)
├─ Decode GIF via gif_load (GIF_Load callback composites frames with disposal)
├─ Validate dimensions (must be 14×15 or 23×24)
├─ For each frame:
│   ├─ Check loop frame marker (bottom-left pixel white → set loopFrame)
│   ├─ Store frame duration (snap 30ms/40ms to 1/30s)
│   └─ For each of 9 panels:
│       └─ ExtractPanel():
│           ├─ 14×15: sample 4×4 grid at (col*5, row*5) → 16 LEDs
│           └─ 23×24: sample outer 4×4 at even coords + inner 3×3 at odd → 25 LEDs
├─ Lock g_AnimMutex
├─ Store PanelAnimationData in g_Animations[pad][type]
└─ Reset playback state for all 9 panels
```

### SMX_LightsAnimation_SetAuto

Starts or stops the animation playback thread.

```
SMX_LightsAnimation_SetAuto(enable)
│
├─ enable=true:
│   ├─ If already running, return
│   └─ Spawn AnimationThreadMain():
│       └─ Loop at 30 FPS until shutdown:
│           ├─ Check g_fStopAnimatingUntil (pause if SMX_SetLights2 called directly)
│           ├─ Lock g_AnimMutex
│           ├─ For each pad:
│           │   ├─ Get input state (for pressed animation)
│           │   ├─ For each panel:
│           │   │   ├─ Render released animation frame → output buffer
│           │   │   └─ If pressed: overlay pressed animation frame
│           │   └─ Advance frame timing (timeInFrame += 1/30, wrap at loopFrame)
│           ├─ Unlock
│           ├─ Call SMX_SetLights2_Internal() (bypasses TemporaryStop)
│           └─ Sleep remainder of 33ms frame
│
└─ enable=false:
    ├─ Set g_bAnimShutdown = true
    └─ Join animation thread
```

### SMX_LightsUpload_PrepareUpload

Converts a GIF into firmware upload commands.

```
SMX_LightsUpload_PrepareUpload(gif, size, pad, type, error)
│
├─ Validate inputs, decode GIF, check dimensions (must be 23×24)
├─ Check frame count (max 32)
├─ Extract per-panel frames + detect loop frame marker
├─ For each of 9 panels:
│   ├─ BuildPalette(): extract up to 15 unique colors (black = transparent)
│   ├─ PackGraphic(): pack each frame into 13-byte 4-bit nibble format
│   └─ Apply 0.6666 color scaling to palette
├─ Build master timing: frame indices + 30FPS delay counts + loop frame
├─ Generate upload command sequence:
│   ├─ CreateUploadPackets() for each panel's graphics + palette
│   ├─ Interleave across panels (one packet per panel per burst)
│   ├─ Insert delay commands (max_size × 3.4ms) between bursts
│   ├─ Duplicate panel data for reliability
│   └─ Append master timing packets (final_packet=1 on last)
└─ Append command list to g_UploadCommands[pad]
```

### SMX_LightsUpload_BeginUpload

Queues all prepared upload commands for transmission.

```
SMX_LightsUpload_BeginUpload(pad, callback, pUser)
│
├─ If no commands prepared: callback(100), return
├─ Create shared atomic counter for progress tracking
└─ For each command in g_UploadCommands[pad]:
    └─ SMX_SendCommandForPad(pad, cmd, completion_callback)
        └─ On completion: increment counter, report progress (0-99, then 100 on last)
```

## Device Connection Lifecycle

Note: The device begins sending Report 3 (input state) immediately upon USB connection. There is no "activation" command — the `m_bActive` flag in the SDK is purely internal bookkeeping that gates when the SDK starts processing Report 6 command responses and triggers the initial config read.

```
                    ┌─────────────────┐
                    │   Disconnected   │
                    └────────┬────────┘
                             │ AttemptConnections() finds device
                             │ Open(path, hidDevice)
                             │ (Device already sending Report 3)
                             ▼
                    ┌─────────────────┐
                    │  Pending Info    │  RequestDeviceInfo() sent
                    │  (connected but  │  IsConnected() = false
                    │   no device info)│  IsConnectedWithDeviceInfo() = false
                    │                  │  (Report 3 received but input state
                    │                  │   not yet exposed to application)
                    └────────┬────────┘
                             │ Device info response received
                             │ (PACKET_FLAG_DEVICE_INFO in HandleUsbPacket)
                             │ m_bGotInfo = true
                             ▼
                    ┌─────────────────┐
                    │  Pending Config  │  CheckActive() sets m_bActive=true
                    │  (has info, no   │  and sends "G"/"g\n" to read config
                    │   config yet)    │  IsConnectedWithDeviceInfo() = true
                    │                  │  IsConnected() = false (no config)
                    └────────┬────────┘
                             │ Config response received
                             │ (HandlePackets parses 'g'/'G' packet)
                             │ m_bHaveConfig = true
                             ▼
                    ┌─────────────────┐
                    │  Fully Connected │  IsConnected() = true
                    │                  │  Fire Connected callback
                    │                  │  Input state now exposed via API
                    └────────┬────────┘
                             │ Read error / device unplugged
                             │ CloseDevice()
                             ▼
                    ┌─────────────────┐
                    │   Disconnected   │  Fire Disconnected callback
                    └─────────────────┘
```

## Command Flow (SendCommand → Response)

```
Application calls SendCommand(cmd, callback)
│
├─ Fragment cmd into 64-byte HID packets:
│   ┌──────────────────────────────────────────────────────────────┐
│   │ Packet N:                                                     │
│   │   [0] = 0x05 (Report ID: command)                            │
│   │   [1] = flags (START=0x04 on first, END=0x01 on last)        │
│   │   [2] = payload size (0-61)                                  │
│   │   [3..63] = command payload bytes                            │
│   └──────────────────────────────────────────────────────────────┘
│
├─ Queue PendingCommand with all packets concatenated
│
└─ [Main I/O thread: CheckWrites()]
   ├─ Wait until no command in flight (m_pCurrentCommand == nullptr)
   ├─ Pop front of m_aPendingCommands
   ├─ Write all 64-byte packets sequentially via IHIDDevice::Write()
   ├─ Mark command as sent, start 2-second timeout
   │
   └─ [Wait for response via Report 6 packets]
      ├─ USB polling thread buffers Report 6 → m_sReport6Buffer
      ├─ Main thread: CheckReads() → HandleUsbPacket()
      │   ├─ START_OF_COMMAND (0x04): clear partial buffer
      │   ├─ Accumulate payload into m_sCurrentReadBuffer
      │   ├─ END_OF_COMMAND (0x01): queue complete packet to m_sReadBuffers
      │   └─ HOST_CMD_FINISHED (0x02): invoke callback(response)
      │
      └─ If timeout (2s): retry command (push back to front of queue)
```
