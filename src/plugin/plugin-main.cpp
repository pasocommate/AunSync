#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "audio/audio-processor.hpp"
#include "core/constants.hpp"
#include "plugin/plugin-config.hpp"
#include "plugin/plugin-helpers.hpp"
#include "plugin/plugin-services.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/release-check.hpp"
#include "ui/properties-builder.hpp"
#include "ui/properties-delay.hpp"
#include "viewmodel/delay-viewmodel.hpp"
#include "widgets/delay-diagram-widget.hpp"
#include "widgets/help-callout-widget.hpp"

#include <QApplication>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <obs-module.h>
#include <string>
#include <thread>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("aunsync", "ja-JP")

static std::mutex    g_singleton_mtx;
static obs_source_t *g_singleton_owner      = nullptr;
static uint64_t      g_singleton_generation = 0;

using ods::plugin::DelayStreamData;
using ods::plugin::UpdateCheckStatus;

class DelayStreamFilter {
public:

	static const char *get_name(void *);
	static void *create(obs_data_t *, obs_source_t *);
	static void destroy(void *);
	static void update(void *, obs_data_t *);
	static obs_audio_data *filter_audio(void *, obs_audio_data *);
	static obs_properties_t *get_properties(void *);
	static void get_defaults(obs_data_t *);

private:

	static void queue_ui_safe(DelayStreamData *, std::function<void(DelayStreamData *)>);
	static void schedule_widget_injects(DelayStreamData *);
	static void schedule_audio_sync_check(DelayStreamData *, std::weak_ptr<std::atomic<bool>>);
	static void start_update_check_async(DelayStreamData *, std::weak_ptr<std::atomic<bool>>);
	static void schedule_update_check(DelayStreamData *, std::weak_ptr<std::atomic<bool>>, bool);
};

void DelayStreamFilter::queue_ui_safe(DelayStreamData *d, std::function<void(DelayStreamData *)> fn) {
	struct Ctx {
		std::weak_ptr<std::atomic<bool>>       life_token;
		DelayStreamData                       *d;
		std::function<void(DelayStreamData *)> fn;
	};
	auto c = std::make_unique<Ctx>(Ctx{d->life_token, d, std::move(fn)});
	obs_queue_task(OBS_TASK_UI, [](void *p) {
		auto c = std::unique_ptr<Ctx>(static_cast<Ctx *>(p));
		auto life = c->life_token.lock();
		if (life && life->load(std::memory_order_acquire))
			c->fn(c->d); }, c.release(), false);
}

void DelayStreamFilter::schedule_widget_injects(DelayStreamData *d) {
	if (!d || !d->context) return;
	ods::widgets::schedule_help_callout_inject(d->context);
	ods::widgets::schedule_delay_diagram_inject(d->context);
}

void DelayStreamFilter::schedule_audio_sync_check(
	DelayStreamData                 *d,
	std::weak_ptr<std::atomic<bool>> life_token_weak) {
	constexpr int kIntervalMs = 3000;
	QTimer::singleShot(kIntervalMs, qApp, [d, life_token_weak]() {
		auto token = life_token_weak.lock();
		if (!token || !token->load(std::memory_order_acquire)) return;

		int64_t current = INT64_MIN;
		ods::plugin::try_get_parent_audio_sync_offset_ns(d->context, current);
		const int64_t last =
			d->last_rendered_audio_sync_offset_ns.load(std::memory_order_relaxed);
		if (current != last)
			d->request_props_refresh("audio_sync_offset_poll");

		const bool streaming = ods::plugin::is_obs_streaming_active();
		bool       on_track  = false;
		if (ods::plugin::try_get_parent_on_streaming_track(d->context, on_track)) {
			if (streaming && !on_track)
				d->rtsp_seen_offtrack.store(true, std::memory_order_relaxed);
			const auto note = ods::plugin::classify_rtsp_track_note(
				on_track,
				d->rtsp_seen_offtrack.load(std::memory_order_relaxed),
				streaming);
			const int note_i = static_cast<int>(note);
			if (note_i != d->rtsp_track_note_state.exchange(note_i, std::memory_order_relaxed))
				d->request_props_refresh("rtsp_track_note_poll");
		}

		schedule_audio_sync_check(d, life_token_weak);
	});
}

void DelayStreamFilter::start_update_check_async(
	DelayStreamData                 *d,
	std::weak_ptr<std::atomic<bool>> life_token_weak) {
	if (!d) return;
	bool expected = false;
	if (!d->update_check.inflight.compare_exchange_strong(
			expected,
			true,
			std::memory_order_acq_rel)) {
		return;
	}
	d->update_check.status.store(UpdateCheckStatus::Checking, std::memory_order_release);

	try {
		std::thread([d, life_token_weak]() {
			ods::plugin::LatestReleaseInfo info;
			const bool                     ok = ods::plugin::fetch_latest_release_info(info);
			const bool                     has_update =
				ok && ods::plugin::is_newer_version(info.latest_version, PLUGIN_VERSION);

			struct Ctx {
				std::weak_ptr<std::atomic<bool>> life_token;
				DelayStreamData                 *d;
				bool                             ok         = false;
				bool                             has_update = false;
				ods::plugin::LatestReleaseInfo   info;
			};
			auto ctx = std::make_unique<Ctx>(Ctx{life_token_weak, d, ok, has_update, std::move(info)});
			obs_queue_task(OBS_TASK_UI, [](void *p) {
				auto ctx = std::unique_ptr<Ctx>(static_cast<Ctx *>(p));
				auto life = ctx->life_token.lock();
				if (!life || !life->load(std::memory_order_acquire)) return;
				DelayStreamData *d = ctx->d;
				d->update_check.set_strings(
					ctx->info.latest_version,
					ctx->info.release_url,
					ctx->info.error);
				UpdateCheckStatus new_status = UpdateCheckStatus::Error;
				if (ctx->ok)
					new_status = ctx->has_update ? UpdateCheckStatus::UpdateAvailable : UpdateCheckStatus::UpToDate;
				const UpdateCheckStatus prev =
					d->update_check.status.exchange(new_status, std::memory_order_acq_rel);
				d->update_check.inflight.store(false, std::memory_order_release);
				if (new_status == UpdateCheckStatus::UpdateAvailable || prev != new_status)
					d->request_props_refresh("release_update_check"); }, ctx.release(), false);
		}).detach();
	} catch (...) {
		d->update_check.inflight.store(false, std::memory_order_release);
		d->update_check.status.store(UpdateCheckStatus::Error, std::memory_order_release);
		d->request_props_refresh("release_update_check_spawn_error");
	}
}

void DelayStreamFilter::schedule_update_check(
	DelayStreamData                 *d,
	std::weak_ptr<std::atomic<bool>> life_token_weak,
	bool                             immediate) {
	constexpr int kInitialDelayMs = 1200;
	constexpr int kIntervalMs     = 6 * 60 * 60 * 1000;
	const int     delay           = immediate ? kInitialDelayMs : kIntervalMs;
	QTimer::singleShot(delay, qApp, [d, life_token_weak]() {
		auto token = life_token_weak.lock();
		if (!token || !token->load(std::memory_order_acquire)) return;
		start_update_check_async(d, life_token_weak);
		schedule_update_check(d, life_token_weak, false);
	});
}

const char *DelayStreamFilter::get_name(void *) {
	return "AunSync";
}

constexpr int kFrontendStreamingStarted = 1;
constexpr int kFrontendStreamingStopped = 3;

void on_frontend_event(int event, void *private_data) {
	if (event != kFrontendStreamingStarted && event != kFrontendStreamingStopped)
		return;
	auto *d = static_cast<DelayStreamData *>(private_data);
	if (!d || d->destroying.load(std::memory_order_acquire)) return;
	d->request_props_refresh("frontend_streaming_state");
}

void *DelayStreamFilter::create(obs_data_t *settings, obs_source_t *source) {
	blog(LOG_INFO, "[aunsync] create START source=%s", obs_source_get_name(source));
	auto *d    = new DelayStreamData();
	d->context = source;

	{
		std::lock_guard<std::mutex> lk(g_singleton_mtx);
		if (g_singleton_owner == nullptr || g_singleton_owner == source) {
			g_singleton_owner       = source;
			d->singleton_generation = ++g_singleton_generation;
			d->owns_singleton_slot  = true;
		} else {
			d->is_duplicate_instance = true;
			d->owns_singleton_slot   = false;
		}
	}
	ods::ui::props_refresh_unblock_source(source);
	if (d->is_duplicate_instance) {
		d->create_done.store(true);
		return d;
	}

	std::string settings_reason;
	if (!ods::plugin::validate_settings_compatibility(settings, settings_reason)) {
		d->has_settings_mismatch = true;
		d->enabled.store(false, std::memory_order_relaxed);
		blog(LOG_WARNING, "[aunsync] settings compatibility check failed: %s", settings_reason.c_str());
		d->create_done.store(true);
		return d;
	}

	ods::plugin::maybe_autofill_rtmp_url(settings, false);
	d->rtmp_url_auto.store(obs_data_get_bool(settings, "rtmp_url_auto"), std::memory_order_relaxed);
	ods::plugin::add_obs_frontend_event_callback(on_frontend_event, d);

	update(d, settings);
	d->create_done.store(true);
	queue_ui_safe(d, [](DelayStreamData *dp) {
		auto life = std::weak_ptr<std::atomic<bool>>(dp->life_token);
		schedule_audio_sync_check(dp, life);
		schedule_update_check(dp, life, true);
	});
	blog(LOG_INFO, "[aunsync] create complete");
	return d;
}

void DelayStreamFilter::destroy(void *data) {
	auto d = std::unique_ptr<DelayStreamData>(static_cast<DelayStreamData *>(data));
	if (!d) return;
	d->destroying.store(true, std::memory_order_release);
	if (d->life_token) {
		d->life_token->store(false, std::memory_order_release);
		d->life_token.reset();
	}
	if (d->context)
		ods::ui::props_refresh_block_source(d->context);

	const bool     release_singleton_slot = d->owns_singleton_slot;
	obs_source_t  *my_source              = d->context;
	const uint64_t my_gen                 = d->singleton_generation;
	if (!d->is_warning_only_instance()) {
		d->rtsp_e2e_measure.cancel();
		ods::plugin::remove_obs_frontend_event_callback(on_frontend_event, d.get());
	}
	d.reset();
	if (release_singleton_slot) {
		std::lock_guard<std::mutex> lk(g_singleton_mtx);
		if (g_singleton_owner == my_source && g_singleton_generation == my_gen)
			g_singleton_owner = nullptr;
	}
}

void DelayStreamFilter::update(void *data, obs_data_t *settings) {
	ods::plugin::apply_settings(static_cast<DelayStreamData *>(data), settings);
}

obs_audio_data *DelayStreamFilter::filter_audio(void *data, obs_audio_data *audio) {
	return ods::audio::filter_audio_delay_stream(static_cast<DelayStreamData *>(data), audio);
}

obs_properties_t *DelayStreamFilter::get_properties(void *data) {
	obs_properties_t *props = obs_properties_create();
	if (!data) return props;
	auto *d = static_cast<DelayStreamData *>(data);

	struct GetPropsDepthGuard {
		DelayStreamData *d;
		~GetPropsDepthGuard() {
			if (d) d->get_props_depth.fetch_sub(1, std::memory_order_acq_rel);
		}
	};
	d->get_props_depth.fetch_add(1, std::memory_order_acq_rel);
	GetPropsDepthGuard depth_guard{d};

	{
		int64_t sync_offset_ns = INT64_MIN;
		ods::plugin::try_get_parent_audio_sync_offset_ns(d->context, sync_offset_ns);
		d->last_rendered_audio_sync_offset_ns.store(sync_offset_ns, std::memory_order_relaxed);
	}

	ods::ui::add_plugin_group(props, d);
	if (!d->is_warning_only_instance()) {
		ods::ui::add_master_group(props, d);
		auto vm = ods::viewmodel::DelayViewModel::build(d->delay);
		ods::ui::delay::add_fine_tune_group(props, d, vm);
		ods::ui::delay::add_delay_diagram_group(props, d, vm);
		schedule_widget_injects(d);
	}

	return props;
}

void DelayStreamFilter::get_defaults(obs_data_t *settings) {
	ods::plugin::set_delay_stream_defaults(settings);
}

static struct obs_source_info delay_stream_filter;

static void register_source_info() {
	memset(&delay_stream_filter, 0, sizeof(delay_stream_filter));
	delay_stream_filter.id             = "aunsync_filter";
	delay_stream_filter.type           = OBS_SOURCE_TYPE_FILTER;
	delay_stream_filter.output_flags   = OBS_SOURCE_AUDIO;
	delay_stream_filter.get_name       = DelayStreamFilter::get_name;
	delay_stream_filter.create         = DelayStreamFilter::create;
	delay_stream_filter.destroy        = DelayStreamFilter::destroy;
	delay_stream_filter.update         = DelayStreamFilter::update;
	delay_stream_filter.filter_audio   = DelayStreamFilter::filter_audio;
	delay_stream_filter.get_properties = DelayStreamFilter::get_properties;
	delay_stream_filter.get_defaults   = DelayStreamFilter::get_defaults;
}

bool obs_module_load(void) {
	register_source_info();
	obs_register_source(&delay_stream_filter);
	blog(LOG_INFO, "[aunsync] v" PLUGIN_VERSION " loaded");
	return true;
}

void obs_module_unload(void) {}
