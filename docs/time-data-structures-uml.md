# src の時間関連主要データ構造（UML）

`src` 配下で「時間（delay / latency）」を扱う主要なデータ構造を mermaid の `classDiagram` で整理したもの。

```mermaid
classDiagram
    direction LR

    class DelayBuffer {
        +sample_rate_ : uint32_t
        +channels_ : uint32_t
        +delay_ms_ : uint32_t
        +delay_samples_ : size_t (atomic)
        +write_pos_ : size_t
        +buf_ : vector~float~
        +init(sample_rate, channels, max_delay_ms)
        +set_delay_ms(ms)
        +get_delay_ms() uint32_t
        +process(input, output, frames)
    }

    class RtspE2eResult {
        +valid : bool
        +latency_ms : double
        +min_latency_ms : double
        +max_latency_ms : double
        +error_msg : string
    }

    class RtspE2eMeasureState {
        +prober : RtspE2eProber
        +apply_result(r)
        +result() RtspE2eResult
        +is_measuring() bool
        +completed_sets() int
        +total_sets() int
        +last_error() string
    }

    class DelayState {
        +measured_rtsp_e2e_ms : int
        +rtsp_e2e_measured : bool
        +avatar_latency_ms : int
        +calc_all_delays() DelaySnapshot
    }

    class DelaySnapshot {
        +master_delay_ms : int
        +service_too_slow : bool
        +rtsp_e2e_measured : bool
        +audio_sync_offset_ms : int
    }

    class DelayStreamData {
        +delay : DelayState
        +master_buf : DelayBuffer
        +rtsp_e2e_measure : RtspE2eMeasureState
        +sample_rate : uint32_t
        +channels : uint32_t
    }

    DelayStreamData *-- DelayState : delay
    DelayStreamData *-- DelayBuffer : master_buf
    DelayStreamData *-- RtspE2eMeasureState : rtsp_e2e_measure

    RtspE2eMeasureState --> RtspE2eResult : result_
    DelayState --> DelaySnapshot : calc_all_delays()
```

## 補足

- `DelayStreamData` がランタイム中の設定値（`avatar_latency_ms`, `measured_rtsp_e2e_ms`）と各機能（`RtspE2eMeasureState`）を集約する。
- 実際の音声遅延適用は `DelayBuffer`（`master_buf`）で行われる。
- 計測値は `RtspE2eMeasureState` → `RtspE2eResult` に保持される。

## ディレイ計算式

`calc_all_delays()` で実行する。

```
R = measured_rtsp_e2e_ms   (RTSP E2E 計測結果)
A = avatar_latency_ms      (想定アバター遅延)

service_too_slow = rtsp_e2e_measured && (R > A)
master_delay_ms  = service_too_slow ? 0 : max(0, A - R)
```

`service_too_slow` が真のときはディレイ調整不能（配信遅延 R が想定アバター遅延 A を超えている）。
この場合は UI でエラーを表示し、より低遅延な配信サービスへの切り替えを促す。
