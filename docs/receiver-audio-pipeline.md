# 受信側 音声パイプライン（低レイテンシ・メインスレッド非依存）設計メモ

receiver（ブラウザ）で WS 配信の Opus/PCM 音声を再生する経路の設計と、低レイテンシ
での再生中の途切れ・再同期時の音飛びを解消するために導入した
**AudioWorklet + SharedArrayBuffer** 構成をまとめる。

> ステータス: **実装済み**（受信再生を AudioWorklet 化／SAB が使えない環境は従来経路へ自動フォールバック）。
> 関連: [`ci-obs-version-pinning.md`](ci-obs-version-pinning.md)（FFmpeg 依存排除＝別件）。
> 実装反映:
> `receiver/src/audio-ring.ts`, `receiver/src/playback-worklet.js`,
> `receiver/src/audio.ts`, `receiver/src/opus.ts`, `receiver/src/connection.ts`, `receiver/src/ui.ts`,
> `src/network/stream-router.cpp`（TCP_NODELAY / COOP・COEP ヘッダ）。

---

## 1. 事象

低レイテンシ（再生バッファ 100ms 程度）で、

- 再生中に時々音が途切れる（自動再同期のタイミングに関係なく発生）。
- 自動再同期（receiver 側機能）のタイミングで音飛びが目立つ。

再生バッファを 300ms 程度まで上げると安定するが、レイテンシが大きくなり用途に合わない。

---

## 2. 原因（実測で確定）

送信〜受信を各区間で計測し、以下を確認した。

| 区間 | 計測 | 結果 |
|---|---|---|
| OBS → エンコード供給 | filter_audio 呼び出し間隔 | 10.3ms で安定（詰まり無し） |
| Opus エンコード | 生成収支 / 1フレーム処理時間 | 実時間生成・0.2ms（問題無し） |
| io_service 送出 | send 実行間隔 / 送信キュー滞留量 | 20ms 間隔・滞留ほぼ 0（即時に捌けている） |
| ブラウザ受信 | onmessage 到着間隔 | 時々 140〜190ms のギャップ |

→ **送信側（プラグイン）は完全に正常**。バイトはブラウザへ即時届いている（送信キューが滞留しない＝TCP 逆圧無し）のに、JS の `onmessage` だけが遅れる。

結論: **受信ブラウザのメインスレッドが周期的に停止**し、WebSocket 受信と Web Audio の再生スケジューリングが遅れていた。停止の主因は **1パケットごとの `AudioBuffer` / `AudioBufferSourceNode` 生成（50回/秒）による GC 圧**。停止中に受信バッファが枯れ（アンダーラン）、その後バックログが一括到着してバッファが膨張、自動再同期がそれを切り詰めて音飛びになっていた。

PC・スマホ双方で再現したことから、特定端末の問題ではなく受信コード共通の構造的要因と判断した。

---

## 3. 対策：再生経路をメインスレッドから分離

音声の **再生** をメインスレッドの Web Audio スケジューリングから、オーディオスレッドで
動く **AudioWorklet** へ移し、デコード結果は **SharedArrayBuffer のリングバッファ**で渡す。

```
[main thread]  WS受信 → Opusデコード(WebCodecs/WASM) → リングへ書き込み
                                                          │ (SharedArrayBuffer)
[audio thread] AudioWorklet ←─────────────────────────────┘  リングから読み出して出力
```

要点:

- **per-packet の `AudioBuffer` / `createBufferSource` を撤廃**（最大の GC 源を除去）。デコード結果は
  リングへ float を書き込むだけ。
- 再生は AudioWorklet（オーディオスレッド）で行うため、**メインスレッドが GC/描画で止まっても
  途切れない**。
- バッファ制御は AudioWorklet 内で完結（[`playback-worklet.js`](../receiver/src/playback-worklet.js)）:
  - プリバッファ: 滞留が target に達するまで無音（起動・アンダーラン復帰）。
  - 上限超過 / 再同期要求: target まで古いデータを破棄（自動再同期相当・低レイテンシ維持）。
  - アンダーラン: 残りを出して無音で埋め、再プリバッファへ。
- メーターはオーディオグラフ後段の `AnalyserNode` を `requestAnimationFrame` で読むだけにし、
  パケット経路から分離。

> 注: WebSocket 受信と Opus デコードは引き続きメインスレッドで実行している。per-packet 確保の
> 撤廃で GC 由来の停止が大幅に減り、かつ再生がオーディオスレッドへ移ったことで、低レイテンシでも
> 実用水準まで安定した。残存ジッタをさらに詰める場合は **WS 受信＋デコードも Worker へ移す**のが
> 次の打ち手（§6）。

### 補助的な改善（送信側）
- **TCP_NODELAY**（Nagle 無効化）: 小さな Opus パケットの送出バッチ化を防ぐ。リアルタイム配信の定石。
- flush 時にエンコーダをリセットせず端数も破棄しない（連続性維持）。

---

## 4. SharedArrayBuffer の前提（重要・壊しやすい）

`SharedArrayBuffer` は **cross-origin isolation** が有効でないと使えない。受信ページを配信する
プラグインの HTTP 応答に次のヘッダを付与している（[`stream-router.cpp`](../src/network/stream-router.cpp) `on_http`）:

```
Cross-Origin-Opener-Policy:   same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: same-origin
```

- 受信ページの全アセットは同一オリジン配信なので COEP `require-corp` 下でも読み込める。
- AudioWorklet モジュールは Blob URL 経由で `addModule` する（[`audio.ts`](../receiver/src/audio.ts)）。
  Vite が `.ts` を誤った MIME の data URL 化して壊すため、worklet は素の JS
  （[`playback-worklet.js`](../receiver/src/playback-worklet.js)）にして `?raw` で取り込み、
  実行時に Blob 化している。
- **これらのヘッダや配信構成を崩すと `crossOriginIsolated` が false になり、新経路が無効化**される
  （その場合は自動的に従来 `playBuffer` 経路へフォールバックし、音は出るが本問題が再発しうる）。

リングのレイアウト定数（`CTL_*` / 容量）は [`audio-ring.ts`](../receiver/src/audio-ring.ts) と
[`playback-worklet.js`](../receiver/src/playback-worklet.js) の **両方に複製**している（worklet が
import を持てないため）。変更時は両方を一致させること。

---

## 5. 動作確認手順

1. `build.bat` で再ビルド・インストール（receiver も同時更新）。
2. OBS 起動 → 受信ページを **ハードリロード（Ctrl+Shift+R）**。COOP/COEP を確実に反映するため。
3. F12 Console に **`[audio] AudioWorklet 再生を有効化`** が出れば新経路が有効
   （出ない＝isolation 未適用＝従来経路）。
4. 再生バッファを低め（100ms 程度）に設定し、自動再同期も併用して数分連続再生。

確認ポイント:
- 低レイテンシ（~100ms）で再生中の途切れ・再同期時の音飛びが実用水準に収まること。
- `crossOriginIsolated` が true（DevTools コンソールで確認可）。
- SAB 不可環境（ヘッダ未適用）でも従来経路で音が出ること（フォールバック）。

---

## 6. 既知の限界 / 次の打ち手

- WS 受信＋デコードはメインスレッドのまま。重い描画や GC が残っていれば稀にジッタが出る。
  → **受信＋デコードを Web Worker へ移し**、Worker から SAB リングへ直接書き込めば、
  音声経路を完全にメインスレッド非依存にできる（さらなる低レイテンシ安定化）。
- worklet の target（プリバッファ量）やアンダーラン復帰の滑らかさは調整余地あり。
