#include "ui/properties-delay.hpp"

#include "core/string-format.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "ui/props-refresh.hpp"
#include "viewmodel/delay-viewmodel.hpp"
#include "widgets/delay-diagram-widget.hpp"
#include "widgets/help-callout-widget.hpp"

#include <string>

#define T_(s) obs_module_text(s)

namespace ods::ui::delay {

	using ods::plugin::DelayStreamData;
	using ods::viewmodel::DelayViewModel;
	using namespace ods::core;
	using namespace ods::widgets;

	namespace {

		bool cb_avatar_latency_changed(
			void *priv,
			obs_properties_t *,
			obs_property_t *,
			obs_data_t *settings) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			if (settings)
				ods::plugin::apply_settings(d, settings);
			d->request_props_refresh("avatar_latency_changed");
			return false;
		}

	} // namespace

	void add_fine_tune_group(obs_properties_t *props, DelayStreamData *d, const DelayViewModel &vm) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();

		obs_property_t *avatar = obs_properties_add_int(
			grp,
			ods::plugin::kAvatarLatencyKey,
			T_("AvatarLatencyLabel"),
			0,
			5000,
			1);
		obs_property_set_long_description(avatar, T_("AvatarLatencyHelpText"));
		obs_property_set_modified_callback2(avatar, cb_avatar_latency_changed, d);

		const std::string delay_text = ods::core::string_printf(
			T_("MasterDelayResultFmt"),
			vm.master_delay_ms);
		obs_properties_add_text(grp, "master_delay_result", delay_text.c_str(), OBS_TEXT_INFO);

		if (vm.service_too_slow) {
			obs_properties_add_help_callout(
				grp,
				"service_too_slow",
				T_("ServiceTooSlowError"),
				CalloutVariant::Error);
		}

		obs_properties_add_group(
			props,
			"grp_fine_tune",
			T_("GroupFineTune"),
			OBS_GROUP_NORMAL,
			grp);
	}

	void add_delay_diagram_group(obs_properties_t *props, DelayStreamData *d, const DelayViewModel &vm) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();

		DelayDiagramInfo info{};
		info.R            = vm.rtsp_e2e_ms;
		info.A            = vm.avatar_latency_ms;
		info.master_delay = vm.master_delay_ms;
		info.live_ok      = !vm.service_too_slow;

		DelayDiagramLabels labels;
		labels.legend_delay         = T_("DiagramDelay");
		labels.legend_delay_desc    = T_("DiagramDelayAunSyncDesc");
		labels.legend_avatar        = T_("DiagramAvatarLatency");
		labels.legend_broadcast     = T_("DiagramBroadcastLatency");
		labels.lane_broadcast       = T_("DiagramLaneBroadcast");
		labels.lane_local           = T_("DiagramLaneAvatar");
		labels.no_data              = T_("DiagramNoData");
		labels.no_data_rtsp         = T_("DiagramNoDataRtsp");
		labels.legend_listen_timing = T_("DiagramAudienceTiming");
		labels.help_text            = T_("DiagramHelpTextAunSync");
		obs_properties_add_delay_diagram(grp, "delay_diagram", info, labels);

		obs_properties_add_group(
			props,
			"grp_delay_diagram",
			T_("GroupDelayDiagram"),
			OBS_GROUP_NORMAL,
			grp);
	}

} // namespace ods::ui::delay
