---
name: mvvm-architecture
description: Enforce this project's MVVM architecture rules when the task involves adding or changing settings parameters, delay calculations (DelayState, calc_all_delays), SettingsRepo accessors, DelayViewModel fields, UI properties groups (properties-builder.cpp, properties-delay.cpp, get_properties), or modified callbacks (obs_property_set_modified_callback).
allowed-tools: Read, Edit, Write, Grep, Glob, Bash
---

# MVVM アーキテクチャ規約の適用

パラメータ追加・ディレイ計算変更・UI 改修を行う際に、プロジェクト固有の MVVM アーキテクチャ規約を適用する。

背景情報（全体構造、データフロー、スレッドモデル）は [`docs/architecture-mvvm.md`](docs/architecture-mvvm.md) を参照すること。このスキルはその中の手続き的ルールを強制する。

## 層の責務

| 層 | ディレクトリ | やること | やらないこと |
|---|---|---|---|
| View | `src/ui/` | ViewModel の const 参照を読んで UI を構築。ボタン CB は Model/SettingsRepo を更新 | `DelayStreamData` のフィールドを直接変更しない |
| ViewModel | `src/viewmodel/` | `build()` で Model から毎回構築。スタックローカルで消費 | メンバ変数やグローバルに保持しない |
| Model | `src/model/` | `DelayState` でディレイ計算、`SettingsRepo` で obs_data アクセス | UI 層を直接知らない |
| Infra | `src/plugin/` | `DelayStreamData`, `SettingsApplier`, イベント CB | — |

## 禁止事項

以下に該当するコードを書こうとした場合、必ず停止して正しいパターンに修正する。

1. **`properties-*.cpp` から `obs_data_get_*/set_*` を直接呼ぶ** → `SettingsRepo` 経由にする
2. **`calc_all_delays()` 以外の場所でディレイを計算する** → `DelayState::calc_all_delays()` に集約する
3. **`get_properties()` 内で `DelayStreamData` のフィールドを変更する** → UI 構築は読み取り専用
4. **ViewModel をメンバ変数やグローバルに保持する** → スタックローカルで毎回構築する
5. **`obs_property_t*` をコールバック間でキャッシュする** → OBS は毎回再構築する設計
6. **計算済み表示データを `obs_data_t` に永続保存する** → 入力値のみ保存し都度計算する
7. **カスタムウィジェットがあるページで `return true` 後に inject を再スケジュールしない** → §CB ルール参照
8. **カスタムウィジェットのデストラクタで binding_id マップエントリを削除する** → `RefreshProperties` がウィジェットを再構築しても `obs_properties_t` は生存しているため、再 inject で同じ binding_id が必要になる。古いエントリは登録関数（`obs_properties_add_*_row`）で新しい binding_id を作る際にプレフィックス一致で掃除する

## modified callback の `return true` ルール {#cb-rule}

### OBS の RefreshProperties メカニズム

OBS の modified callback が `true` を返すと `RefreshProperties()` が走る。重要なのは **`RefreshProperties()` は既存の `obs_properties_t` を再トラバースするだけで `get_properties()` を再呼出ししない** こと。つまり `schedule_widget_injects()` は呼ばれず、`OBS_TEXT_INFO` プレースホルダー経由で注入されたカスタムウィジェット（HelpCallout, DelayDiagram 等）は破壊されたまま復元されない。

> **参照**: `third_party/obs-studio/shared/properties-view/properties-view.cpp` の `RefreshProperties()` — `obs_properties_first(properties.get())` で既存オブジェクトを走査。

### inject の実行タイミング

このプロジェクトの OBS の `ui_task_handler`（`OBSApp.cpp`）は `Qt::AutoConnection` を使用するため、UI スレッドから呼ばれた `obs_queue_task(OBS_TASK_UI, ..., false)` は **DirectConnection（同期実行）** になる。

コールバック本体で `schedule_*_inject` を呼ぶと:

1. inject 関数が **同期的に即座に** 実行される
2. 旧プレースホルダーは既に前回の inject で置換済み → ラベルが見つからない → **リトライ開始**（`QTimer::singleShot`）
3. コールバックが `true` を返す → OBS が `RefreshProperties()` をキューイング
4. `RefreshProperties()` がウィジェットを再構築 → 新しいプレースホルダーが生まれる
5. リトライが新しいプレースホルダーを検出して置換 → **復元完了**

**ルール**: `return true` する modified callback は、そのグループに必要な **すべての** inject を `props_ui_with_preserved_scroll` 内でスケジュールすること。1 種類でも漏れると、その種別だけ未置換のプレースホルダーがユーザーに見える。

```cpp
// OK: return true + 当該グループの全 inject を再スケジュール
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

// NG: inject 漏れのある return true
bool cb_xxx_changed(...) {
    props_ui_with_preserved_scroll([d]() {
        schedule_help_callout_inject(d->context);
        // schedule_delay_diagram_inject が漏れている → タイミング図が消える
    });
    return true;
}

// NG: inject なしの return true
bool cb_xxx_changed(...) {
    obs_property_set_visible(p, show);
    return true;  // 全カスタムウィジェットが消える
}
```

### ボタンコールバックとの違い

ColorButtonRow / TextButton 等のカスタムウィジェット経由のボタンクリックは、コールバックが `true` を返すと `obs_source_update_properties()` を呼ぶ。これは **`get_properties()` を再呼出し** するフルリロードのため、`schedule_widget_injects()` が自動的に走る。ボタンコールバック本体で inject を手動スケジュールする必要はない。

### なぜ `return false` + `request_props_refresh` ではダメか

OBS はダイアログ構築時にも modified callback を呼ぶ。その時点で `get_props_depth` は 0 のため `request_props_refresh` がガードを通過し、`get_properties` → 構築 → callback → refresh → … の無限ループになる。

### グループ別の必要 inject 一覧

`schedule_widget_injects()` (plugin-main.cpp) を正とする。新しいカスタムウィジェットを追加した場合、**この関数**と**以下の表**と**同グループの全 modified callback 本体**の 3 箇所を更新する。

| グループ | inject |
|---|---|
| fine_tune_group | help_callout, delay_diagram |

## パラメータ追加チェックリスト

新しい設定パラメータを追加したら以下を順に確認する。各ステップを完了してから次に進む。

### ディレイ計算に影響するパラメータ

- [ ] `DelayState` にフィールド追加 (`src/model/delay-state.hpp`)、`calc_all_delays()` を更新
- [ ] `SettingsRepo` にアクセサ追加 (`src/model/settings-repo.hpp`)。キーが新規なら `plugin-settings.hpp` にも追加
- [ ] `SettingsApplier::apply_delay_settings()` で obs_data → `d->delay.xxx` への転写を追加
- [ ] 必要なら `DelayViewModel` に表示フィールドを追加し `build()` で値を設定
- [ ] UI 構築関数を更新（ViewModel から読み取って表示）
- [ ] ロケールキー追加 (`data/locale/`)

### ディレイ計算に影響しないパラメータ

- [ ] `SettingsRepo` にアクセサ追加（obs_data 経由の場合）
- [ ] `SettingsApplier` で転写
- [ ] UI 構築関数を更新

## UI 改修チェックリスト

### 微調整グループ (fine_tune_group / delay_diagram_group)

- [ ] 表示データは `DelayViewModel` 経由で取得。`DelayStreamData` を直接参照しない
- [ ] 新しい表示項目は `DelayViewModel` にフィールド追加 → `build()` で計算

### その他グループ (plugin_group / master_group)

- [ ] ViewModel 未導入グループは `DelayStreamData` を直接参照して良い

### 新しいカスタムウィジェット種別を追加する場合

- [ ] binding_id は `prop_name#seq` 形式で一意に生成する（既存の `make_*_binding_id` 関数を参照）
- [ ] デストラクタで binding_id マップエントリを **削除しない**（禁止事項 8 参照）
- [ ] 登録関数（`obs_properties_add_*_row`）で新しい binding_id 登録時に、同じ prop_name プレフィックスの旧エントリを掃除する
- [ ] `schedule_widget_injects()` にグループ別の inject 呼び出しを追加する
- [ ] **同じグループにある `return true` する全 modified callback** の `props_ui_with_preserved_scroll` 本体にも inject 呼び出しを追加する
- [ ] 上記のグループ別 inject 一覧表を更新する

## レビュー手順

タスク完了前に以下を確認する。

1. 追加・変更したコードが上記の禁止事項に該当しないこと
2. パラメータ追加の場合、チェックリストの全項目が完了していること
3. modified callback を追加・変更した場合、`return true` ルールに従っていること
4. **inject クロスチェック**: カスタムウィジェットの追加・変更を行った場合、以下の 3 箇所が同期していること
   - `schedule_widget_injects()` (plugin-main.cpp)
   - 同グループの全 modified callback の `props_ui_with_preserved_scroll` 本体
   - §CB ルールのグループ別 inject 一覧表（このスキル内）
