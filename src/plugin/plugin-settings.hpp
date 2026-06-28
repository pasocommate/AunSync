#pragma once

#include <obs-module.h>
#include <string>

namespace ods::plugin {

	struct DelayStreamData;

	inline constexpr char kAvatarLatencyKey[]         = "avatar_latency_ms";
	inline constexpr char kMeasuredRtspE2eKey[]       = "measured_rtsp_e2e_ms";
	inline constexpr char kRtspE2eMeasuredKey[]       = "rtsp_e2e_measured";
	inline constexpr char kRtspUrlKey[]               = "rtsp_url";
	inline constexpr char kRtspUseRtmpUrlKey[]        = "rtsp_url_use_rtmp";
	inline constexpr char kFfmpegExePathKey[]         = "ffmpeg_exe_path";
	inline constexpr char kFfmpegExePathModeKey[]     = "ffmpeg_exe_path_mode";
	inline constexpr char kRtspE2eProbeModeKey[]      = "rtsp_e2e_probe_mode";
	inline constexpr char kSettingsSchemaVersionKey[] = "settings_schema_version";
	inline constexpr char kSettingsSavedVersionKey[]  = "settings_saved_version";
	inline constexpr char kPathModeFromEnvPath[]      = "%PATH%";
	inline constexpr int  kSettingsSchemaVersion      = 2;

	enum class ExePathMode {
		Auto     = 0, ///< 既定配置先から自動取得（未配置ならダウンロード）
		FromPath = 1, ///< `%PATH%` から実行ファイルを探索する
		Absolute = 2, ///< 絶対パスを手動指定する
	};

	ExePathMode normalize_exe_path_mode(int raw_mode);

	/// マスターディレイを再計算して DelayBuffer へ適用する。
	void recalc_all_delays(DelayStreamData *d);

	/// OBS 設定値を `DelayStreamData` に反映する。
	void apply_settings(DelayStreamData *d, obs_data_t *settings);

	/// 保存済み設定の必須キー・型・バージョン整合性を検証する。
	bool validate_settings_compatibility(obs_data_t *settings, std::string &reason);

} // namespace ods::plugin
