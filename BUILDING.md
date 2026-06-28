# ビルド手順（Windows）

## 最短手順（推奨）

`build.bat` は以下を一括実行します。

- OBS Studio のクローン・ビルド（初回のみ自動）
- プラグインの CMake 構成/ビルド
- OBS へのインストール（ProgramData 既定）

```powershell
.\build.bat
```

既存の OBS ソースを使う場合:

```powershell
.\build.bat D:\dev\obs-studio
```

## build.bat の設定

`build.env`（`build.env.sample` 参照）に以下を指定できます。

```text
OBS_SOURCE_DIR=D:\dev\obs-studio
OBS_LEGACY_INSTALL=0
OBS_CI=1
```

- `OBS_SOURCE_DIR`: OBS Studio ソースのパス
- `OBS_LEGACY_INSTALL`: `1` で Program Files レイアウト、`0` で ProgramData レイアウト（既定）
- `OBS_CI`: `1` でインストール/`pause` をスキップ

既定インストール先:

- ProgramData（推奨）: `C:\ProgramData\obs-studio`
- legacy: `C:\Program Files\obs-studio`

## 必要環境

- OS: Windows 10 1909+ / Windows 11
- Visual Studio 2022（Desktop development with C++、MSVC v143、Windows SDK）
- CMake 3.16+
- Git for Windows

## 手動ビルド（必要時のみ）

### 1. OBS Studio をビルド

対象バージョンは `OBS_STUDIO_REF` ファイルで管理されています（現在: `32.0.4`）。

```powershell
git clone --depth 1 --branch 32.0.4 --recurse-submodules `
  https://github.com/obsproject/obs-studio.git third_party\obs-studio
cd third_party\obs-studio
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo --parallel
cd ..\..
```

### 2. プラグインを構成/ビルド

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR=third_party\obs-studio `
  -DOBS_PLUGIN_LEGACY_INSTALL=OFF
cmake --build build --config RelWithDebInfo --parallel
```

生成物:

- `build\RelWithDebInfo\aunsync.dll`

### 3. OBS へインストール

推奨（CMake install）:

```powershell
cmake --install build --config RelWithDebInfo --prefix "C:\ProgramData\obs-studio"
```

legacy 配置:

```powershell
cmake --install build --config RelWithDebInfo --prefix "C:\Program Files\obs-studio"
```

ProgramData 配置の例:

```text
build\RelWithDebInfo\aunsync.dll
  -> C:\ProgramData\obs-studio\plugins\aunsync\bin\64bit\
data\locale\*.ini
  -> C:\ProgramData\obs-studio\plugins\aunsync\data\locale\
```

### 4. 動作確認

1. OBS Studio を起動
2. 音声ソースを右クリック
3. フィルター -> `+` -> `AunSync`
4. GUI が開けば完了

## よくあるエラー

### `Could not find libobs`

- `OBS_SOURCE_DIR` が OBS ソースのルートを指しているか確認
- `OBS_SOURCE_DIR\build_x64\libobs\RelWithDebInfo\obs.lib` の存在を確認

## 現在の主要ファイル構成

```text
AunSync/
  .github/workflows/build.yml      リリースビルド（タグ push）
  data/locale/
    en-US.ini
    ja-JP.ini
  src/
    audio/
      audio-processor.cpp/hpp      音声フィルタ処理・プローブ注入
      probe-signal.cpp/hpp         RTSP E2E 計測用チャープ信号
    core/
      constants.hpp                共有定数
      delay-buffer.hpp             遅延バッファ
      string-format.hpp            文字列ユーティリティ
    model/
      delay-state.hpp              DelayState / DelaySnapshot
      settings-repo.hpp            SettingsRepo
    network/
      rtsp-e2e-prober.cpp/hpp      RTSP E2E 遅延計測
    plugin/
      plugin-main.cpp              プラグインエントリポイント
      plugin-config.cpp/hpp        設定スキーマ・デフォルト値
      plugin-helpers.cpp/hpp       共通ヘルパー
      plugin-services.cpp/hpp      バックグラウンドサービス
      plugin-settings.cpp/hpp      SettingsApplier・キー定数
      plugin-state.hpp             DelayStreamData
      plugin-utils.cpp/hpp         ユーティリティ
      release-check.cpp/hpp        更新確認
    ui/
      properties-builder.cpp/hpp   プラグイン情報・RTSP計測 UI
      properties-delay.cpp/hpp     微調整・タイミング図 UI
      props-refresh.cpp/hpp        プロパティ再描画制御
    viewmodel/
      delay-viewmodel.hpp          DelayViewModel
    widgets/
      delay-diagram-widget.cpp/hpp タイミング図ウィジェット
      help-callout-widget.cpp/hpp  ヘルプコールアウトウィジェット
      widget-inject-utils.hpp      ウィジェット注入ユーティリティ
      widget-payload-utils.cpp/hpp ウィジェットペイロード処理
  third_party/                     （ローカル取得。通常は Git 管理外）
    obs-studio/
  build.bat
  build.env.sample
  CMakeLists.txt
  BUILDING.md
```
