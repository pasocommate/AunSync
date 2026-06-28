# MVVM アーキテクチャガイド

本プロジェクトでは、OBS プラグイン API の制約下で MVVM 風の 3 層アーキテクチャを採用している。
パラメータ追加・UI 改修を行う際は、このドキュメントに従うこと。

> **Claude Code 利用時**: §4〜§6 のチェックリスト・禁止事項は `.claude/skills/mvvm-architecture/SKILL.md` としてスキル化されており、関連タスクで自動適用される。

---

## 1. 全体構造

```
┌──────────────────────────────────────────────┐
│  View (OBS properties + Qt widgets)           │
│  get_properties() で ViewModel を読むだけ      │
│  ボタン CB は Model / SettingsRepo を更新       │
└───────────────┬──────────────────────────────┘
                │ reads (const 参照)
┌───────────────┴──────────────────────────────┐
│  ViewModel (読み取り専用スナップショット)        │
│  DelayViewModel                              │
│  build() で Model + obs_data から毎回構築      │
│  計算済み表示データを保持                       │
└───────────────┬──────────────────────────────┘
                │ reads
┌───────────────┴──────────────────────────────┐
│  Model (計算ロジック + 永続化ファサード)        │
│  DelayState — ディレイ計算の入力値と計算関数    │
│  SettingsRepo — obs_data_t の型安全ラッパー    │
└──────────────────────────────────────────────┘
```

### ディレクトリ構成

| ディレクトリ | 層 | 内容 |
|---|---|---|
| `src/model/` | Model | `DelayState`, `SettingsRepo` |
| `src/viewmodel/` | ViewModel | `DelayViewModel` |
| `src/ui/` | View | `properties-builder.cpp`, `properties-delay.cpp` |
| `src/plugin/` | Infrastructure | `DelayStreamData`, `SettingsApplier`, イベント CB |

---

## 2. 主要コンポーネント

### 2.1 DelayState / DelaySnapshot (`src/model/delay-state.hpp`)

**ディレイ計算の唯一の真実 (single source of truth)**。

- `DelayState` — 計算に必要な入力値（計測結果、アバター遅延）
- `DelaySnapshot` — `calc_all_delays()` が返す不変の計算結果

```cpp
// 入力値
struct DelayState {
    int  measured_rtsp_e2e_ms = 0;     // R: OBS 配信遅延 (ms)
    bool rtsp_e2e_measured    = false; // RTSP E2E 計測済みフラグ
    int  avatar_latency_ms    = 200;   // A: 想定アバター遅延 (ms)

    DelaySnapshot calc_all_delays() const;
};

// 計算結果（不変）: D = max(0, A − R)
struct DelaySnapshot {
    int  master_delay_ms;       // 出口で付加するディレイ D (ms)
    bool service_too_slow;      // R > A でディレイ調整不能か
    bool rtsp_e2e_measured;     // R が計測済みか
    int  audio_sync_offset_ms;  // 親ソースに求める同期オフセット（固定 −950）
};
```

**ルール**:
- ディレイ値の計算は **必ず** `calc_all_delays()` を経由する。個別計算を散在させない。
- `DelayStreamData` は `DelayState delay;` メンバを持ち、ディレイ関連フィールドはすべてここに集約する。

### 2.2 SettingsRepo (`src/model/settings-repo.hpp`)

**`obs_data_t` への型安全アクセスファサード**。

```cpp
class SettingsRepo {
    obs_data_t *s_;
public:
    int  avatar_latency_ms() const;
    void set_avatar_latency_ms(int v);
    int  measured_rtsp_e2e_ms() const;
    void set_measured_rtsp_e2e_ms(int v);
    bool rtsp_e2e_measured() const;
    void set_rtsp_e2e_measured(bool v);
    bool rtsp_use_rtmp_url() const;
    void set_rtsp_use_rtmp_url(bool v);
    std::string rtsp_url() const;
    void set_rtsp_url(const std::string &v);
};
```

**ルール**:
- UI コールバック (`properties-*.cpp`) から `obs_data_get_*/set_*` を直接呼ばない。必ず `SettingsRepo` 経由。
- 新しい obs_data キーを追加する場合、SettingsRepo にアクセサを追加してから使う。
- キー名文字列は `plugin-settings.hpp` の定数として定義する。外部に散在させない。

### 2.3 DelayViewModel (`src/viewmodel/delay-viewmodel.hpp`)

**微調整 UI 用の読み取り専用スナップショット**。

```cpp
struct DelayViewModel {
    DelaySnapshot snapshot;
    int  rtsp_e2e_ms;
    bool rtsp_e2e_measured;
    int  avatar_latency_ms;
    int  master_delay_ms;
    bool service_too_slow;

    static DelayViewModel build(const DelayState &delay);
};
```

**ルール**:
- `get_properties()` の冒頭で `build()` し、UI 構築関数に `const` 参照で渡す。
- ViewModel は **スタックローカル** で構築・消費する。メンバ変数に保持しない。
- UI 構築関数は ViewModel のフィールドだけを読み、`DelayStreamData` を直接参照しない。

---

## 3. データフロー

### 3.1 設定変更 → ディレイ反映

```
obs_data_t 変更 (OBS update コールバック)
  → SettingsApplier::apply_delay_settings()
    → d->delay.xxx へ値を転写
    → recalc_all_delays(d)
      → d->delay.calc_all_delays()  → DelaySnapshot
      → master_buf へ master_delay_ms を適用
```

### 3.2 計測完了 → 設定書き戻し → ディレイ反映

```
計測コールバック (rtsp_e2e_measure.on_result)
  → queue_ui_safe() で UI スレッドへディスパッチ
    → SettingsRepo で obs_data に書き戻し
    → recalc_all_delays(d)
    → request_props_refresh()
```

### 3.3 UI 再描画

```
get_properties(d)
  → add_plugin_group(props, d)
  → add_master_group(props, d)        // RTSP E2E 計測グループ
  → DelayViewModel vm = DelayViewModel::build(d->delay)
  → delay::add_fine_tune_group(props, d, vm)
  → delay::add_delay_diagram_group(props, d, vm)
```

---

## 4. パラメータ追加チェックリスト

新しい設定パラメータを追加する場合、以下の手順に従う。

### 4.1 ディレイ計算に影響するパラメータの場合

1. **DelayState にフィールド追加** (`src/model/delay-state.hpp`)
   - `calc_all_delays()` の計算式を更新

2. **SettingsRepo にアクセサ追加** (`src/model/settings-repo.hpp`)
   - キーが新規なら `plugin-settings.hpp` にキー定数を追加

3. **SettingsApplier で転写** (`src/plugin/plugin-settings.cpp`)
   - `apply_delay_settings()` で obs_data → `d->delay.xxx` への転写を追加

4. **DelayViewModel に表示フィールド追加** (必要な場合のみ, `src/viewmodel/delay-viewmodel.hpp`)
   - ViewModel 直下にフィールドを追加
   - `build()` で値を設定

5. **UI 構築関数を更新** (`src/ui/properties-delay.cpp`)
   - ViewModel から読み取って表示

6. **ロケールキー追加** (`data/locale/`)

### 4.2 ディレイ計算に影響しないパラメータの場合

1. **SettingsRepo にアクセサ追加** (obs_data 経由のパラメータの場合)

2. **SettingsApplier で転写** (`src/plugin/plugin-settings.cpp`)

3. **UI 構築関数を更新** (`src/ui/properties-builder.cpp` または `properties-delay.cpp`)

---

## 5. UI 改修チェックリスト

### 5.1 微調整グループの改修

1. 表示データは **必ず `DelayViewModel` 経由**で取得する
2. `DelayStreamData` を直接参照しない
3. 新しい表示項目は ViewModel にフィールドを追加し、`build()` で計算する

### 5.2 その他グループの改修

- `add_plugin_group` / `add_master_group` は `DelayStreamData` を直接参照して良い
- 将来そのグループの複雑度が上がった場合、同様のパターンで ViewModel を導入する

---

## 6. やってはいけないこと

| 禁止事項 | 理由 |
|---|---|
| `properties-*.cpp` から `obs_data_get_*/set_*` を直接呼ぶ | SettingsRepo に集約済み。キー名の散在を防ぐ |
| ディレイ計算を `calc_all_delays()` 以外の場所で行う | 計算ロジックの重複を防ぐ |
| `get_properties()` 内で `DelayStreamData` のフィールドを直接変更する | UI 構築は読み取り専用であるべき |
| ViewModel をメンバ変数やグローバルに保持する | スタックローカルで毎回構築して鮮度を保証する |
| `obs_property_t*` をコールバック間でキャッシュする | OBS API はプロパティを毎回再構築する設計 |
| 計算済みの表示データを `obs_data_t` に永続保存する | 入力値のみ保存し、表示データは都度計算する |
| カスタムウィジェットがあるページで `return true` 後に inject を再スケジュールしない | `return true` は OBS の `RefreshProperties` を起動し、注入済みウィジェットを破壊する（§6.1 参照） |

### 6.1 modified callback と `return true` のルール

OBS の modified callback が `true` を返すと、OBS は `RefreshProperties()` を実行してダイアログ全体の Qt ウィジェットツリーを再構築する。この再構築は `OBS_TEXT_INFO` プレースホルダーを経由して注入されたカスタムウィジェット（HelpCallout, DelayDiagram 等）を破壊する。

**ルール**: カスタムウィジェットが存在するグループで `return true` する場合、直後にそのグループに必要な inject を再スケジュールすること。

```cpp
// ✓ 正しいパターン
bool cb_xxx_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
    auto *d = static_cast<DelayStreamData *>(priv);
    // ... visibility/enabled 変更 ...
    props_ui_with_preserved_scroll([d]() {
        if (!d || !d->context) return;
        schedule_help_callout_inject(d->context);
        schedule_delay_diagram_inject(d->context);
    });
    return true;
}

// ✗ 危険: inject なしの return true → カスタムウィジェットが消える
bool cb_xxx_changed(...) {
    obs_property_set_visible(p, show);
    return true;
}
```

**なぜ `return false` + `request_props_refresh_for_tabs` ではダメか**:

OBS はプロパティダイアログ構築時にも modified callback を呼ぶ。その時点で `get_props_depth` は 0（`get_properties` は既に返っている）のため、`request_props_refresh` はガードを通過して再構築を要求する。結果、`get_properties` → ダイアログ構築 → callback → refresh → `get_properties` → … の無限ループになる。

---

## 7. スレッド安全性

| データ | 書込みスレッド | 読取りスレッド | 保護機構 |
|---|---|---|---|
| `DelayBuffer::delay_samples_` | UI (`set_delay_ms`) | Audio (`process`) | atomic (既存) |
| `DelayState` フィールド | UI (`SettingsApplier` / CB) | UI (`ViewModel::build`) | 単一スレッド |
| ViewModel | UI で構築 | UI で消費 | スタックローカル |
| `RtspE2eMeasureState` 内部 | Worker | UI | per-instance mutex |

新しいフィールドを追加する際は、このスレッドモデルに合わせて適切な保護を選択する。

---

## 8. 将来の拡張方針

現在 ViewModel が導入されているのは微調整グループのみ。
他のグループ（RTSP 計測等）の複雑度が上がった場合は、同じパターンで ViewModel を導入する:

1. `src/viewmodel/` に `XxxViewModel` を作成
2. `build()` で必要なデータを事前計算
3. UI 構築関数の引数を `const XxxViewModel &` に変更
4. `get_properties()` 内でそのグループ構築時だけ構築
