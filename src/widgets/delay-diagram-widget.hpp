#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * ディレイタイミング図ウィジェットに渡す描画データ。
	 *
	 * AunSync のソロモデル: アバターレーン [A] と配信レーン [D][R] を描画し、
	 * 成立時（live_ok）は両レーンの右端が一致する。
	 */
	struct DelayDiagramInfo {
		int  R;            ///< OBS 配信レイテンシ (ms)
		int  A;            ///< アバターレイテンシ (ms)
		int  master_delay; ///< 自動調整ディレイ D (ms)
		bool live_ok;      ///< D + R = A が成立しているか（R ≤ A）
	};

	/**
	 * タイミング図の凡例ラベル文字列。
	 *
	 * plugin-main.cpp 側で obs_module_text() を使って渡す。
	 */
	struct DelayDiagramLabels {
		const char *legend_delay         = nullptr; ///< "自動調整ディレイ"
		const char *legend_delay_desc    = nullptr; ///< "は上記の値に基づいて…"
		const char *legend_avatar        = nullptr; ///< "アバターレイテンシ"
		const char *legend_broadcast     = nullptr; ///< "OBS配信レイテンシ"
		const char *lane_broadcast       = nullptr; ///< "配信" (レーンラベル)
		const char *lane_local           = nullptr; ///< "アバター" (レーンラベル)
		const char *no_data              = nullptr; ///< "計測データなし"
		const char *no_data_rtsp         = nullptr; ///< "OBS配信遅延が未計測です（…）"
		const char *legend_listen_timing = nullptr; ///< "観客が知覚するタイミング"
		const char *help_text            = nullptr; ///< グループ末尾のヘルプテキスト
	};

	/// QFormLayout 用プレースホルダーとして OBS_TEXT_INFO プロパティを追加する
	obs_property_t *obs_properties_add_delay_diagram(
		obs_properties_t         *props,
		const char               *prop_name,
		const DelayDiagramInfo   &info,
		const DelayDiagramLabels &labels);

	/// プレースホルダーを実ウィジェットへ差し替える inject を予約する
	void schedule_delay_diagram_inject(obs_source_t *source);

} // namespace ods::widgets
