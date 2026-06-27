// AudioWorklet 再生プロセッサ（素の JS・自己完結）。
//
// audio-ring の SharedArrayBuffer リングから音声を読み出してオーディオスレッドで
// 出力する。メインスレッドが GC や描画で停止しても再生は途切れない。
// このファイルは ?raw で文字列として取り込み、Blob URL 経由で addModule する
// （Vite が .ts を data URL 化して壊す問題を避けるため）。レイアウト定数は
// audio-ring.ts と一致させること。

const RING_CHANNELS = 2;
const RING_CAPACITY_FRAMES = 1 << 16;
const RING_MASK = RING_CAPACITY_FRAMES - 1;
const CTL_LEN = 8;
const CTL_WRITE = 0;
const CTL_READ = 1;
const CTL_TARGET = 2;
const CTL_MAX = 3;
const CTL_RECENTER = 4;

class PlaybackProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const sab = options.processorOptions.sab;
    this.ctrl = new Int32Array(sab, 0, CTL_LEN);
    this.data = new Float32Array(sab, CTL_LEN * 4, RING_CAPACITY_FRAMES * RING_CHANNELS);
    this.started = false;
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const chCount = out.length;
    const frames = out[0].length;
    const ctrl = this.ctrl;
    const data = this.data;

    const w = Atomics.load(ctrl, CTL_WRITE);
    let r = Atomics.load(ctrl, CTL_READ);
    let avail = w - r;

    const target = Atomics.load(ctrl, CTL_TARGET);
    const max = Atomics.load(ctrl, CTL_MAX) || target * 2;

    // 再同期要求 or 上限超過 → target まで古いデータを捨てる（低レイテンシ維持）。
    if ((Atomics.load(ctrl, CTL_RECENTER) === 1 || (max > 0 && avail > max)) && avail > target) {
      r = w - target;
      Atomics.store(ctrl, CTL_READ, r);
      avail = target;
    }
    Atomics.store(ctrl, CTL_RECENTER, 0);

    // プリバッファ: target に満たなければ無音。
    if (!this.started) {
      if (target > 0 && avail >= target) {
        this.started = true;
      } else {
        for (let c = 0; c < chCount; c++) out[c].fill(0);
        return true;
      }
    }

    const copy = avail >= frames ? frames : avail;
    for (let i = 0; i < frames; i++) {
      if (i < copy) {
        const pos = ((r + i) & RING_MASK) * RING_CHANNELS;
        for (let c = 0; c < chCount; c++) {
          out[c][i] = data[pos + (c < RING_CHANNELS ? c : RING_CHANNELS - 1)];
        }
      } else {
        for (let c = 0; c < chCount; c++) out[c][i] = 0;
      }
    }

    if (copy >= frames) {
      Atomics.store(ctrl, CTL_READ, r + frames);
    } else {
      // アンダーラン: 残りを消費し、再プリバッファへ。
      Atomics.store(ctrl, CTL_READ, w);
      this.started = false;
    }
    return true;
  }
}

registerProcessor('playback-processor', PlaybackProcessor);
