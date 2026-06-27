// 受信音声を AudioWorklet へ低レイテンシかつメインスレッド非依存で渡すための
// SharedArrayBuffer リングバッファ。
//
// 単一プロデューサ（メインスレッド: デコード結果を書き込む）／
// 単一コンシューマ（AudioWorklet: 再生のため読み出す）の lock-free リング。
// インターリーブのステレオ float を保持する。read/write 位置は Atomics で共有。

export const RING_CHANNELS = 2;
// フレーム容量（2のべき乗。約1.36秒 @48kHz）。マスクで高速に剰余を取る。
export const RING_CAPACITY_FRAMES = 1 << 16;
export const RING_MASK = RING_CAPACITY_FRAMES - 1;

// Int32 ヘッダのインデックス。
export const CTL_WRITE = 0; ///< 書き込み済みフレーム数（絶対値・単調増加）
export const CTL_READ = 1; ///< 読み出し済みフレーム数（絶対値・単調増加）
export const CTL_TARGET = 2; ///< 目標プリバッファ（フレーム数）
export const CTL_MAX = 3; ///< 許容最大滞留（フレーム数。超過分は破棄）
export const CTL_RECENTER = 4; ///< 1 で「目標まで切り詰めて再同期」を要求
export const CTL_LEN = 8; ///< ヘッダの Int32 個数

export function createRingSAB(): SharedArrayBuffer {
  const headerBytes = CTL_LEN * 4;
  const dataBytes = RING_CAPACITY_FRAMES * RING_CHANNELS * 4;
  return new SharedArrayBuffer(headerBytes + dataBytes);
}

export interface RingViews {
  ctrl: Int32Array;
  data: Float32Array;
}

export function ringViews(sab: SharedArrayBuffer): RingViews {
  const ctrl = new Int32Array(sab, 0, CTL_LEN);
  const data = new Float32Array(sab, CTL_LEN * 4, RING_CAPACITY_FRAMES * RING_CHANNELS);
  return { ctrl, data };
}

/**
 * メインスレッド側の書き込みハンドル。
 * デコード済みのインターリーブステレオ float をリングへ書き込む。
 */
export class RingWriter {
  private ctrl: Int32Array;
  private data: Float32Array;

  constructor(sab: SharedArrayBuffer) {
    const v = ringViews(sab);
    this.ctrl = v.ctrl;
    this.data = v.data;
  }

  /// インターリーブステレオ float を frames フレーム書き込む。空きが足りなければ末尾を捨てる。
  write(interleaved: Float32Array, frames: number): void {
    const ctrl = this.ctrl;
    const data = this.data;
    const w = Atomics.load(ctrl, CTL_WRITE);
    const r = Atomics.load(ctrl, CTL_READ);
    const free = RING_CAPACITY_FRAMES - (w - r);
    let n = frames;
    if (n > free) n = free;
    for (let i = 0; i < n; i++) {
      const pos = ((w + i) & RING_MASK) * RING_CHANNELS;
      data[pos] = interleaved[i * 2];
      data[pos + 1] = interleaved[i * 2 + 1];
    }
    Atomics.store(ctrl, CTL_WRITE, w + n);
  }

  setTarget(frames: number): void {
    Atomics.store(this.ctrl, CTL_TARGET, frames);
  }

  setMax(frames: number): void {
    Atomics.store(this.ctrl, CTL_MAX, frames);
  }

  /// 目標まで切り詰めて再同期するよう要求する。
  recenter(): void {
    Atomics.store(this.ctrl, CTL_RECENTER, 1);
  }

  /// バッファを空にして再プリバッファさせる（接続切替時など）。
  reset(): void {
    const w = Atomics.load(this.ctrl, CTL_WRITE);
    Atomics.store(this.ctrl, CTL_READ, w);
  }

  /// 現在の滞留フレーム数。
  buffered(): number {
    return Atomics.load(this.ctrl, CTL_WRITE) - Atomics.load(this.ctrl, CTL_READ);
  }
}
