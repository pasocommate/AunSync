#pragma once

#include "plugin/plugin-settings.hpp"

#include <obs-module.h>
#include <string>

namespace ods::model {

	/**
	 * obs_data_t への型安全な読み書きファサード。
	 */
	class SettingsRepo {
		obs_data_t *s_;

	public:

		explicit SettingsRepo(obs_data_t *settings) : s_(settings) {}

		obs_data_t *raw() const { return s_; }

		int avatar_latency_ms() const {
			return static_cast<int>(obs_data_get_int(s_, plugin::kAvatarLatencyKey));
		}
		void set_avatar_latency_ms(int v) {
			obs_data_set_int(s_, plugin::kAvatarLatencyKey, v);
		}

		int measured_rtsp_e2e_ms() const {
			return static_cast<int>(obs_data_get_int(s_, plugin::kMeasuredRtspE2eKey));
		}
		void set_measured_rtsp_e2e_ms(int v) {
			obs_data_set_int(s_, plugin::kMeasuredRtspE2eKey, v);
		}

		bool rtsp_e2e_measured() const {
			return obs_data_get_bool(s_, plugin::kRtspE2eMeasuredKey);
		}
		void set_rtsp_e2e_measured(bool v) {
			obs_data_set_bool(s_, plugin::kRtspE2eMeasuredKey, v);
		}

		bool rtsp_use_rtmp_url() const {
			return obs_data_get_bool(s_, plugin::kRtspUseRtmpUrlKey);
		}
		void set_rtsp_use_rtmp_url(bool v) {
			obs_data_set_bool(s_, plugin::kRtspUseRtmpUrlKey, v);
		}

		std::string rtsp_url() const {
			const char *v = obs_data_get_string(s_, plugin::kRtspUrlKey);
			return v ? v : "";
		}
		void set_rtsp_url(const std::string &v) {
			obs_data_set_string(s_, plugin::kRtspUrlKey, v.c_str());
		}
	};

} // namespace ods::model
