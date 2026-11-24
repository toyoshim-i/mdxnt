class MdxntWorkletProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.exports = null;
    this.paused = false;
    this.loaded = false;
    this.statusCounter = 0;

    this.port.onmessage = async (event) => {
      const msg = event.data;
      try {
        if (msg.type === 'init') {
          const imports = {
            wasi_snapshot_preview1: {
              fd_close: () => 0,
              fd_seek: () => 0,
              fd_write: () => 0,
            },
          };
          const { instance } = await WebAssembly.instantiate(msg.wasmBytes, imports);
          this.exports = instance.exports;
          if (typeof this.exports._initialize === 'function') {
            this.exports._initialize();
          }
          this.exports.mdxnt_wasm_init(Math.round(sampleRate));
          this.port.postMessage({ type: 'inited' });
        } else if (msg.type === 'load' && this.exports) {
          const bytes = new Uint8Array(msg.data);
          const cap = this.exports.mdxnt_wasm_get_data_capacity();
          if (bytes.length > cap) {
            this.port.postMessage({ type: 'error', message: `MDXT too large (${bytes.length} > ${cap} B)` });
            return;
          }
          const ptr = this.exports.mdxnt_wasm_get_data_buf();
          new Uint8Array(this.exports.memory.buffer, ptr, bytes.length).set(bytes);
          const ok = this.exports.mdxnt_wasm_load(bytes.length);
          this.loaded = ok === 1;
          this.paused = false;
          this.port.postMessage({ type: 'loaded', ok: this.loaded });
        } else if (msg.type === 'stop' && this.exports) {
          this.exports.mdxnt_wasm_stop();
          this.paused = false;
          this.port.postMessage({ type: 'status', tick: 0, tb: this.exports.mdxnt_wasm_get_tb(), playing: false, peak: 0 });
        } else if (msg.type === 'pause') {
          this.paused = true;
        } else if (msg.type === 'resume') {
          this.paused = false;
        } else if (msg.type === 'setSpeed' && this.exports) {
          this.exports.mdxnt_wasm_set_speed(msg.speed | 0);
        } else if (msg.type === 'seek' && this.exports) {
          const tick = this.exports.mdxnt_wasm_seek_ticks(Math.max(0, msg.ticks | 0));
          this.port.postMessage({ type: 'status', tick, tb: this.exports.mdxnt_wasm_get_tb(), playing: !this.paused, peak: 0 });
        } else if (msg.type === 'setMuteMask' && this.exports) {
          this.exports.mdxnt_wasm_set_mute_mask(msg.mask | 0);
        }
      } catch (err) {
        this.port.postMessage({ type: 'error', message: String(err) });
      }
    };
  }

  process(_inputs, outputs) {
    const output = outputs[0];
    if (!output || output.length === 0) return true;
    const chL = output[0];
    const chR = output[1] || output[0];
    const frames = chL.length;

    if (!this.exports || !this.loaded || this.paused || !this.exports.mdxnt_wasm_is_playing()) {
      chL.fill(0);
      if (chR !== chL) chR.fill(0);
      return true;
    }

    this.exports.mdxnt_wasm_render(frames);
    const pcmPtr = this.exports.mdxnt_wasm_get_pcm_buf();
    const pcm = new Float32Array(this.exports.memory.buffer, pcmPtr, frames * 2);

    let peak = 0;
    for (let i = 0; i < frames; i++) {
      const l = pcm[i * 2];
      const r = pcm[i * 2 + 1];
      chL[i] = l;
      if (chR !== chL) chR[i] = r;
      const a = Math.abs(l);
      if (a > peak) peak = a;
    }

    this.statusCounter++;
    if (this.statusCounter >= 6) {
      this.statusCounter = 0;
      this.port.postMessage({
        type: 'status',
        tick: this.exports.mdxnt_wasm_get_tick(),
        tb: this.exports.mdxnt_wasm_get_tb(),
        playing: this.exports.mdxnt_wasm_is_playing() === 1 && !this.paused,
        peak,
      });
    }

    return true;
  }
}

registerProcessor('mdxnt-opn-processor', MdxntWorkletProcessor);
