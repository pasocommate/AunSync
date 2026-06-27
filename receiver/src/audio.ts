import { state } from './state';
import { NUM_BARS } from './constants';
import { scheduleSustain } from './sustain';
import {
  volSlider,
  dbDisplay,
  muteBtn,
  muteIcon,
  meterEl,
  infoBuf,
  infoSR,
  infoCH,
} from './elements';
import { setCodecLabel } from './ui';
import { t } from './i18n';
import type { WindowWithWebkitAudioContext, AudioDataLike } from './types';
import { RingWriter, createRingSAB, RING_CHANNELS } from './audio-ring';
import playbackWorkletSource from './playback-worklet.js?raw';

// ============================================================
// AudioWorklet 再生（低レイテンシ・メインスレッド非依存）
// ============================================================
//
// SharedArrayBuffer が使える（cross-origin isolated）環境では、デコード結果を
// リングバッファへ書き込み、AudioWorklet（オーディオスレッド）が再生する。
// メインスレッドが GC や描画で一時停止しても再生が途切れない。
// SAB が使えない環境では従来の playBuffer 経路にフォールバックする。

let ringWriter: RingWriter | null = null;
let workletNode: AudioWorkletNode | null = null;
let ringSampleRate = 0;
let interleaveTmp = new Float32Array(0);
let meterRafActive = false;

export function workletPlaybackAvailable(): boolean {
  return (
    typeof SharedArrayBuffer !== 'undefined' &&
    (self as unknown as { crossOriginIsolated?: boolean }).crossOriginIsolated === true
  );
}

function ensureTmp(frames: number): Float32Array {
  if (interleaveTmp.length < frames * 2) interleaveTmp = new Float32Array(frames * 2);
  return interleaveTmp;
}

function applyRingTarget(): void {
  if (!ringWriter || ringSampleRate <= 0) return;
  const target = Math.round(state.playbackBuffer * ringSampleRate);
  ringWriter.setTarget(target);
  // 上限 = 目標の2倍 + 60ms。超過分は AudioWorklet 側が破棄して低レイテンシを保つ。
  ringWriter.setMax(Math.round(state.playbackBuffer * 2 * ringSampleRate) + Math.round(0.06 * ringSampleRate));
}

/// 再生バッファ（目標）が変わったときに反映する。
export function onPlaybackBufferChanged(): void {
  applyRingTarget();
  if (ringWriter) ringWriter.recenter();
}

/// 接続切替時などに再生状態を初期化する。
export function resetPlayback(): void {
  if (ringWriter) ringWriter.reset();
  state.nextTime = 0;
}

/// 手動/自動再同期: リング再生時は目標まで切り詰める（音飛びを最小化）。
export function recenterRing(): boolean {
  if (!ringWriter) return false;
  ringWriter.recenter();
  return true;
}

async function setupWorkletNode(actx: AudioContext, sab: SharedArrayBuffer): Promise<void> {
  // worklet ソースを Blob URL 化して addModule する（Vite の .ts→data URL 化を回避）。
  const blob = new Blob([playbackWorkletSource], { type: 'application/javascript' });
  const moduleUrl = URL.createObjectURL(blob);
  try {
    await actx.audioWorklet.addModule(moduleUrl);
    if (!state.actx || state.actx !== actx || !state.gainNode) return;
    const node = new AudioWorkletNode(actx, 'playback-processor', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [RING_CHANNELS],
      processorOptions: { sab },
    });
    node.connect(state.gainNode);
    workletNode = node;
    console.info('[audio] AudioWorklet 再生を有効化（低レイテンシ・メインスレッド非依存）');
  } catch (e) {
    console.warn('[audio] AudioWorklet 初期化に失敗。従来再生へフォールバック', e);
    ringWriter = null;
    workletNode = null;
  } finally {
    URL.revokeObjectURL(moduleUrl);
  }
}

// インターリーブステレオ float をリングへ書き込む（chCount=1 は複製）。
function pushPlanar(planes: Float32Array[], frames: number, chCount: number): void {
  if (!ringWriter) return;
  const tmp = ensureTmp(frames);
  const l = planes[0];
  const r = chCount > 1 && planes[1] ? planes[1] : planes[0];
  for (let i = 0; i < frames; i++) {
    tmp[i * 2] = l[i];
    tmp[i * 2 + 1] = r[i];
  }
  ringWriter.write(tmp, frames);
}

function pushInterleavedFloat(src: Float32Array, frames: number, chCount: number): void {
  if (!ringWriter) return;
  if (chCount === RING_CHANNELS) {
    ringWriter.write(src, frames);
    return;
  }
  const tmp = ensureTmp(frames);
  for (let i = 0; i < frames; i++) {
    const s = src[i * chCount];
    tmp[i * 2] = s;
    tmp[i * 2 + 1] = chCount > 1 ? src[i * chCount + 1] : s;
  }
  ringWriter.write(tmp, frames);
}

/// WebCodecs / WASM のデコード結果（プレーナ float）をリングへ。ringWriter が無ければ false。
export function pushDecodedPlanar(planes: Float32Array[], frames: number, chCount: number): boolean {
  if (!ringWriter) return false;
  pushPlanar(planes, frames, chCount);
  return true;
}

/// インターリーブ float をリングへ。ringWriter が無ければ false。
export function pushDecodedInterleaved(src: Float32Array, frames: number, chCount: number): boolean {
  if (!ringWriter) return false;
  pushInterleavedFloat(src, frames, chCount);
  return true;
}

export function ringActive(): boolean {
  return ringWriter !== null;
}

let copyTmpF32 = new Float32Array(0);
let planeL = new Float32Array(0);
let planeR = new Float32Array(0);
let copyTmpI16 = new Int16Array(0);

/// WebCodecs の AudioData をリングへ書き込む（AudioBuffer を作らず GC を抑える）。
/// ringWriter が無い、または未対応フォーマットなら false。
export function pushAudioData(ad: AudioDataLike): boolean {
  if (!ringWriter) return false;
  const frames = ad.numberOfFrames;
  const channels = ad.numberOfChannels;
  const fmt = ad.format || '';
  const planar = fmt.endsWith('-planar');
  const f32 = fmt.startsWith('f32');
  const s16 = fmt.startsWith('s16');
  if (!f32 && !s16) return false;

  if (planar) {
    if (planeL.length < frames) planeL = new Float32Array(frames);
    if (planeR.length < frames) planeR = new Float32Array(frames);
    if (f32) {
      ad.copyTo(planeL, { planeIndex: 0 });
      if (channels > 1) ad.copyTo(planeR, { planeIndex: 1 });
    } else {
      if (copyTmpI16.length < frames) copyTmpI16 = new Int16Array(frames);
      ad.copyTo(copyTmpI16, { planeIndex: 0 });
      for (let i = 0; i < frames; i++) planeL[i] = copyTmpI16[i] / 32768;
      if (channels > 1) {
        ad.copyTo(copyTmpI16, { planeIndex: 1 });
        for (let i = 0; i < frames; i++) planeR[i] = copyTmpI16[i] / 32768;
      }
    }
    pushPlanar([planeL, channels > 1 ? planeR : planeL], frames, channels);
    return true;
  }

  const total = frames * channels;
  if (f32) {
    if (copyTmpF32.length < total) copyTmpF32 = new Float32Array(total);
    ad.copyTo(copyTmpF32, { planeIndex: 0 });
    pushInterleavedFloat(copyTmpF32, frames, channels);
  } else {
    if (copyTmpI16.length < total) copyTmpI16 = new Int16Array(total);
    ad.copyTo(copyTmpI16, { planeIndex: 0 });
    const tmp = ensureTmp(frames);
    for (let i = 0; i < frames; i++) {
      const l = copyTmpI16[i * channels] / 32768;
      tmp[i * 2] = l;
      tmp[i * 2 + 1] = channels > 1 ? copyTmpI16[i * channels + 1] / 32768 : l;
    }
    ringWriter.write(tmp, frames);
  }
  return true;
}

function startMeterLoop(): void {
  if (meterRafActive) return;
  meterRafActive = true;
  const tick = (): void => {
    if (!meterRafActive) return;
    updateMeter();
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

const ANALYZER_FFT_SIZE = 512;
const ANALYZER_SMOOTHING = 0.82;
const METER_ANALYZER_SMOOTHING = 0;
const ANALYZER_MIN_DB = -90;
const ANALYZER_MAX_DB = -24;
const ANALYZER_MIN_HZ = 60;
const ANALYZER_MAX_HZ = 12000;
const METER_UPDATE_INTERVAL_MS = 33;
const METER_DIGITAL_STEPS = 24;

// ============================================================
// 音量
// ============================================================

export function sliderToGain(val: number): number {
  if (val <= 0) return 0;
  return Math.pow(val / 100, 2);
}

export function gainToDb(gain: number): number {
  if (gain <= 0) return -Infinity;
  return 20 * Math.log10(gain);
}

export function formatDb(gain: number): string {
  if (gain <= 0) return '−∞ dB';
  const db = gainToDb(gain);
  return (db >= 0 ? '+' : '') + db.toFixed(1) + ' dB';
}

function updateVolumeDisplay(gain: number): void {
  volSlider.disabled = state.muted;
  volSlider.classList.toggle('is-primary', !state.muted);
  dbDisplay.textContent = state.muted ? t('audio.muted') : formatDb(gain);
  const dbClasses = [
    'tag',
    'is-medium',
    'has-background-dark',
    'has-text-white',
    'is-family-monospace',
  ];
  if (state.muted) dbClasses.push('has-text-grey');
  dbDisplay.className = dbClasses.join(' ');
}

export function onVolumeChange(val: number | string): void {
  const v = typeof val === 'number' ? val : parseInt(val, 10);
  const safeV = Number.isFinite(v) ? v : 0;
  localStorage.setItem('volume', String(safeV));
  const gain = sliderToGain(safeV);
  if (state.gainNode && !state.muted) state.gainNode.gain.value = gain;
  updateVolumeDisplay(gain);
}

export function toggleMute(): void {
  state.muted = !state.muted;
  muteBtn.className = state.muted
    ? 'button is-ghost has-text-grey'
    : 'button is-primary';
  if (muteIcon)
    muteIcon.className = state.muted
      ? 'fas fa-volume-mute'
      : 'fas fa-volume-up';
  if (state.gainNode) {
    state.gainNode.gain.value = state.muted
      ? 0
      : sliderToGain(parseInt(volSlider.value, 10));
  }
  meterEl.classList.toggle('is-muted', state.muted);
  lastMutedState = state.muted;
  const gain = sliderToGain(parseInt(volSlider.value, 10));
  updateVolumeDisplay(gain);
}

// ============================================================
// AudioContext
// ============================================================

export function ensureAudioContext(sampleRate?: number): boolean {
  if (state.actx) {
    if (!sampleRate || state.actx.sampleRate === sampleRate) return true;
    // サンプルレート不一致 — 再生成して per-buffer リサンプリングノイズを回避
    try { state.actx.close(); } catch { /* */ }
    state.actx = null;
    state.gainNode = null;
    state.xfade = [null, null];
    state.nextTime = 0;
    state.lastBuffer = null;
    ringWriter = null;
    workletNode = null;
  }
  const AudioContextCtor =
    window.AudioContext ||
    (window as WindowWithWebkitAudioContext).webkitAudioContext;
  if (!AudioContextCtor) return false;
  try {
    state.actx = new AudioContextCtor(sampleRate ? { sampleRate } : undefined);
  } catch {
    state.actx = new AudioContextCtor();
  }
  state.gainNode = state.actx.createGain();
  state.gainNode.gain.value = sliderToGain(parseInt(volSlider.value, 10));
  meterAnalyserNode = state.actx.createAnalyser();
  meterAnalyserNode.fftSize = ANALYZER_FFT_SIZE;
  meterAnalyserNode.smoothingTimeConstant = METER_ANALYZER_SMOOTHING;
  meterAnalyserNode.minDecibels = ANALYZER_MIN_DB;
  meterAnalyserNode.maxDecibels = ANALYZER_MAX_DB;
  analyserNode = state.actx.createAnalyser();
  analyserNode.fftSize = ANALYZER_FFT_SIZE;
  analyserNode.smoothingTimeConstant = ANALYZER_SMOOTHING;
  analyserNode.minDecibels = ANALYZER_MIN_DB;
  analyserNode.maxDecibels = ANALYZER_MAX_DB;
  state.gainNode.connect(meterAnalyserNode);
  meterAnalyserNode.connect(analyserNode);
  analyserNode.connect(state.actx.destination);
  spectrumData = new Uint8Array(meterAnalyserNode.frequencyBinCount);
  spectrumRanges = buildSpectrumRanges(state.actx.sampleRate, meterAnalyserNode.fftSize);
  // クロスフェード用ノード 2 本を gainNode の手前に配置
  state.xfade[0] = state.actx.createGain();
  state.xfade[1] = state.actx.createGain();
  state.xfade[0].connect(state.gainNode);
  state.xfade[1].connect(state.gainNode);
  state.xfade[1].gain.value = 0;   // 非アクティブ側は無音
  state.xfadeIdx = 0;

  // AudioWorklet 再生（利用可能なら）。SAB リングを即座に用意して書き込みを開始し、
  // ノードは addModule 完了後に非同期で接続する（早着パケットはリングに溜まる）。
  if (workletPlaybackAvailable()) {
    try {
      const sab = createRingSAB();
      ringWriter = new RingWriter(sab);
      ringSampleRate = state.actx.sampleRate;
      applyRingTarget();
      void setupWorkletNode(state.actx, sab);
      startMeterLoop();
    } catch (e) {
      console.warn('[audio] リング初期化に失敗。従来再生へフォールバック', e);
      ringWriter = null;
    }
  }
  return true;
}

// ============================================================
// バッファ再生
// ============================================================

export function playBuffer(abuf: AudioBuffer | null): void {
  if (!abuf || !state.actx || !state.gainNode) return;
  const dest = state.xfade[state.xfadeIdx] || state.gainNode;

  const now = state.actx.currentTime;
  // アンダーラン検出: nextTime が過去に落ちた場合、サステインで隙間を埋める
  if (state.nextTime < now + 0.02) {
    const gapStart = Math.max(state.nextTime, now);
    const newNext = now + state.playbackBuffer;
    const gapSec = newNext - gapStart;
    if (gapSec > 0.005) {
      scheduleSustain(dest, gapStart, gapSec);
    }
    state.nextTime = newNext;
  }

  state.lastBuffer = abuf;
  const src = state.actx.createBufferSource();
  src.buffer = abuf;
  src.connect(dest);
  src.start(state.nextTime);
  const bufMs = Math.round((state.nextTime - state.actx.currentTime) * 1000);
  state.nextTime += abuf.duration;

  infoBuf.textContent = bufMs + ' ms';
  updateMeter();
}

// ============================================================
// PCM デコード
// ============================================================

export function updateAudioInfo(
  sampleRate: number,
  channels: number,
): void {
  infoSR.textContent = sampleRate + ' Hz';
  infoCH.textContent = channels === 1 ? 'Mono' : channels === 2 ? 'Stereo' : channels + 'ch';
}

export function handlePcm16(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  frameCount: number,
): void {
  ensureAudioContext(sampleRate);
  if (!state.actx) return;
  updateAudioInfo(sampleRate, channels);
  setCodecLabel('PCM');

  const pcm = new Int16Array(buf, 16);
  if (ringWriter) {
    const tmp = ensureTmp(frameCount);
    for (let f = 0; f < frameCount; f++) {
      const l = pcm[f * channels] / 32768;
      tmp[f * 2] = l;
      tmp[f * 2 + 1] = channels > 1 ? pcm[f * channels + 1] / 32768 : l;
    }
    ringWriter.write(tmp, frameCount);
    return;
  }
  const abuf = state.actx.createBuffer(channels, frameCount, sampleRate);
  for (let c = 0; c < channels; c++) {
    const d = abuf.getChannelData(c);
    for (let f = 0; f < frameCount; f++) {
      d[f] = pcm[f * channels + c] / 32768;
    }
  }
  playBuffer(abuf);
}

// ============================================================
// スペクトラムメーター
// ============================================================

let bars: HTMLDivElement[] = [];
let lastLevelSteps: number[] = [];
let analyserNode: AnalyserNode | null = null;
let meterAnalyserNode: AnalyserNode | null = null;
let spectrumData = new Uint8Array(0);
let spectrumRanges: Array<[number, number]> = [];
let lastMeterUpdateAt = 0;
let lastMutedState = false;

function buildSpectrumRanges(
  sampleRate: number,
  fftSize: number,
): Array<[number, number]> {
  const binCount = Math.max(1, Math.floor(fftSize / 2));
  const nyquist = sampleRate / 2;
  const minHz = Math.max(20, Math.min(ANALYZER_MIN_HZ, nyquist));
  const maxHz = Math.max(minHz, Math.min(ANALYZER_MAX_HZ, nyquist));
  const hzPerBin = nyquist / binCount || 1;
  const ranges: Array<[number, number]> = [];
  let prevEnd = -1;

  for (let i = 0; i < NUM_BARS; i++) {
    const startHz =
      minHz * Math.pow(maxHz / minHz, i / NUM_BARS);
    const endHz =
      minHz * Math.pow(maxHz / minHz, (i + 1) / NUM_BARS);
    let start = Math.floor(startHz / hzPerBin);
    let end = Math.floor(endHz / hzPerBin);

    if (start <= prevEnd) start = prevEnd + 1;
    if (start >= binCount) start = binCount - 1;
    if (end < start) end = start;
    if (end >= binCount) end = binCount - 1;

    ranges.push([start, end]);
    prevEnd = end;
  }
  return ranges;
}

export function initMeter(): void {
  bars = [];
  lastLevelSteps = new Array(NUM_BARS).fill(-1);
  lastMutedState = state.muted;
  meterEl.classList.toggle('is-muted', state.muted);
  meterEl.innerHTML = '';
  for (let i = 0; i < NUM_BARS; i++) {
    const b = document.createElement('div');
    b.className = 'bar';
    b.style.setProperty('--level', '0');
    meterEl.appendChild(b);
    bars.push(b);
  }
}

function updateMeter(): void {
  if (!meterAnalyserNode || spectrumData.length === 0 || bars.length === 0) return;
  const now = performance.now();
  if (now - lastMeterUpdateAt < METER_UPDATE_INTERVAL_MS) return;
  lastMeterUpdateAt = now;
  if (lastMutedState !== state.muted) {
    meterEl.classList.toggle('is-muted', state.muted);
    lastMutedState = state.muted;
  }

  meterAnalyserNode.getByteFrequencyData(spectrumData);
  for (let i = 0; i < bars.length; i++) {
    const range = spectrumRanges[i] || [0, 0];
    const start = range[0];
    const end = range[1];
    let sum = 0;
    for (let j = start; j <= end; j++) {
      sum += spectrumData[j] || 0;
    }
    const avg = sum / (end - start + 1);
    const normalized = avg / 255;
    const level = Math.min(1, Math.max(0, normalized));
    const levelStep = Math.round(level * METER_DIGITAL_STEPS);
    if (lastLevelSteps[i] !== levelStep) {
      const quantized = levelStep / METER_DIGITAL_STEPS;
      bars[i].style.setProperty('--level', quantized.toFixed(3));
      lastLevelSteps[i] = levelStep;
    }
  }
}
