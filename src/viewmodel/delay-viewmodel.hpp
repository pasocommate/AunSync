#pragma once

#include "model/delay-state.hpp"

namespace ods::viewmodel {

	using ods::model::DelaySnapshot;
	using ods::model::DelayState;

	/**
	 * AunSync の微調整 UI に渡す読み取り専用スナップショット。
	 */
	struct DelayViewModel {
		DelaySnapshot snapshot;                  ///< ディレイ計算結果
		int           rtsp_e2e_ms       = 0;     ///< R: OBS 配信遅延 (ms)
		bool          rtsp_e2e_measured = false; ///< R が計測済みか
		int           avatar_latency_ms = 0;     ///< A: 想定アバター遅延 (ms)
		int           master_delay_ms   = 0;     ///< D: 出口で付加するディレイ (ms)
		bool          service_too_slow  = false; ///< R > A でディレイ調整不能か

		static DelayViewModel build(const DelayState &delay) {
			DelayViewModel vm;
			vm.snapshot          = delay.calc_all_delays();
			vm.rtsp_e2e_ms       = delay.measured_rtsp_e2e_ms;
			vm.rtsp_e2e_measured = delay.rtsp_e2e_measured;
			vm.avatar_latency_ms = delay.avatar_latency_ms;
			vm.master_delay_ms   = vm.snapshot.master_delay_ms;
			vm.service_too_slow  = vm.snapshot.service_too_slow;
			return vm;
		}
	};

} // namespace ods::viewmodel
