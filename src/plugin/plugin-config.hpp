#pragma once

#include <obs-module.h>

namespace ods::plugin {

	/// フィルタ設定のデフォルト値を書き込む。
	void set_delay_stream_defaults(obs_data_t *settings);

} // namespace ods::plugin
