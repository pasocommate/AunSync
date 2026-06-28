#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}
namespace ods::viewmodel {
	struct DelayViewModel;
}

namespace ods::ui::delay {

	/// 想定アバター遅延と算出ディレイの UI グループを追加する。
	void add_fine_tune_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, const ods::viewmodel::DelayViewModel &vm);

	/// AunSync のタイミング図 UI グループを追加する。
	void add_delay_diagram_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, const ods::viewmodel::DelayViewModel &vm);

} // namespace ods::ui::delay
