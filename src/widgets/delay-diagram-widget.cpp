/*
 * delay-diagram-widget.cpp
 *
 * QPainter カスタムウィジェットによるタイミング図。
 *
 * レーン構成（AunSync ソロモデル）:
 *   アバター : [アバター遅延 A]
 *   配信     : [ディレイ D] [OBS配信遅延 R]
 *
 * 成立時（D + R = A）は両レーンの右端が揃う。
 * 不成立時（R > A）はずれを可視化し、R 未計測時は "計測データなし" を表示する。
 */

#include "widgets/delay-diagram-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kDiagramMagic[]        = "DDIAGRAM|";
		constexpr int  kDiagramInjectRetryMax = 40;
		constexpr int  kDiagramInjectRetryMs  = 5;

		// セグメント配色
		inline QColor colorDelay() {
			return QColor(239, 68, 68);
		} // #ef4444
		inline QColor colorAvatar() {
			return QColor(245, 158, 11);
		} // #f59e0b
		inline QColor colorBroadcast() {
			return QColor(20, 184, 166);
		} // #14b8a6

		// 描画に必要なパース済みデータ。
		struct DiagramData {
			int  R            = 0;
			int  A            = 0;
			int  master_delay = 0;
			bool live_ok      = false;

			// 凡例ラベル
			QString lbl_delay;
			QString lbl_delay_desc;
			QString lbl_avatar;
			QString lbl_broadcast;
			QString lbl_lane_broadcast;
			QString lbl_lane_local;
			QString lbl_no_data;
			QString lbl_no_data_rtsp;
			QString lbl_listen_timing;
			QString help_text;
		};

		// レーン内のセグメント 1 つ。
		struct Segment {
			int    ms;
			QColor color;
		};

		// レーン 1 本。
		struct Lane {
			QString              label;
			std::vector<Segment> segments;

			int total_ms() const {
				int t = 0;
				for (const auto &s : segments)
					t += std::max(0, s.ms);
				return t;
			}
		};

		// ============================================================
		// DelayDiagramWidget
		// ============================================================

		constexpr int kLaneH         = 18;
		constexpr int kLaneGap       = 6;
		constexpr int kRulerH        = 20;
		constexpr int kRulerMarginB  = 8;
		constexpr int kLegendH       = 22;
		constexpr int kLegendMarginT = 10;
		constexpr int kMarginTop     = 4;
		constexpr int kMarginBottom  = 4;
		constexpr int kMarginL       = 48;
		constexpr int kMarginR       = 12;
		constexpr int kMinSegW       = 3;

		class DelayDiagramWidget : public QWidget {
		public:

			explicit DelayDiagramWidget(const DiagramData &data, QWidget *parent = nullptr)
				: QWidget(parent), data_(data) {
				setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
				setFixedHeight(calcHeight());
			}

		protected:

			void paintEvent(QPaintEvent * /*event*/) override {
				QPainter p(this);
				p.setRenderHint(QPainter::Antialiasing, false);
				p.setRenderHint(QPainter::TextAntialiasing, true);

				if (!hasData()) {
					drawNoData(p);
					return;
				}

				// アバターレーン [A]、配信レーン [D][R]
				std::vector<Lane> lanes;
				{
					Lane local;
					local.label    = data_.lbl_lane_local;
					local.segments = {{data_.A, colorAvatar()}};
					lanes.push_back(std::move(local));
				}
				{
					Lane broadcast;
					broadcast.label    = data_.lbl_lane_broadcast;
					broadcast.segments = {
						{data_.master_delay, colorDelay()},
						{data_.R, colorBroadcast()},
					};
					lanes.push_back(std::move(broadcast));
				}

				const int usableW = width() - kMarginL - kMarginR;
				int       maxMs   = 0;
				for (const auto &lane : lanes)
					maxMs = std::max(maxMs, lane.total_ms());
				if (maxMs <= 0) {
					drawNoData(p);
					return;
				}
				const double scale = static_cast<double>(usableW) / maxMs;

				const int rulerY = kMarginTop;
				drawRuler(p, rulerY, maxMs, scale);

				const int lanesTop     = rulerY + kRulerH + kRulerMarginB;
				const int broadcastIdx = static_cast<int>(lanes.size()) - 1;
				for (int li = 0; li < static_cast<int>(lanes.size()); ++li) {
					const int y = lanesTop + li * (kLaneH + kLaneGap);
					drawLane(p, lanes[li], y, scale,
					         /*mark_listen_timing=*/li != broadcastIdx);
				}

				const int legendY = lanesTop + static_cast<int>(lanes.size()) * (kLaneH + kLaneGap) + kLegendMarginT;
				drawLegend(p, legendY);
			}

		private:

			bool hasData() const { return data_.R > 0; }

			int countVisibleLanes() const { return 2; } // avatar + broadcast

			int calcHeight() const {
				if (!hasData()) return 20 + 20; // 1 行のノーデータメッセージ
				// 凡例2行: 1行目=レイテンシ系、2行目=自動調整ディレイ説明
				return kMarginTop + kRulerH + kRulerMarginB +
				       countVisibleLanes() * (kLaneH + kLaneGap) +
				       kLegendMarginT + kLegendH + kLegendH + kMarginBottom;
			}

			void drawNoData(QPainter &p) const {
				QStringList lines;
				if (data_.R <= 0 && !data_.lbl_no_data_rtsp.isEmpty())
					lines << data_.lbl_no_data_rtsp;
				if (lines.isEmpty())
					lines << data_.lbl_no_data;

				p.setPen(warningTextColor(palette()));
				QFont f = font();
				f.setPointSize(9);
				p.setFont(f);

				const QFontMetrics fm(f);
				const int          lineH  = fm.height() + 4;
				const int          totalH = lines.size() * lineH;
				int                y      = (height() - totalH) / 2;
				for (const auto &line : lines) {
					p.drawText(QRect(0, y, width(), lineH), int(Qt::AlignCenter), line);
					y += lineH;
				}
			}

			void drawRuler(QPainter &p, int top, int maxMs, double scale) const {
				const int     baseY = top + kRulerH;
				constexpr int tickH = 5;

				const QColor lineColor = palette().color(QPalette::Mid);
				const QColor textColor = palette().color(QPalette::Disabled, QPalette::Text);

				p.setPen(QPen(lineColor, 1));
				p.drawLine(kMarginL, baseY, kMarginL + static_cast<int>(maxMs * scale), baseY);

				const double rawStep = maxMs / 8.0;
				const double mag     = std::pow(10.0, std::floor(std::log10(rawStep)));
				double       nice    = mag;
				for (double n : {1.0, 2.0, 5.0, 10.0}) {
					if (n * mag >= rawStep) {
						nice = n * mag;
						break;
					}
				}
				const int step = std::max(1, static_cast<int>(nice));

				QFont f = font();
				f.setPixelSize(9);
				p.setFont(f);

				for (int ms = 0; ms <= maxMs; ms += step) {
					const int x = kMarginL + static_cast<int>(ms * scale);
					p.setPen(QPen(lineColor, 1));
					p.drawLine(x, baseY - tickH, x, baseY);

					p.setPen(textColor);
					const QString label = (ms == 0)
					                          ? QStringLiteral("0 ms")
					                          : QString::number(ms);
					p.drawText(QRect(x - 30, top, 60, kRulerH - tickH - 1),
					           int(Qt::AlignHCenter | Qt::AlignBottom),
					           label);
				}
			}

			// レーン 1 本の描画。mark_listen_timing=true ならアバター区間の左端に ▼ を描く。
			void drawLane(
				QPainter   &p,
				const Lane &lane,
				int         y,
				double      scale,
				bool        mark_listen_timing = false) const {
				{
					const QColor textColor = palette().color(QPalette::Text);
					p.setPen(textColor);
					QFont f = font();
					f.setPixelSize(11);
					f.setBold(true);
					p.setFont(f);
					p.drawText(QRect(0, y, kMarginL - 6, kLaneH),
					           int(Qt::AlignRight | Qt::AlignVCenter),
					           lane.label);
				}

				int   cumMs   = 0;
				int   x       = kMarginL;
				int   hearX   = -1;
				QFont segFont = font();
				segFont.setPixelSize(10);

				for (const auto &seg : lane.segments) {
					if (seg.ms <= 0) continue;
					cumMs += seg.ms;
					const int nextX = kMarginL + static_cast<int>(cumMs * scale);
					const int w     = std::max(kMinSegW, nextX - x);

					p.setPen(Qt::NoPen);
					p.setBrush(seg.color);
					p.drawRect(x, y, w, kLaneH);

					p.setPen(QPen(QColor(255, 255, 255, 20), 0.5));
					p.setBrush(Qt::NoBrush);
					p.drawRect(x, y, w, kLaneH);

					if (w > 22) {
						p.setPen(QColor(255, 255, 255));
						p.setFont(segFont);
						p.drawText(QRect(x, y, w, kLaneH),
						           int(Qt::AlignCenter),
						           QString::number(seg.ms));
					}

					if (seg.color.rgb() == colorAvatar().rgb())
						hearX = x;

					x += w;
				}

				if (mark_listen_timing && hearX >= 0) {
					QFont markerFont = font();
					markerFont.setPixelSize(10);
					p.setFont(markerFont);
					p.setPen(QColor(239, 68, 68));
					p.drawText(QRect(hearX - 6, y - 6, 12, 12),
					           int(Qt::AlignCenter),
					           QStringLiteral("▼"));
				}
			}

			void drawLegend(QPainter &p, int y) const {
				struct LegendItem {
					QColor  color;
					QString label;
				};
				// 1行目: レイテンシ系
				std::vector<LegendItem> row1Items = {
					{colorAvatar(), data_.lbl_avatar},
					{colorBroadcast(), data_.lbl_broadcast},
				};
				row1Items.erase(
					std::remove_if(
						row1Items.begin(),
						row1Items.end(),
						[](const LegendItem &item) { return item.label.isEmpty(); }),
					row1Items.end());

				const QColor lineColor = palette().color(QPalette::Mid);
				p.setPen(QPen(lineColor, 1));
				p.drawLine(kMarginL, y - kLegendMarginT / 2, width() - kMarginR, y - kLegendMarginT / 2);

				QFont f = font();
				f.setPixelSize(10);
				p.setFont(f);
				const QFontMetrics fm(f);

				constexpr int swatchW = 12;
				constexpr int swatchH = 12;
				constexpr int gap     = 10;
				constexpr int textGap = 4;

				const QColor textColor = palette().color(QPalette::Disabled, QPalette::Text);

				// --- 1行目 ---
				int x = kMarginL;
				for (const auto &item : row1Items) {
					const int sy = y + (kLegendH - swatchH) / 2;
					p.setPen(Qt::NoPen);
					p.setBrush(item.color);
					p.drawRect(x, sy, swatchW, swatchH);

					p.setPen(textColor);
					const int textX = x + swatchW + textGap;
					const int textW = fm.horizontalAdvance(item.label);
					p.drawText(QRect(textX, y, textW + 2, kLegendH),
					           int(Qt::AlignLeft | Qt::AlignVCenter),
					           item.label);

					x = textX + textW + gap;
				}

				// --- 2行目: 自動調整ディレイ説明 ---
				const int y2 = y + kLegendH;
				x            = kMarginL;

				const int sy2 = y2 + (kLegendH - swatchH) / 2;
				p.setPen(Qt::NoPen);
				p.setBrush(colorDelay());
				p.drawRect(x, sy2, swatchW, swatchH);

				QFont boldFont = f;
				boldFont.setBold(true);
				p.setFont(boldFont);
				const QFontMetrics fmBold(boldFont);

				p.setPen(textColor);
				const int labelX = x + swatchW + textGap;
				const int labelW = fmBold.horizontalAdvance(data_.lbl_delay);
				p.drawText(QRect(labelX, y2, labelW + 2, kLegendH),
				           int(Qt::AlignLeft | Qt::AlignVCenter),
				           data_.lbl_delay);

				p.setFont(f);
				const int descX = labelX + labelW + 4;
				const int descW = fm.horizontalAdvance(data_.lbl_delay_desc);
				p.drawText(QRect(descX, y2, descW + 2, kLegendH),
				           int(Qt::AlignLeft | Qt::AlignVCenter),
				           data_.lbl_delay_desc);

				// ▼ マーカー凡例
				if (!data_.lbl_listen_timing.isEmpty()) {
					const int markerX = descX + descW + gap;
					p.setPen(QColor(239, 68, 68));
					const QString marker  = QStringLiteral("▼");
					const int     markerW = fm.horizontalAdvance(marker);
					p.drawText(QRect(markerX, y2, markerW + 2, kLegendH),
					           int(Qt::AlignLeft | Qt::AlignVCenter),
					           marker);

					p.setPen(textColor);
					const int ltX = markerX + markerW + textGap;
					const int ltW = fm.horizontalAdvance(data_.lbl_listen_timing);
					p.drawText(QRect(ltX, y2, ltW + 2, kLegendH),
					           int(Qt::AlignLeft | Qt::AlignVCenter),
					           data_.lbl_listen_timing);
				}
			}

			DiagramData data_;
		};

		// ============================================================
		// inject インフラ
		// ============================================================

		struct DiagramInjectCtx {
			explicit DiagramInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {}
			~DiagramInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kDiagramInjectRetryMax;
		};

		// ペイロード文字列をパースする。
		// 書式: DDIAGRAM|R|A|master_delay|live_ok
		//       |lbl_delay|lbl_delay_desc|lbl_avatar|lbl_broadcast
		//       |lbl_lane_broadcast|lbl_lane_local|lbl_no_data|lbl_no_data_rtsp
		//       |lbl_listen_timing|help_text
		bool parse_diagram_payload(const QString &text, DiagramData &out) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.empty() || fields[0] != QLatin1String("DDIAGRAM"))
				return false;

			constexpr int kFixedFields = 5; // header + R, A, master_delay, live_ok
			constexpr int kLabelFields = 10;
			if (fields.size() < kFixedFields + kLabelFields)
				return false;

			bool ok = false;
			out.R   = fields[1].toInt(&ok);
			if (!ok) return false;
			out.A = fields[2].toInt(&ok);
			if (!ok) return false;
			out.master_delay = fields[3].toInt(&ok);
			if (!ok) return false;
			out.live_ok = fields[4] == QLatin1String("1");

			out.lbl_delay          = fields[5];
			out.lbl_delay_desc     = fields[6];
			out.lbl_avatar         = fields[7];
			out.lbl_broadcast      = fields[8];
			out.lbl_lane_broadcast = fields[9];
			out.lbl_lane_local     = fields[10];
			out.lbl_no_data        = fields[11];
			out.lbl_no_data_rtsp   = fields[12];
			out.lbl_listen_timing  = fields[13];
			out.help_text          = fields[14];

			return true;
		}

		// プレースホルダーを DelayDiagramWidget へ差し替える。
		void do_diagram_inject(void *param) {
			auto ctx = std::unique_ptr<DiagramInjectCtx>(
				static_cast<DiagramInjectCtx *>(param));
			if (!ctx) return;

			struct Placeholder {
				QLabel *label = nullptr;
				QString text;
			};
			std::vector<Placeholder>    found;
			std::vector<ScrollSnapshot> scroll_snapshots;

			const auto all_widgets = QApplication::allWidgets();
			for (QWidget *w : all_widgets) {
				auto *lbl = qobject_cast<QLabel *>(w);
				if (!lbl) continue;
				const QString text = lbl->text();
				if (text.startsWith(QLatin1String(kDiagramMagic)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				DiagramData data;
				if (!parse_diagram_payload(ph.text, data))
					continue;

				QWidget *parent = ph.label->parentWidget();
				if (!parent) continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form) continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(ph.label, &row, &role);
				if (row < 0) continue;

				auto *container = new QWidget(parent);
				auto *vlay      = new QVBoxLayout(container);
				vlay->setContentsMargins(0, 0, 0, 0);
				vlay->setSpacing(0);
				vlay->addWidget(new DelayDiagramWidget(data, container));

				if (!data.help_text.isEmpty())
					vlay->addWidget(create_help_callout(data.help_text, container));

				form->removeRow(row);
				form->insertRow(row, container);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kDiagramInjectRetryMs,
				                   [next]() { do_diagram_inject(next); });
				return;
			}
		}

	} // namespace

	// ============================================================
	// 公開 API
	// ============================================================

	obs_property_t *obs_properties_add_delay_diagram(
		obs_properties_t         *props,
		const char               *prop_name,
		const DelayDiagramInfo   &info,
		const DelayDiagramLabels &labels) {
		if (!props || !prop_name || !*prop_name)
			return nullptr;

		// 書式: DDIAGRAM|R|A|master_delay|live_ok
		//       |lbl_delay|lbl_delay_desc|lbl_avatar|lbl_broadcast
		//       |lbl_lane_broadcast|lbl_lane_local|lbl_no_data|lbl_no_data_rtsp
		//       |lbl_listen_timing|help_text
		std::string payload = "DDIAGRAM";
		{
			char buf[48];
			std::snprintf(
				buf,
				sizeof(buf),
				"|%d|%d|%d|%d",
				info.R,
				info.A,
				info.master_delay,
				info.live_ok ? 1 : 0);
			payload += buf;
		}
		for (const char *s : {
				 labels.legend_delay,
				 labels.legend_delay_desc,
				 labels.legend_avatar,
				 labels.legend_broadcast,
				 labels.lane_broadcast,
				 labels.lane_local,
				 labels.no_data,
				 labels.no_data_rtsp,
				 labels.legend_listen_timing,
				 labels.help_text}) {
			payload += '|';
			payload += escape_field(s ? s : "");
		}

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_delay_diagram_inject(obs_source_t *source) {
		if (!source) return;
		auto ctx = std::make_unique<DiagramInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_diagram_inject, ctx.release(), false);
	}

} // namespace ods::widgets
