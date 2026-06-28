#include "plugin/plugin-config.hpp"

#include "plugin/plugin-settings.hpp"

namespace ods::plugin {

	void set_delay_stream_defaults(obs_data_t *settings) {
		if (!settings) return;
		obs_data_set_default_bool(settings, "delay_disable", false);
		obs_data_set_default_int(settings, kAvatarLatencyKey, 200);
		obs_data_set_default_int(settings, kMeasuredRtspE2eKey, 0);
		obs_data_set_default_bool(settings, kRtspE2eMeasuredKey, false);
		obs_data_set_default_bool(settings, "rtmp_url_auto", true);
		obs_data_set_default_string(settings, "rtmp_url", "");
		obs_data_set_default_bool(settings, kRtspUseRtmpUrlKey, true);
		obs_data_set_default_string(settings, kRtspUrlKey, "");
		obs_data_set_default_int(settings, kRtspE2eProbeModeKey, 1);
		obs_data_set_default_string(settings, kFfmpegExePathKey, "auto");
		obs_data_set_default_int(settings, kFfmpegExePathModeKey, static_cast<int>(ExePathMode::Auto));
		obs_data_set_default_int(settings, kSettingsSchemaVersionKey, kSettingsSchemaVersion);
		obs_data_set_default_string(settings, kSettingsSavedVersionKey, PLUGIN_VERSION);
	}

} // namespace ods::plugin
