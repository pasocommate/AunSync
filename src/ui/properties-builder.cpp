#include "ui/properties-builder.hpp"

#include "core/constants.hpp"
#include "core/string-format.hpp"
#include "model/settings-repo.hpp"
#include "network/rtsp-e2e-prober.hpp"
#include "plugin/plugin-helpers.hpp"
#include "plugin/plugin-services.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "widgets/help-callout-widget.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

#define T_(s) obs_module_text(s)

namespace ods::ui {

	using ods::plugin::DelayStreamData;
	using namespace ods::core;

	namespace {

		static constexpr int64_t REQUIRED_AUDIO_SYNC_OFFSET_NS =
			static_cast<int64_t>(ods::core::REQUIRED_AUDIO_SYNC_OFFSET_MS) * 1000000LL;
		static constexpr char kEmptyAbsolutePathSentinel[] = "__AUNSYNC_EMPTY_ABSOLUTE_PATH__";

		std::string build_exe_path_hint(obs_data_t *settings) {
			if (!settings) return "auto";
			const auto mode = ods::plugin::normalize_exe_path_mode(
				static_cast<int>(obs_data_get_int(settings, ods::plugin::kFfmpegExePathModeKey)));
			if (mode == ods::plugin::ExePathMode::Auto) return "auto";
			if (mode == ods::plugin::ExePathMode::FromPath) return ods::plugin::kPathModeFromEnvPath;
			const char *raw = obs_data_get_string(settings, ods::plugin::kFfmpegExePathKey);
			if (!raw || !*raw) return kEmptyAbsolutePathSentinel;
			if (_stricmp(raw, "auto") == 0 || _stricmp(raw, ods::plugin::kPathModeFromEnvPath) == 0)
				return kEmptyAbsolutePathSentinel;
			return raw;
		}

		void sync_rtsp_url_from_rtmp_if_needed(obs_data_t *settings) {
			if (!settings || !obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey)) return;
			const char       *raw_rtmp = obs_data_get_string(settings, "rtmp_url");
			const std::string rtsp_url =
				ods::plugin::to_rtsp_url_from_rtmp(raw_rtmp ? raw_rtmp : "");
			if (!rtsp_url.empty())
				obs_data_set_string(settings, ods::plugin::kRtspUrlKey, rtsp_url.c_str());
		}

		void sync_rtsp_url_enabled(obs_properties_t *props, obs_data_t *settings) {
			if (!props || !settings) return;
			const bool use_rtmp = obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey);
			if (auto *rtsp_p = obs_properties_get(props, ods::plugin::kRtspUrlKey))
				obs_property_set_enabled(rtsp_p, !use_rtmp);
		}

		bool cb_rtmp_url_auto_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !settings) return false;
			const bool auto_new = obs_data_get_bool(settings, "rtmp_url_auto");
			d->rtmp_url_auto.store(auto_new, std::memory_order_relaxed);
			if (auto_new) ods::plugin::maybe_autofill_rtmp_url(settings, true);
			sync_rtsp_url_from_rtmp_if_needed(settings);
			if (auto *p = obs_properties_get(props, "rtmp_url"))
				obs_property_set_enabled(p, !auto_new);
			sync_rtsp_url_enabled(props, settings);
			return false;
		}

		bool cb_rtsp_use_rtmp_changed(void *, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			if (!settings) return false;
			if (obs_data_get_bool(settings, "rtmp_url_auto"))
				ods::plugin::maybe_autofill_rtmp_url(settings, true);
			sync_rtsp_url_from_rtmp_if_needed(settings);
			sync_rtsp_url_enabled(props, settings);
			return false;
		}

		bool cb_rtmp_url_changed(void *, obs_properties_t *, obs_property_t *, obs_data_t *settings) {
			sync_rtsp_url_from_rtmp_if_needed(settings);
			return false;
		}

		bool cb_ffmpeg_download(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;
			bool expected = false;
			if (!d->manual_ffmpeg_download_running.compare_exchange_strong(
					expected,
					true,
					std::memory_order_acq_rel)) {
				return false;
			}
			d->request_props_refresh("ffmpeg_download_begin");

			auto life = std::weak_ptr<std::atomic<bool>>(d->life_token);
			try {
				std::thread([d, life]() {
					std::string out_path;
					std::string err;
					const bool  ok = ods::network::RtspE2eProber::ensure_auto_ffmpeg_path(out_path, err);
					if (!ok) {
						blog(LOG_WARNING, "[aunsync] ffmpeg download failed: %s", err.c_str());
					}

					struct UiCtx {
						std::weak_ptr<std::atomic<bool>> life_token;
						DelayStreamData                 *data;
					};
					auto ui_ctx = std::make_unique<UiCtx>(UiCtx{life, d});
					obs_queue_task(OBS_TASK_UI, [](void *param) {
						auto ctx = std::unique_ptr<UiCtx>(static_cast<UiCtx *>(param));
						auto token = ctx->life_token.lock();
						if (!token || !token->load(std::memory_order_acquire)) return;
						ctx->data->manual_ffmpeg_download_running.store(false, std::memory_order_release);
						ctx->data->request_props_refresh("ffmpeg_download_done"); }, ui_ctx.release(), false);
				}).detach();
			} catch (...) {
				d->manual_ffmpeg_download_running.store(false, std::memory_order_release);
				d->request_props_refresh("ffmpeg_download_spawn_error");
			}
			return false;
		}

		bool cb_flow_start_rtsp_e2e(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;
			if (d->rtsp_e2e_measure.is_measuring()) return false;

			obs_data_t *settings = obs_source_get_settings(d->context);
			if (!settings) return false;
			if (obs_data_get_bool(settings, "rtmp_url_auto"))
				ods::plugin::maybe_autofill_rtmp_url(settings, true);

			std::string rtsp_url;
			if (obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey)) {
				const char *raw_rtmp_url = obs_data_get_string(settings, "rtmp_url");
				rtsp_url                 = ods::plugin::to_rtsp_url_from_rtmp(raw_rtmp_url ? raw_rtmp_url : "");
				if (!rtsp_url.empty())
					obs_data_set_string(settings, ods::plugin::kRtspUrlKey, rtsp_url.c_str());
			} else {
				const char *raw_rtsp_url = obs_data_get_string(settings, ods::plugin::kRtspUrlKey);
				rtsp_url                 = raw_rtsp_url ? raw_rtsp_url : "";
			}
			const std::string ffmpeg_path = build_exe_path_hint(settings);
			const int         probe_mode  = static_cast<int>(
				obs_data_get_int(settings, ods::plugin::kRtspE2eProbeModeKey));
			obs_data_release(settings);

			std::string error_msg;
			if (rtsp_url.empty()) {
				error_msg = T_("RtspUrlMissing");
			} else if (!ods::plugin::is_obs_streaming_active()) {
				error_msg = T_("ObsStreamingInactive");
			}
			if (!error_msg.empty()) {
				d->rtsp_e2e_measure.set_last_error(error_msg);
				d->request_props_refresh("rtsp_e2e_start_error");
				return false;
			}

			d->probe_silent_active.store(probe_mode == 1, std::memory_order_release);
			d->rtsp_e2e_measure.set_last_error("");
			d->rtsp_e2e_measure.set_progress(0, RTSP_E2E_MEASURE_SETS_DEFAULT);
			d->inject_impulse.store(false, std::memory_order_release);
			d->rtsp_e2e_measure.set_cached_url(rtsp_url);

			auto &prober    = d->rtsp_e2e_measure.prober;
			prober.on_ready = [d]() {
				d->inject_impulse.store(true, std::memory_order_release);
				d->rtsp_e2e_measure.prober.notify_impulse_sent(std::chrono::steady_clock::now());
			};
			prober.on_progress = [d](int completed, int total) {
				d->rtsp_e2e_measure.set_progress(completed, total);
				d->request_props_refresh("rtsp_e2e_progress");
			};
			prober.on_result = [d](ods::network::RtspE2eResult r) {
				d->probe_silent_active.store(false, std::memory_order_release);
				d->rtsp_e2e_measure.apply_result(r);
				if (r.valid) {
					d->delay.measured_rtsp_e2e_ms = static_cast<int>(std::lround(r.latency_ms));
					d->delay.rtsp_e2e_measured    = true;
					d->rtsp_e2e_measure.set_progress(
						d->rtsp_e2e_measure.total_sets(),
						d->rtsp_e2e_measure.total_sets());

					struct Ctx {
						std::weak_ptr<std::atomic<bool>> life;
						DelayStreamData                 *d;
					};
					auto c = std::make_unique<Ctx>(Ctx{d->life_token, d});
					obs_queue_task(OBS_TASK_UI, [](void *p) {
						auto ctx = std::unique_ptr<Ctx>(static_cast<Ctx *>(p));
						auto life = ctx->life.lock();
						if (!life || !life->load(std::memory_order_acquire)) return;
						auto *dd = ctx->d;
						obs_data_t *s = obs_source_get_settings(dd->context);
						if (s) {
							ods::model::SettingsRepo repo(s);
							repo.set_measured_rtsp_e2e_ms(dd->delay.measured_rtsp_e2e_ms);
							repo.set_rtsp_e2e_measured(dd->delay.rtsp_e2e_measured);
							obs_source_update(dd->context, s);
							obs_data_release(s);
						}
						ods::plugin::recalc_all_delays(dd);
						dd->request_props_refresh("rtsp_e2e_result"); }, c.release(), false);
				} else {
					d->rtsp_e2e_measure.set_last_error(r.error_msg);
					d->request_props_refresh("rtsp_e2e_result_error");
				}
			};

			if (!prober.start(rtsp_url, ffmpeg_path)) {
				d->probe_silent_active.store(false, std::memory_order_release);
				d->rtsp_e2e_measure.set_last_error(T_("RtspMeasureStartFailed"));
				d->request_props_refresh("rtsp_e2e_start_failed");
				return false;
			}

			d->request_props_refresh("rtsp_e2e_started");
			return false;
		}

	} // namespace

	void add_plugin_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();

		obs_property_t *about_p = obs_properties_add_text(
			grp,
			"about_info",
			"",
			OBS_TEXT_INFO);
		obs_property_set_long_description(
			about_p,
			"v" PLUGIN_VERSION " | AunSync | (C) 2026 Chigiri Tsutsumi | GPL 2.0+ | "
			"<a href=\"https://github.com/pasocommate/AunSync\">GitHub</a>");
		obs_property_text_set_info_word_wrap(about_p, false);

		obs_property_t *schema_p = obs_properties_add_int(
			grp,
			ods::plugin::kSettingsSchemaVersionKey,
			"",
			0,
			999,
			1);
		obs_property_set_visible(schema_p, false);
		obs_property_set_enabled(schema_p, false);
		obs_property_t *saved_ver_p = obs_properties_add_text(
			grp,
			ods::plugin::kSettingsSavedVersionKey,
			"",
			OBS_TEXT_DEFAULT);
		obs_property_set_visible(saved_ver_p, false);
		obs_property_set_enabled(saved_ver_p, false);

		if (d->update_check.status.load(std::memory_order_acquire) ==
			ods::plugin::UpdateCheckStatus::UpdateAvailable) {
			const std::string latest_version = d->update_check.latest_version();
			if (!latest_version.empty()) {
				const std::string update_notice =
					ods::core::string_printf(T_("UpdateAvailableNoticeFmt"), latest_version.c_str());
				obs_property_t *update_notice_p = obs_properties_add_text(
					grp,
					"update_available_notice_top",
					update_notice.c_str(),
					OBS_TEXT_INFO);
				obs_property_text_set_info_word_wrap(update_notice_p, true);
			}
		}

		if (d->is_duplicate_instance) {
			obs_properties_add_text(
				grp,
				"duplicate_instance_warning",
				T_("DuplicateInstanceWarning"),
				OBS_TEXT_INFO);
		} else if (d->has_settings_mismatch) {
			obs_property_t *warn_p = obs_properties_add_text(
				grp,
				"settings_mismatch_warning",
				T_("SettingsMismatchWarning"),
				OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(warn_p, true);
		} else {
			int64_t sync_offset_ns = 0;
			if (ods::plugin::try_get_parent_audio_sync_offset_ns(d->context, sync_offset_ns) &&
				sync_offset_ns != REQUIRED_AUDIO_SYNC_OFFSET_NS) {
				obs_property_t *warn_p = obs_properties_add_text(
					grp,
					"audio_sync_offset_warning_top",
					T_("AudioSyncOffsetWarning"),
					OBS_TEXT_INFO);
				obs_property_text_set_info_word_wrap(warn_p, true);
			}

			if (d->rtsp_e2e_measure.is_measuring()) {
				const bool      probe_silent = d->probe_silent_active.load(std::memory_order_acquire);
				obs_property_t *warn_p       = obs_properties_add_text(
					grp,
					"rtsp_measure_warning",
					probe_silent ? T_("RtspMeasureWarnMute") : T_("RtspMeasureWarnMix"),
					OBS_TEXT_INFO);
				obs_property_text_set_info_word_wrap(warn_p, true);
			}
		}

		obs_properties_add_group(props, "grp_plugin", T_("Plugin"), OBS_GROUP_NORMAL, grp);
	}

	void add_master_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();

		obs_property_t *rtmp_auto =
			obs_properties_add_bool(grp, "rtmp_url_auto", T_("RtmpUrlAuto"));
		obs_property_set_modified_callback2(rtmp_auto, cb_rtmp_url_auto_changed, d);
		obs_property_t *rtmp_url =
			obs_properties_add_text(grp, "rtmp_url", T_("RtmpUrl"), OBS_TEXT_DEFAULT);
		obs_property_set_modified_callback2(rtmp_url, cb_rtmp_url_changed, d);

		obs_property_t *use_rtmp = obs_properties_add_bool(
			grp,
			ods::plugin::kRtspUseRtmpUrlKey,
			T_("RtspUseRtmpUrl"));
		obs_property_set_modified_callback2(use_rtmp, cb_rtsp_use_rtmp_changed, d);
		obs_property_t *rtsp_url = obs_properties_add_text(
			grp,
			ods::plugin::kRtspUrlKey,
			T_("RtspUrl"),
			OBS_TEXT_DEFAULT);

		obs_property_t *mode = obs_properties_add_list(
			grp,
			ods::plugin::kRtspE2eProbeModeKey,
			T_("RtspProbeMode"),
			OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(mode, T_("RtspProbeModeMix"), 0);
		obs_property_list_add_int(mode, T_("RtspProbeModeSilent"), 1);

		obs_property_t *ffmpeg_mode = obs_properties_add_list(
			grp,
			ods::plugin::kFfmpegExePathModeKey,
			T_("FfmpegExePathMode"),
			OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(ffmpeg_mode, T_("ExePathModeAuto"), static_cast<int>(ods::plugin::ExePathMode::Auto));
		obs_property_list_add_int(ffmpeg_mode, T_("ExePathModePath"), static_cast<int>(ods::plugin::ExePathMode::FromPath));
		obs_property_list_add_int(ffmpeg_mode, T_("ExePathModeAbsolute"), static_cast<int>(ods::plugin::ExePathMode::Absolute));
		obs_properties_add_text(grp, ods::plugin::kFfmpegExePathKey, T_("FfmpegExePath"), OBS_TEXT_DEFAULT);

		std::string ffmpeg_auto_path;
		const bool  ffmpeg_auto_exists =
			ods::network::RtspE2eProber::get_auto_ffmpeg_path_if_exists(ffmpeg_auto_path);
		obs_property_t *ffmpeg_download =
			obs_properties_add_button2(grp, "ffmpeg_download", T_("PathModeDownload"), cb_ffmpeg_download, d);
		obs_property_set_enabled(
			ffmpeg_download,
			!ffmpeg_auto_exists &&
				!d->manual_ffmpeg_download_running.load(std::memory_order_acquire));

		const bool      measuring = d->rtsp_e2e_measure.is_measuring();
		obs_property_t *start =
			obs_properties_add_button2(grp, "rtsp_e2e_start", T_("RtspE2eMeasure"), cb_flow_start_rtsp_e2e, d);
		obs_property_set_enabled(start, !measuring);

		if (measuring) {
			const int         completed = d->rtsp_e2e_measure.completed_sets();
			const int         total     = std::max(1, d->rtsp_e2e_measure.total_sets());
			const std::string progress  = ods::core::string_printf(
				T_("RtspE2eProgressFmt"),
				completed,
				total);
			obs_properties_add_text(grp, "rtsp_e2e_progress", progress.c_str(), OBS_TEXT_INFO);
		} else {
			const auto result = d->rtsp_e2e_measure.result();
			if (d->delay.rtsp_e2e_measured) {
				const std::string result_text = ods::core::string_printf(
					T_("RtspE2eResultFmt"),
					static_cast<double>(d->delay.measured_rtsp_e2e_ms),
					d->delay.calc_all_delays().master_delay_ms);
				obs_properties_add_text(grp, "rtsp_e2e_result", result_text.c_str(), OBS_TEXT_INFO);
			} else if (result.valid) {
				const std::string result_text = ods::core::string_printf(
					T_("RtspE2eResultFmt"),
					result.latency_ms,
					d->delay.calc_all_delays().master_delay_ms);
				obs_properties_add_text(grp, "rtsp_e2e_result", result_text.c_str(), OBS_TEXT_INFO);
			}
			const std::string err = d->rtsp_e2e_measure.last_error();
			if (!err.empty()) {
				const std::string err_text = ods::core::string_printf(T_("RtspE2eFailedFmt"), err.c_str());
				obs_properties_add_text(grp, "rtsp_e2e_error", err_text.c_str(), OBS_TEXT_INFO);
			}
		}

		obs_data_t *settings = obs_source_get_settings(d->context);
		if (settings) {
			obs_property_set_enabled(rtmp_url, !obs_data_get_bool(settings, "rtmp_url_auto"));
			obs_property_set_enabled(rtsp_url, !obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey));
			obs_data_release(settings);
		}

		obs_properties_add_group(props, "grp_master", T_("GroupMasterRtmp"), OBS_GROUP_NORMAL, grp);
	}

} // namespace ods::ui
