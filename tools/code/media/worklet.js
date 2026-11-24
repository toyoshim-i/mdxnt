class MdxntWorkletProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.exports = null;
    this.paused = false;
    this.loaded = false;
    this.wasPlaying = false;
    this.seq = 0;
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
          this.seq = typeof msg.seq === 'number' ? msg.seq : this.seq + 1;
          const bytes = new Uint8Array(msg.data);
          const cap = this.exports.mdxnt_wasm_get_data_capacity();
          if (bytes.length > cap) {
            this.loaded = false;
            this.wasPlaying = false;
            this.port.postMessage({ type: 'loaded', ok: false, seq: this.seq, message: `MDXT too large (${bytes.length} > ${cap} B)` });
            return;
          }
          const ptr = this.exports.mdxnt_wasm_get_data_buf();
          new Uint8Array(this.exports.memory.buffer, ptr, bytes.length).set(bytes);
          const ok = this.exports.mdxnt_wasm_load(bytes.length) === 1;
          this.loaded = ok;
          this.paused = false;
          this.wasPlaying = ok && this.exports.mdxnt_wasm_is_playing() === 1;
          this.statusCounter = 0;
          this.port.postMessage({
            type: 'loaded',
            ok,
            seq: this.seq,
            playing: this.wasPlaying,
            tb: this.exports.mdxnt_wasm_get_tb(),
          });
        } else if (msg.type === 'stop' && this.exports) {
          this.seq = typeof msg.seq === 'number' ? msg.seq : this.seq + 1;
          this.exports.mdxnt_wasm_stop();
          this.paused = false;
          this.wasPlaying = false;
          this.port.postMessage({
            type: 'status',
            seq: this.seq,
            tick: 0,
            tb: this.exports.mdxnt_wasm_get_tb(),
            playing: false,
            paused: false,
            finished: false,
            peak: 0,
          });
        } else if (msg.type === 'pause') {
          this.paused = true;
        } else if (msg.type === 'resume') {
          this.paused = false;
        } else if (msg.type === 'setSpeed' && this.exports) {
          this.exports.mdxnt_wasm_set_speed(msg.speed | 0);
        } else if (msg.type === 'seek' && this.exports && this.loaded) {
          const tick = this.exports.mdxnt_wasm_seek_ticks(Math.max(0, msg.ticks | 0));
          const playing = this.exports.mdxnt_wasm_is_playing() === 1;
          this.wasPlaying = playing;
          this.port.postMessage({
            type: 'status',
            seq: this.seq,
            tick,
            tb: this.exports.mdxnt_wasm_get_tb(),
            playing,
            paused: this.paused,
            peak: 0,
          });
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

    if (!this.exports || !this.loaded || this.paused) {
      chL.fill(0);
      if (chR !== chL) chR.fill(0);
      return true;
    }

    const playingBefore = this.exports.mdxnt_wasm_is_playing() === 1;
    if (!playingBefore) {
      chL.fill(0);
      if (chR !== chL) chR.fill(0);
      if (this.wasPlaying) {
        this.wasPlaying = false;
        this.port.postMessage({
          type: 'status',
          seq: this.seq,
          tick: this.exports.mdxnt_wasm_get_tick(),
          tb: this.exports.mdxnt_wasm_get_tb(),
          playing: false,
          paused: false,
          finished: true,
          peak: 0,
        });
      }
      return true;
    }

    const rendered = this.exports.mdxnt_wasm_render(frames);
    const pcmPtr = this.exports.mdxnt_wasm_get_pcm_buf();
    const pcm = new Float32Array(this.exports.memory.buffer, pcmPtr, rendered * 2);

    let peak = 0;
    for (let i = 0; i < rendered; i++) {
      const l = pcm[i * 2];
      const r = pcm[i * 2 + 1];
      chL[i] = l;
      if (chR !== chL) chR[i] = r;
      const a = Math.abs(l);
      if (a > peak) peak = a;
    }
    for (let i = rendered; i < frames; i++) {
      chL[i] = 0;
      if (chR !== chL) chR[i] = 0;
    }

    const playingAfter = this.exports.mdxnt_wasm_is_playing() === 1;
    this.wasPlaying = playingAfter;

    this.statusCounter++;
    if (this.statusCounter >= 6 || !playingAfter) {
      this.statusCounter = 0;
      this.port.postMessage({
        type: 'status',
        seq: this.seq,
        tick: this.exports.mdxnt_wasm_get_tick(),
        tb: this.exports.mdxnt_wasm_get_tb(),
        playing: playingAfter,
        paused: this.paused,
        finished: !playingAfter,
        peak: playingAfter ? peak : 0,
      });
    }

    return true;
  }
}

registerProcessor('mdxnt-opn-processor', MdxntWorkletProcessor);
