# AunSync v0.1.0

A native OBS audio filter for solo local live performance. It aligns the broadcast audio heard by the audience with the avatar motion seen by the audience.

## Model

- `R`: OBS broadcast latency measured by RTSP E2E probing
- `A`: estimated avatar latency
- `D`: output delay applied by AunSync

The alignment condition is `D + R = A`. AunSync applies `D = max(0, A - R)` to the master delay buffer. If `R > A`, audio cannot be advanced, so `D = 0` and the UI reports that a lower-latency streaming service is required.

The parent source audio sync offset is always expected to be `-950 ms`.

## Features

- Derive RTSP URL from RTMP URL
- RTSP E2E measurement with silent and mixed probe modes
- Estimated avatar latency input
- Automatic output delay application
- AunSync timing diagram
- Automatic ffmpeg download

## Build

OBS Studio source and `build_x64` are required.

```bat
build.bat D:\dev\obs-studio
```

Optional `build.env`:

```ini
OBS_SOURCE_DIR=D:\dev\obs-studio
OBS_LEGACY_INSTALL=0
```
