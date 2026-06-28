#pragma once

/**
 * ディレイバッファ・共通定数など、プラグイン全体で共有するコアユーティリティ。
 */
namespace ods::core {

	// ディレイバッファ
	static constexpr float MAX_DELAY_MS = 10000.0f; ///< ディレイバッファの上限 (ms)

	// RTSP E2E 計測パラメータ
	static constexpr int RTSP_E2E_TIMEOUT_S            = 60; ///< プローブ検出タイムアウト (s)
	static constexpr int RTSP_E2E_READY_TIMEOUT_S      = 15; ///< RTSP 受信開始待機タイムアウト (s)
	static constexpr int RTSP_E2E_MEASURE_SETS_DEFAULT = 5;  ///< 接続・計測・切断のデフォルト反復回数

	// 音声同期オフセット
	static constexpr int REQUIRED_AUDIO_SYNC_OFFSET_MS = -950; ///< 配信に求める親ソースの同期オフセット (ms)

	// UI 色（警告テキスト）
	static constexpr const char *UI_COLOR_WARNING_LIGHT = "#DC2626"; ///< ライトテーマ用警告テキスト色
	static constexpr const char *UI_COLOR_WARNING_DARK  = "#F87171"; ///< ダークテーマ用警告テキスト色

} // namespace ods::core
