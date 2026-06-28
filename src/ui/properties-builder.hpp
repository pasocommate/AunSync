#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::ui {

	/// プラグイン情報 UI グループを追加する。
	void add_plugin_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// RTSP E2E 計測 UI グループを追加する。
	void add_master_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

} // namespace ods::ui
