# AunSync v0.1.0

OBS の音声ソースに挿すフィルタープラグインです。ソロ配信者のローカル生演奏で、観客が聴く配信音と観客が見るアバター動作のタイミングを合わせます。

## モデル

- `R`: OBS 配信遅延（RTSP E2E 計測値）
- `A`: 想定アバター遅延
- `D`: AunSync が出口で付加するディレイ

成立条件は `D + R = A` です。AunSync は `D = max(0, A - R)` をマスターディレイバッファへ適用します。`R > A` の場合、音声を前倒しできないため `D = 0` とし、低遅延な配信サービスへの変更を促します。

親ソースの音声同期オフセットは常に `-950 ms` を要求します。

## 機能

- RTMP URL から RTSP URL を自動導出
- RTSP E2E 計測（サイレント / ミックス）
- 想定アバター遅延の入力
- 出口ディレイの自動適用
- AunSync 用タイミング図
- ffmpeg 自動ダウンロード

## ビルド

OBS Studio のソースと `build_x64` が必要です。

```bat
build.bat D:\dev\obs-studio
```

`build.env` を作る場合:

```ini
OBS_SOURCE_DIR=D:\dev\obs-studio
OBS_LEGACY_INSTALL=0
```
