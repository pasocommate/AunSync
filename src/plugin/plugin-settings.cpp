#include "plugin/plugin-settings.hpp"

#include "model/settings-repo.hpp"
#include "plugin/plugin-helpers.hpp"
#include "plugin/plugin-services.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"

#include <algorithm>

namespace ods::plugin {

	ExePathMode normalize_exe_path_mode(int raw_mode) {
		switch (raw_mode) {
		case static_cast<int>(ExePathMode::Auto):
			return ExePathMode::Auto;
		case static_cast<int>(ExePathMode::FromPath):
			return ExePathMode::FromPath;
		case static_cast<int>(ExePathMode::Absolute):
			return ExePathMode::Absolute;
		default:
			return ExePathMode::Auto;
		}
	}

	void recalc_all_delays(DelayStreamData *d) {
		if (!d) return;
		const auto snap = d->delay.calc_all_delays();
		d->master_buf.set_delay_ms(static_cast<uint32_t>(std::max(0, snap.master_delay_ms)));
	}

	void apply_settings(DelayStreamData *d, obs_data_t *settings) {
		if (!d || !settings) return;

		d->enabled.store(!obs_data_get_bool(settings, "delay_disable"), std::memory_order_relaxed);
		d->rtmp_url_auto.store(obs_data_get_bool(settings, "rtmp_url_auto"), std::memory_order_relaxed);
		if (obs_data_get_bool(settings, "rtmp_url_auto")) {
			maybe_autofill_rtmp_url(settings, false);
		}
		if (obs_data_get_bool(settings, kRtspUseRtmpUrlKey)) {
			const char       *rtmp = obs_data_get_string(settings, "rtmp_url");
			const std::string rtsp = to_rtsp_url_from_rtmp(rtmp ? rtmp : "");
			if (!rtsp.empty())
				obs_data_set_string(settings, kRtspUrlKey, rtsp.c_str());
		}

		ods::model::SettingsRepo repo(settings);
		d->delay.measured_rtsp_e2e_ms = std::max(0, repo.measured_rtsp_e2e_ms());
		d->delay.rtsp_e2e_measured    = repo.rtsp_e2e_measured();
		d->delay.avatar_latency_ms    = std::clamp(repo.avatar_latency_ms(), 0, 5000);
		if (d->delay.avatar_latency_ms != repo.avatar_latency_ms()) {
			repo.set_avatar_latency_ms(d->delay.avatar_latency_ms);
		}

		recalc_all_delays(d);
	}

	bool validate_settings_compatibility(obs_data_t *settings, std::string &reason) {
		reason.clear();
		if (!settings) return true;

		const int schema = static_cast<int>(obs_data_get_int(settings, kSettingsSchemaVersionKey));
		if (schema > kSettingsSchemaVersion) {
			reason = "settings schema is newer than this plugin";
			return false;
		}

		const int avatar = static_cast<int>(obs_data_get_int(settings, kAvatarLatencyKey));
		if (avatar < 0 || avatar > 5000) {
			reason = "avatar_latency_ms out of range";
			return false;
		}

		const int rtsp = static_cast<int>(obs_data_get_int(settings, kMeasuredRtspE2eKey));
		if (rtsp < 0 || rtsp > static_cast<int>(ods::core::MAX_DELAY_MS)) {
			reason = "measured_rtsp_e2e_ms out of range";
			return false;
		}

		const int mode = static_cast<int>(obs_data_get_int(settings, kRtspE2eProbeModeKey));
		if (mode < 0 || mode > 1) {
			reason = "rtsp_e2e_probe_mode out of range";
			return false;
		}
		return true;
	}

} // namespace ods::plugin
