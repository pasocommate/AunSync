#pragma once

#include "audio/probe-signal.hpp"
#include "core/delay-buffer.hpp"
#include "model/delay-state.hpp"
#include "network/rtsp-e2e-prober.hpp"
#include "ui/props-refresh.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <obs-module.h>
#include <string>
#include <vector>

namespace ods::plugin {

	using ods::core::DelayBuffer;
	using ods::model::DelaySnapshot;
	using ods::model::DelayState;
	using ods::network::RtspE2eProber;
	using ods::network::RtspE2eResult;

	/**
	 * RtspE2eProber と計測結果をまとめた状態クラス。
	 */
	class RtspE2eMeasureState {
	public:

		RtspE2eProber prober; ///< on_ready/on_result は外部で設定

		bool is_measuring() const { return prober.is_running(); }

		void apply_result(const RtspE2eResult &r) {
			std::lock_guard<std::mutex> lk(mtx_);
			result_  = r;
			applied_ = false;
		}

		RtspE2eResult result() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return result_;
		}

		bool is_applied() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return applied_;
		}

		void set_applied(bool v) {
			std::lock_guard<std::mutex> lk(mtx_);
			applied_ = v;
		}

		std::string cached_url() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return cached_url_;
		}

		void set_cached_url(const std::string &url) {
			std::lock_guard<std::mutex> lk(mtx_);
			cached_url_ = url;
		}

		void set_progress(int completed, int total) {
			completed_sets_.store(completed, std::memory_order_relaxed);
			total_sets_.store(total, std::memory_order_relaxed);
		}

		int completed_sets() const { return completed_sets_.load(std::memory_order_relaxed); }
		int total_sets() const { return total_sets_.load(std::memory_order_relaxed); }

		std::string last_error() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return last_error_;
		}

		void set_last_error(const std::string &err) {
			std::lock_guard<std::mutex> lk(mtx_);
			last_error_ = err;
		}

		void cancel() {
			prober.cancel();
			completed_sets_.store(0, std::memory_order_relaxed);
			total_sets_.store(0, std::memory_order_relaxed);
		}

	private:

		mutable std::mutex mtx_;               ///< メンバアクセスを保護する mutex
		RtspE2eResult      result_;            ///< 直近の RTSP E2E 計測結果
		bool               applied_ = false;   ///< 結果適用済みフラグ
		std::string        cached_url_;        ///< 直近の計測対象 URL
		std::string        last_error_;        ///< 直近のエラー文字列
		std::atomic<int>   completed_sets_{0}; ///< 完了セット数
		std::atomic<int>   total_sets_{0};     ///< 総セット数
	};

	enum class UpdateCheckStatus {
		Unknown = 0,     ///< 未確認
		Checking,        ///< 確認中
		UpToDate,        ///< 最新
		UpdateAvailable, ///< 更新あり
		Error,           ///< 取得失敗
	};

	class UpdateCheckState {
	public:

		std::atomic<UpdateCheckStatus> status{UpdateCheckStatus::Unknown}; ///< 更新確認の現在状態
		std::atomic<bool>              inflight{false};                    ///< HTTP リクエスト処理中フラグ

		void set_strings(const std::string &version, const std::string &url, const std::string &error) {
			std::lock_guard<std::mutex> lk(mtx_);
			latest_version_ = version;
			latest_url_     = url;
			error_          = error;
		}

		std::string latest_version() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return latest_version_;
		}

		std::string latest_url() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return latest_url_;
		}

		std::string error() const {
			std::lock_guard<std::mutex> lk(mtx_);
			return error_;
		}

	private:

		mutable std::mutex mtx_;            ///< 文字列フィールドを保護する mutex
		std::string        latest_version_; ///< 取得した最新バージョン文字列
		std::string        latest_url_;     ///< 取得した最新版ダウンロード URL
		std::string        error_;          ///< 取得失敗時のエラー文字列
	};

	/**
	 * OBS フィルタ全体の実行状態。
	 */
	struct DelayStreamData {
		obs_source_t           *context               = nullptr; ///< OBS フィルタコンテキスト
		bool                    is_duplicate_instance = false;   ///< 二重起動されたインスタンスか
		bool                    has_settings_mismatch = false;   ///< 保存設定の整合性エラーで警告専用モードか
		bool                    owns_singleton_slot   = false;   ///< シングルトンスロットを確保しているか
		uint64_t                singleton_generation  = 0;       ///< シングルトン世代番号（重複判定用）
		std::atomic<bool>       destroying{false};               ///< デストラクタ実行中フラグ
		std::atomic<bool>       enabled{true};                   ///< フィルタ有効フラグ
		std::atomic<bool>       inject_impulse{false};           ///< RTSP E2E 計測用プローブ注入フラグ
		std::atomic<bool>       probe_silent_active{false};      ///< サイレントモード計測中フラグ
		std::atomic<bool>       rtsp_seen_offtrack{false};       ///< 計測文脈で配信トラック未割当を観測したか
		std::atomic<int>        rtsp_track_note_state{0};        ///< 計測タブのトラック注意表示状態
		ods::audio::ProbeSignal probe_signal;                    ///< RTSP E2E 計測用チャープ信号

		std::shared_ptr<std::atomic<bool>> life_token =
			std::make_shared<std::atomic<bool>>(true);

		DelayState           delay;                                         ///< ディレイ計算の入力値
		DelayBuffer          master_buf;                                    ///< 出口ディレイバッファ
		RtspE2eMeasureState  rtsp_e2e_measure;                              ///< RTSP E2E 計測状態
		std::atomic<bool>    manual_ffmpeg_download_running{false};         ///< ffmpeg 手動ダウンロード実行中フラグ
		uint32_t             sample_rate = 48000;                           ///< 音声サンプルレート (Hz)
		uint32_t             channels    = 2;                               ///< 音声チャンネル数
		bool                 initialized = false;                           ///< 音声処理の初期化完了フラグ
		std::atomic<bool>    create_done{false};                            ///< obs_source_create 完了フラグ
		std::atomic<int>     get_props_depth{0};                            ///< obs_get_properties の再入深度
		std::atomic<int64_t> last_rendered_audio_sync_offset_ns{INT64_MIN}; ///< 最後に描画した同期オフセット
		UpdateCheckState     update_check;                                  ///< 更新確認状態
		std::atomic<bool>    rtmp_url_auto{true};                           ///< RTMP URL 自動補完を有効にするフラグ
		std::vector<float>   work_buf;                                      ///< 音声処理用ワークバッファ

		bool is_warning_only_instance() const {
			return is_duplicate_instance || has_settings_mismatch;
		}

		void request_props_refresh(const char *reason = nullptr) const {
			ods::ui::props_refresh_request(
				context,
				create_done.load(std::memory_order_acquire),
				destroying.load(std::memory_order_acquire),
				get_props_depth.load(std::memory_order_acquire),
				reason);
		}
	};

} // namespace ods::plugin
