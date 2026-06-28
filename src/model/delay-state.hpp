#pragma once

#include "core/constants.hpp"

namespace ods::model {

	/**
	 * AunSync のディレイ計算結果スナップショット。
	 *
	 * ソロ配信者が配信チャンネルに直接フィルタを挿す前提で、
	 * 観客が聴く配信音 (D + R) とアバター動作 (A) を揃える。
	 */
	struct DelaySnapshot {
		int  master_delay_ms      = 0;                                   ///< 出口で付加するディレイ D (ms)
		bool service_too_slow     = false;                               ///< OBS 配信遅延 R が想定アバター遅延 A を超えているか
		bool rtsp_e2e_measured    = false;                               ///< R が計測済みか
		int  audio_sync_offset_ms = core::REQUIRED_AUDIO_SYNC_OFFSET_MS; ///< 親ソースに求める同期オフセット
	};

	/**
	 * AunSync のディレイ計算入力。
	 */
	struct DelayState {
		int  measured_rtsp_e2e_ms = 0;     ///< R: OBS 配信遅延 (ms)
		bool rtsp_e2e_measured    = false; ///< RTSP E2E 計測済みフラグ
		int  avatar_latency_ms    = 200;   ///< A: 想定アバター遅延 (ms)

		/// D = max(0, A - R) を計算する。
		DelaySnapshot calc_all_delays() const {
			DelaySnapshot snap;
			snap.rtsp_e2e_measured = rtsp_e2e_measured;
			const int R            = measured_rtsp_e2e_ms;
			const int A            = avatar_latency_ms;
			snap.service_too_slow  = rtsp_e2e_measured && R > A;
			snap.master_delay_ms   = snap.service_too_slow ? 0 : ((A > R) ? (A - R) : 0);
			return snap;
		}
	};

} // namespace ods::model
