# Whistle continuous transcription on the ESP32-P4-WIFI6

Always-on microphone, energy VAD, and one WAV + one JSONL line per accepted
spoken utterance on a Waveshare **ESP32-P4-WIFI6 (basic kit)**.

This is the current, working P4 port. It is a separate app from the S3
push-to-talk demo in `../whistle_demo`; the S3 app is untouched. The shared
engine has an additive P4 branch in `nd_quant.c` and a P4-only
`nd_p4_dot.S`.

- **Board:** Waveshare ESP32-P4-WIFI6, 32 MB flash, 32 MB PSRAM, ES8311 codec
- **Framework:** ESP-IDF **6.1** (5.5 or newer is declared; 6.1 is what was tested)
- **Model:** Whistle, full 16.9 MB `.cact`, decoder depth 4
- **Microphone:** onboard ES8311 analog mic, 16 kHz mono PCM16
- **Storage:** microSD, FAT32, `/sdcard/rec_NNNNN.wav` + `/sdcard/transcripts.jsonl`
- **Control:** none. There is no button and no LED; the mic is always listening.

## Quick start (Windows PowerShell)

From the repository root, in an **ESP-IDF 6.1** shell:

```powershell
New-Item -ItemType Directory -Force model\whistle | Out-Null
curl.exe -L -o model\whistle\whistle.cact https://huggingface.co/Cactus-Compute/whistle/resolve/main/whistle.cact

cd esp32\whistle_p4
idf.py set-target esp32p4
idf.py build
idf.py -p COM8 flash

cd ..\..
python -m esptool --chip esp32p4 -p COM8 -b 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 32MB 0x210000 model\whistle\whistle.cact

cd esp32\whistle_p4
idf.py -p COM8 monitor
```

The model is only needed to run, not to build. Linux/macOS commands, port
names, revision notes and the full verification procedure are below.

## Current status

Verified on hardware (before the latest VAD-gate build):

- 32 MB PSRAM detected at 200 MHz; model head copied to PSRAM, rest mmap'ed
  from the `model` partition.
- ES8311 microphone and I2S capture at 16 kHz.
- P4 PIE int8 dot kernel (`nd_p4_dot.S`) passes its boot self-test:
  `EVT pie_dot ok sum=8832`.
- VAD-segmented speech transcribed by Whistle at depth 4.
- WAV files and JSONL transcripts written to the SD card.
- SD mounted **1-bit / 10 MHz**; 4-bit / 20 MHz was unreliable on the tested
  30 GB card. If 1-bit mounting fails, the app retries 4-bit / 10 MHz.

Implemented in the current binary, to verify on the next flash:

- Quiet-room transients are rejected before Whistle:
  - onset must be above `-65 dBFS` RMS, and
  - the finished clip must peak above `-45 dBFS`;
  - otherwise it prints `EVT reject quiet` and is not written or transcribed.
- Six utterance slots (previously four) and pre-roll peak tracking.

Known limits:

- Whistle is a **whole-clip** model. It cannot emit partial text while someone
  is speaking; the app cuts speech into utterances and transcribes each one.
- The encoder runs at roughly **1.25× real time** on the P4 (14.16 s of audio
  took 17.67 s). End-to-end, that clip took 17.67 s to encode + 8.83 s to
  decode, about **1.9×**. Six utterance slots buffer roughly three minutes of
  continuous speech. A long unbroken lecture can therefore print
  `EVT drop pool_full`; the oldest queued clip is discarded, not written.
- A loud transient (a hand clap, a door) can still pass the level gates and
  produce a short transcript. The gates remove low-level room noise, not
  every impulsive sound.
- Depth 4 is the default. Depth 8 is the full model and is slower.
- Wi-Fi/BLE (ESP32-C6) is not used. Everything runs offline.

## What it does

```
ES8311 mic -> I2S RX -> 20 ms RMS VAD -> utterance slot (PSRAM)
     -> worker: WAV to SD -> Whistle encode/decode -> JSONL to SD + UART
```

- 20 ms VAD frames, 300 ms pre-roll, 500 ms hangover, 200 ms tail.
- The first second after boot is a warm-up used to learn the noise floor.
  Wait for it before speaking.
- Maximum utterance: 30 s. If the PSRAM pool cannot be allocated, the app
  retries with 15 s slots.
- Six slots; speech never waits for inference before being captured. At 30 s
  per slot the pool is about 5.8 MB of PSRAM; the model head uses 2.16 MB and
  the board has 32 MB, so there is ample headroom.
- Each accepted utterance is written **before** inference, so the audio
  survives a model failure or power loss. The transcript is appended after
  decoding.

## Hardware

No wiring. The basic kit has the codec, analog microphone, speaker amplifier
and TF slot onboard. You need:

- the board,
- a USB-C **data** cable to the Type-C connector (power, programming and
  UART console),
- a FAT32 microSD card (30 GB SDHC was tested; exFAT is not supported).

| Function | GPIO |
|---|---|
| ES8311 I2C SDA / SCL | 7 / 8 (address 0x18) |
| I2S MCLK / BCLK / WS | 13 / 12 / 10 |
| I2S DIN (codec -> P4) / DOUT | 11 / 9 |
| PA_CTRL (held low, speaker off) | 53 |
| SD CLK / CMD | 43 / 44 |
| SD D0-D3 | 39 / 40 / 41 / 42 |
| UART0 TX / RX (CH343) | 37 / 38 |

The Type-C connector is the **CH343 UART** (console and flashing). The 4-pin
USB connector is the P4 OTG port; it is not the console.

The TF slot is SDMMC slot 0 with its IO rail from the P4 on-chip LDO channel 4.
The app mounts 1-bit / 10 MHz first and retries 4-bit / 10 MHz; see
“Current status” above.

The codec is configured for 16 kHz, 16-bit, I2S slave, MCLK 256 x Fs
(4.096 MHz), analog mic, +30 dB PGA, ADC on both slots. The capture code takes
the left slot.

The tested board reports chip revision **v3.2**. `sdkconfig.defaults` selects
rev v3.1 or newer (`CONFIG_ESP32P4_REV_MIN_301=y`). For an older v1.x
engineering sample, use the commented rev <3.0 block at the bottom of that
file and rebuild.

## Repository layout

### Minimum tree needed to build `whistle_p4`

```
whistle-esp32/
├── engine/                                  # portable C99 engine (no SDK deps)
│   ├── include/
│   │   ├── nd_cact.h
│   │   ├── nd_grammar.h
│   │   ├── nd_ledcmd.h
│   │   ├── nd_model.h
│   │   ├── nd_quant.h
│   │   ├── nd_sample.h
│   │   ├── nd_tokenizer.h
│   │   └── nd_whistle.h
│   └── src/
│       ├── nd_cact.c                        # .cact v3/v4 container reader
│       ├── nd_grammar.c
│       ├── nd_ledcmd.c                      # compiled by the glob, unused by P4
│       ├── nd_model.c
│       ├── nd_p4_dot.S                      # P4 PIE int8 dot kernel
│       ├── nd_quant.c                       # quantized kernels; P4 branch
│       ├── nd_sample.c
│       ├── nd_tokenizer.c
│       └── nd_whistle.c                     # Whistle forward pass
├── esp32/
│   ├── needle_demo/
│   │   └── components/
│   │       └── needle/
│   │           └── CMakeLists.txt           # REQUIRED: builds engine/ as an IDF component
│   └── whistle_p4/
│       ├── CMakeLists.txt                   # points EXTRA_COMPONENT_DIRS at ../needle_demo/components
│       ├── partitions.csv                   # nvs / phy_init / factory / model
│       ├── sdkconfig.defaults               # P4, 32 MB flash, 32 MB PSRAM, 32-bit addressing
│       ├── dependencies.lock                # recommended: pins IDF 6.1.0
│       ├── README.md                        # this file
│       └── main/
│           ├── CMakeLists.txt
│           ├── idf_component.yml            # idf >= 5.5; no managed components
│           ├── board_p4.h                   # pins, VAD, slot, depth and gain config
│           ├── audio_p4.c / audio_p4.h      # I2S capture, VAD, utterance pool
│           ├── es8311_p4.c / es8311_p4.h    # ES8311 codec bring-up over I2C
│           ├── sd_log.c / sd_log.h          # SDMMC mount, WAV writer, JSONL
│           └── main.c                       # model load, PIE self-test, listen/decode loop
└── model/
    └── whistle/
        └── whistle.cact                     # 16,919,407 bytes; runtime only, not compiled
```

The `needle` component is only a `CMakeLists.txt`; it globs
`engine/src/*.c` and adds `engine/src/nd_p4_dot.S` for the P4. It lives under
`esp32/needle_demo/components/` because the S3 demo and the P4 app share it.
**If you copy only `esp32/whistle_p4`, the build will fail**; keep the
`esp32/needle_demo/components/needle/CMakeLists.txt` path, or change
`EXTRA_COMPONENT_DIRS` in `esp32/whistle_p4/CMakeLists.txt`.

### Full repository (optional parts)

```
esp32/whistle_demo/      S3 push-to-talk speech -> LED demo (unchanged)
esp32/needle_demo/       Needle 2 demo; also owns the shared needle component
esp32/mic_test/          S3 microphone/button check
esp32/led_probe/         S3 LED check
esp32/psram_test/        S3 PSRAM check
host/                    native wh_run, nd_dump, nd_gtest, ledcmd_test
tools/                   model inspection/strip, console UI, evaluation
tests/                   console regression tests
docs/                    wiring diagram
```

None of those optional folders are needed to build or run `whistle_p4`.
`host/` is useful for verifying a WAV outside the board; `tools/whistle_cact.py`
is useful for inspecting the `.cact` file.

For a P4-only repository, `esp32/needle_demo/components/needle` is an odd
place for the shared component. A cleaner layout would be
`esp32/components/needle/`; moving it requires updating `EXTRA_COMPONENT_DIRS`
in `esp32/whistle_p4/CMakeLists.txt` and the `../../../../engine` relative
path in the component CMakeLists. The current tree keeps the S3 demo working
unchanged, so it was left as-is.

## Build on a fresh PC

### 1. Install ESP-IDF 6.1

Use Espressif's Installation Manager (EIM) and install **ESP-IDF v6.1** with
the ESP32-P4 toolchain. Then open the **"ESP-IDF 6.1 PowerShell"** shortcut
(Windows) or source `export.sh` (Linux/macOS). Verify:

```powershell
idf.py --version
```

The project declares `idf: ">=5.5"` in `main/idf_component.yml` as the
component-manager floor. The 18 MB model partition needs the 32-bit flash
mapping Kconfig used here, which was validated with **6.1**; use 6.1 unless
you are prepared to adapt the bootloader configuration for an older IDF.

Linux/macOS from git, if you do not use EIM:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
. ./export.sh
```

If the `v6.1` tag is not available in your mirror, use EIM or the matching
`release/v6.1` branch.

### 2. Get the source

```bash
git clone <your-repository-url> whistle-esp32
cd whistle-esp32
```

or copy the folder from the machine that already builds it. Do **not** copy
`build/`, `sdkconfig`, `managed_components/` or `.venv/`; they are generated
and machine-specific.

### 3. Fetch the model (runtime only)

The model is not compiled into the app and is deliberately kept out of
`model/` in git (see the handoff checklist below). Download the full 16.9 MB
file once:

```powershell
# Windows PowerShell
New-Item -ItemType Directory -Force model\whistle | Out-Null
curl.exe -L -o model\whistle\whistle.cact https://huggingface.co/Cactus-Compute/whistle/resolve/main/whistle.cact
(Get-Item model\whistle\whistle.cact).Length    # must be 16919407
```

```bash
# Linux / macOS
mkdir -p model/whistle
curl -L -o model/whistle/whistle.cact \
  https://huggingface.co/Cactus-Compute/whistle/resolve/main/whistle.cact
stat -c%s model/whistle/whistle.cact             # 16919407 (Linux)
# macOS: stat -f%z model/whistle/whistle.cact
```

Python alternative:

```bash
python -m pip install huggingface_hub
python -c "from huggingface_hub import hf_hub_download; import shutil, os; os.makedirs('model/whistle', exist_ok=True); shutil.copy(hf_hub_download('Cactus-Compute/whistle','whistle.cact'), 'model/whistle/whistle.cact')"
```

If the file is much smaller than 16,919,407 bytes, you probably saved an HTML
redirect or an error page instead of the weights.

### 4. Build

```powershell
cd esp32\whistle_p4
idf.py set-target esp32p4
idf.py build
```

`set-target` is needed on a fresh checkout. If a stale `sdkconfig` from a
different target is present, remove `sdkconfig` and `sdkconfig.old` first, or
run `idf.py fullclean`, then `idf.py set-target esp32p4` again.

Expected result:

```
whistle_p4.bin binary size 0x6a1d0 bytes (434,640 bytes)
Smallest app partition is 0x200000 bytes. 0x195e30 bytes (79%) free.
Project build complete.
```

The exact size moves by a few hundred bytes with IDF patch versions. The app
partition is 2 MB, so there is ample room.

**The build does not need `model/whistle/whistle.cact`.** The model is flashed
separately and is mmap'ed at run time from its own partition.

### 5. Flash the app, bootloader and partition table

Connect the Type-C port, find the CH343 COM port (Windows Device Manager ->
Ports -> `USB-SERIAL CH343 (COMx)`), then:

```powershell
idf.py -p COM8 flash
```

Use `idf.py flash`, **not** `idf.py app-flash`. The 18 MB model partition
crosses the 16 MB boundary, so the bootloader must be built with 32-bit flash
addressing. `sdkconfig.defaults` already sets:

```ini
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH=y
```

`idf.py flash` writes the bootloader at `0x2000`, the partition table at
`0x8000`, and the app at `0x10000`. If the old bootloader remains, the model
mmap fails with:

```
E flash_mmap: Address 0x00210000 is out of range for 24bit flash mapping
ERR mmap_failed size=18874368
```

Linux/macOS:

```bash
idf.py -p /dev/ttyUSB0 flash
# macOS CH343 usually enumerates as /dev/cu.wchusbserial* or /dev/cu.usbserial-*
```

If auto-reset does not work, hold **BOOT**, tap **RST**, release **BOOT**, and
run the same command.

### 6. Flash the model once

From the repository root (after `idf.py flash` you are in `esp32\whistle_p4`,
so go back up first):

```powershell
cd ..\..
python -m esptool --chip esp32p4 -p COM8 -b 921600 ^
  write-flash --flash-mode dio --flash-freq 80m --flash-size 32MB ^
  0x210000 model\whistle\whistle.cact
```

Linux/macOS:

```bash
cd ../..
python -m esptool --chip esp32p4 -p /dev/ttyUSB0 -b 921600 \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 32MB \
  0x210000 model/whistle/whistle.cact
```

The address `0x210000` is the start of the `model` partition in
`partitions.csv`; the partition is 18 MB (`0x1200000`). The model only needs
flashing once, or again after you erase the flash. At 921600 baud the 16.9 MB
write takes a couple of minutes.

On esptool older than v5, the command is spelled `write_flash`.

### 7. Monitor

```powershell
cd esp32\whistle_p4
idf.py -p COM8 monitor
```

Linux/macOS:

```bash
cd esp32/whistle_p4
idf.py -p /dev/ttyUSB0 monitor
```

or, to flash and monitor in one step after the model is already on the board:

```powershell
idf.py -p COM8 flash monitor
```

Exit the monitor with `Ctrl+]`. The console is UART0 at **115200**; the
Type-C CH343 is the console, not the 4-pin USB OTG port.

## Expected boot and runtime output

```
I (884) SD_HOST: src_freq_hz: 160000000
Name: 00000
Type: SDHC
Speed: 10.00 MHz (limit: 10.00 MHz)
SSR: bus_width=1
EVT sd mounted total_mb=30519 free_mb=30493
EVT sd ready
I (982) es8311: ready: 16 kHz, 16-bit, analog mic, MCLK 256x
EVT audio max_sec=30 slots=6 preroll_ms=300 vad_start_db=9.0 vad_end_db=5.0
EVT pie_dot ok sum=8832
EVT head_in_psram bytes=2164536 ms=75
EVT ready model=whistle depth=4 enc_layers=8 d_model=512 vocab=8199 psram_free=...
EVT  LISTENING - speak; each utterance is transcribed
```

After the one-second warm-up, normal speech produces:

```
EVT speech_start floor_dbfs=-77.5 rms_dbfs=-57.0
EVT speech_end ms=3460 peak_dbfs=-38.8 clipped=0
EVT lang en
TXT You've reached the final lesson in my cryptography essential series.
EVT time audio_ms=3460 encode_ms=3316 decode_ms=2535 tokens=15 peak_dbfs=-38.8
LOG rec_00143.wav
```

A rejected room transient produces only:

```
EVT reject quiet ms=680 peak_dbfs=-58.9
```

`EVT reject quiet` means the utterance was below `P4_VAD_MIN_PEAK_DB`; it was
neither written nor transcribed. `EVT drop pool_full` means the inference
queue was full; the oldest queued utterance was discarded without being
written.

## SD card files

```
/sdcard/rec_00001.wav       16 kHz mono PCM16, 44-byte WAV header
/sdcard/transcripts.jsonl   one JSON object per accepted utterance
```

```json
{"schema":1,"file":"rec_00143.wav","uptime_ms":34567,"lang":"en",
 "text":"You've reached the final lesson...","audio_ms":3460,
 "peak_dbfs":-38.8,"clipped":0,"encode_ms":3316,"decode_ms":2535,
 "tokens":15,"depth":4}
```

- The WAV is written **before** inference.
- The JSONL is appended and closed after decoding.
- The app never formats the card. If it cannot mount, it keeps transcribing to
  UART and prints `EVT sd unavailable`.
- File numbers come from a counter in NVS. `idf.py erase-flash` resets that
  counter; the next run starts at `rec_00001.wav` and will overwrite files of
  the same name. Rename or remove old logs before erasing flash.
- FAT32 only. exFAT cards will not mount.

## Verify a WAV without the board

The WAVs are exactly the format Whistle expects (16 kHz mono PCM16), so the
host engine can transcribe a copy from the card. From the repository root:

```bash
cmake -S host -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host -j
./build-host/wh_run model/whistle/whistle.cact rec_00143.wav --depth 4 --gate 12
```

Windows with the Ninja generator from the IDF tools:

```powershell
cmake -S host -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
.\build-host\wh_run.exe model\whistle\whistle.cact rec_00143.wav --depth 4 --gate 12
```

With a multi-config Visual Studio generator the executable is under
`build-host\Release\wh_run.exe`.

The transcript should match the board's, apart from decoder depth if you
changed `P4_DECODER_DEPTH`.

## Configuration

All app knobs are in `main/board_p4.h` (plus the mount speed in `sd_log.c`).

| Knob | Default | Effect |
|---|---|---|
| `P4_DECODER_DEPTH` | 4 | Whistle ladder depth; 8 is the full model, slower |
| `P4_MAX_UTT_SEC` | 30 | Longest utterance; falls back to 15 s if PSRAM is short |
| `P4_POOL_SLOTS` | 6 | Utterances waiting for / in inference |
| `P4_MIC_ADC_GAIN` | 2 (12 dB) | ES8311 reg 0x16: 0 = 0 dB, 2 = 12 dB, ... 7 = 42 dB |
| `P4_VAD_START_DB` | 9.0 | Onset threshold above the adaptive floor |
| `P4_VAD_END_DB` | 5.0 | Hangover threshold above the adaptive floor |
| `P4_VAD_MIN_RMS_DB` | -65.0 | Absolute onset gate; frames quieter than this cannot start |
| `P4_VAD_MIN_PEAK_DB` | -45.0 | A finished clip quieter than this is rejected |
| `P4_VAD_PREROLL_MS` | 300 | Audio kept before onset |
| `P4_VAD_START_MS` | 60 | Consecutive above-threshold audio before an onset |
| `P4_VAD_END_MS` | 500 | Silence before closing an utterance |
| `P4_VAD_TAIL_MS` | 200 | Audio kept after the last voice frame |
| `P4_VAD_MIN_MS` | 300 | Clips shorter than this are discarded |

If normal speech is rejected, lower `P4_VAD_MIN_PEAK_DB` (e.g. to -50) or
raise `P4_MIC_ADC_GAIN` one step. If room noise still produces text, raise the
peak gate slightly (e.g. -42) and check the `peak_dbfs` values first; do not
raise it above the peak of your quiet speech.

## Performance

Measured on the ESP32-P4-WIFI6, 400 MHz, depth 4, PIE int8 path:

| Audio | Encode | Decode | Total | Ratio |
|---|---|---|---|---|
| 1.80 s | 1.70 s | 0.95 s | 2.65 s | 1.47x |
| 3.46 s | 3.32 s | 2.54 s | 5.86 s | 1.69x |
| 14.16 s | 17.67 s | 8.83 s | 26.50 s | 1.87x |

The encoder dominates and scales with audio length. End-to-end throughput is
roughly **0.55x real time**. Six 30 s slots buffer about three minutes of
continuous speech; long lectures will eventually print `EVT drop pool_full`.
For normal sentence-by-sentence dictation, the queue drains between
utterances.

If you need zero audio loss on long recordings, the correct architecture is
record-first: write one continuous WAV to SD and transcribe segments from the
card in the background. The current app deliberately prioritises low latency
and per-utterance transcripts over unbounded recording.

## Troubleshooting

**`ERR mmap_failed size=18874368`.** The bootloader still uses 24-bit flash
addressing. Confirm `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` and
`CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH=y` in `sdkconfig.defaults`,
run `idf.py fullclean`, rebuild, and flash with `idf.py flash` (not
`app-flash`). The bootloader must be reflashed, not only the app.

**`ERR no_model_partition` or `ERR whistle_open`.** The model is not at
`0x210000`, or the partition table does not match `partitions.csv`. Check the
partition table and reflash `model/whistle/whistle.cact` at `0x210000`.

**No COM port on Windows.** Install the WCH CH343 driver (linked from the
Waveshare wiki) and use the Type-C connector. The 4-pin USB port is OTG, not
the console.

**`idf.py set-target` complains about an existing target / stale config.**
Remove `sdkconfig`, `sdkconfig.old` and `build/`, then rerun
`idf.py set-target esp32p4`.

**`idf.py` fails with a `cryptography` version error.** In the IDF 6.1 venv:

```powershell
python -m pip install "cryptography>=2.1.4,<49"
```

**SD mount fails.** Use FAT32, reseat the card, and try another card. The app
mounts 1-bit / 10 MHz first. If writes fail with
`sdmmc_write_blocks failed (0x107)`, change `mount_try(1, 10000)` in
`sd_log.c` to `mount_try(1, 5000)`, rebuild, and retry.

**Real speech is rejected (`EVT reject quiet`).** Check the printed
`peak_dbfs`. Normal speech in the tested room peaked between -41 and -12
dBFS. Lower `P4_VAD_MIN_PEAK_DB` to -50, or raise `P4_MIC_ADC_GAIN` from 2 to
3, then rebuild.

**Whistle still answers “Thank you.” to noise.** The low-level gates are
working if you see `EVT reject quiet`; a loud impulsive sound can still pass.
Raise `P4_VAD_MIN_PEAK_DB` carefully, or add a spectral/zero-crossing check if
this matters for your environment.

**`EVT drop pool_full`.** The encoder is slower than real time on long
continuous speech. Use shorter utterances, depth 2 for speed (less accurate),
or implement the record-first mode described above.

**Brownout or reset during inference.** The P4 plus codec plus SD card draws
a few hundred mA. Use a supply that holds 5 V; a weak USB port can brown out
during the encoder.

## Git handoff checklist

The repository already exists; this section is only a guide for a future
commit. Nothing was staged or committed while producing this README.

### Commit

| Path | Why |
|---|---|
| `engine/include/`, `engine/src/` | Portable engine; includes `nd_p4_dot.S` and the P4 branch in `nd_quant.c` |
| `esp32/whistle_p4/` | P4 app: `CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`, `main/` |
| `esp32/whistle_p4/dependencies.lock` | Pins the IDF component-manager view (6.1.0); recommended |
| `esp32/needle_demo/components/needle/CMakeLists.txt` | **Required** component registration; easy to lose |
| `host/`, `tools/`, `tests/`, `docs/` | Optional but useful for verification and for the S3 demo |
| `esp32/whistle_demo/`, `esp32/mic_test/`, `esp32/led_probe/`, `esp32/psram_test/` | Optional S3 demos/tests |
| `README.md`, `LICENSE`, `.gitignore` | Project metadata |

### Do not commit

| Path | Why |
|---|---|
| `esp32/whistle_p4/build/`, `build/`, `build-*/` | Generated binaries and Ninja state |
| `esp32/*/sdkconfig`, `esp32/*/sdkconfig.old` | Machine/target-specific; `sdkconfig.defaults` is the source |
| `esp32/*/managed_components/` | Component-manager cache |
| `.venv/`, `__pycache__/`, `*.pyc` | Local Python state |
| `model/` | 16.9 MB weights; download at setup (or use Git LFS / a release asset) |

### The model policy

`model/` is in `.gitignore`. That is deliberate: the app builds without it,
and a 16.9 MB binary blob in normal git history is awkward to update. Keep the
download one-liner in this README, or publish the `.cact` as a release asset.

If you want a one-command clone instead, use Git LFS:

```bash
git lfs install
git lfs track "model/whistle/*.cact"
```

Then remove or negate the `model/` ignore rule and commit the pointer. Do not
commit the same 16.9 MB blob repeatedly as a normal git object.

### The `.gitignore` trap

The current rule is:

```gitignore
/needle/
```

The leading slash matters. It ignores the upstream `needle/` package at the
repo root, but **not** `esp32/needle_demo/components/needle/`. If someone
changes it to `needle/`, the shared component disappears from the clone and
the P4 build fails. Keep the leading slash.

### What a cloner needs

1. The tree in “Minimum tree needed to build `whistle_p4`”.
2. ESP-IDF 6.1.
3. The model downloaded to `model/whistle/whistle.cact` for runtime (not for
   `idf.py build`).
4. A FAT32 microSD card and the board.

That is sufficient to rebuild the app, flash it, and reproduce the SD logs.

## Credits

Whistle and its weights are by Cactus Compute, Apache 2.0. This repository
contains an independent C99 implementation of the engine; the weights are not
included in git. See the top-level `README.md` for the S3 demo, host tools and
the engine verification notes.
