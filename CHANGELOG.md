# Changelog

## 0.1.0

- `obs-delay-stream` v8.2.0 をベースに AunSync へリブランド。
- 共演者チャンネル管理、WS 音声配信、受信 Web アプリ、cloudflared トンネル、URL 共有、WS 計測、RTMP プローバをビルド対象から削除。
- ディレイ計算を `D = max(0, A - R)` に単純化。
- 親ソースの音声同期オフセット案内を `-950 ms` 固定に変更。
- RTSP E2E 計測、RTMP→RTSP URL 自動導出、ffmpeg 自動ダウンロードを維持。
- AunSync 用の微調整 UI とタイミング図へ縮小。
