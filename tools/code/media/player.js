(() => {
  const vscode = acquireVsCodeApi();
  const cfg = window.MDXNT_CONFIG;

  const songTitle = document.getElementById('songTitle');
  const sizeLabel = document.getElementById('sizeLabel');
  const btnPlayPause = document.getElementById('btnPlayPause');
  const btnStop = document.getElementById('btnStop');
  const btnRewind = document.getElementById('btnRewind');
  const btnForward = document.getElementById('btnForward');
  const seekSlider = document.getElementById('seekSlider');
  const seekVal = document.getElementById('seekVal');
  const meterBar = document.getElementById('meterBar');
  const statTick = document.getElementById('statTick');
  const statBar = document.getElementById('statBar');
  const statBpm = document.getElementById('statBpm');
  const speedButtons = document.getElementById('speedButtons');
  const chButtons = document.getElementById('chButtons');
  const statusMsg = document.getElementById('statusMsg');

  let audioCtx = null;
  let workletNode = null;
  let workletReady = false;
  let initPromise = null;
  let wasmBytes = null;
  let lastTrack = null;
  let isPlaying = false;
  let isPaused = false;
  let currentTick = 0;
  let currentSpeed = 100;
  let muteMask = 0;
  let isScrubbing = false;
  let activeSeq = 0;

  function setStatus(text) {
    statusMsg.textContent = text;
  }

  function tbToBpm(tb) {
    const denom = 256 - (tb & 0xff);
    if (denom <= 0) return 120;
    return Math.round(4333.333333333333 / denom);
  }

  function updatePlayButton() {
    const suspended = audioCtx && audioCtx.state === 'suspended';
    btnPlayPause.textContent = isPlaying && !isPaused && !suspended ? '⏸ Pause' : '▶ Play';
  }

  function updateSpeedUI(speed) {
    currentSpeed = speed;
    for (const btn of speedButtons.querySelectorAll('button')) {
      btn.classList.toggle('active', Number(btn.dataset.speed) === speed);
    }
  }

  function updateMuteUI() {
    for (const btn of chButtons.querySelectorAll('button')) {
      const ch = Number(btn.dataset.ch);
      btn.classList.toggle('muted-ch', (muteMask & (1 << ch)) !== 0);
    }
  }

  function resetPositionUI() {
    currentTick = 0;
    statTick.textContent = '0';
    statBar.textContent = '1 : 1';
    seekSlider.max = '3840';
    seekSlider.value = '0';
    seekVal.textContent = '0';
    meterBar.style.width = '0%';
  }

  function handleWorkletMessage(m) {
    if (m.type === 'status') {
      if (typeof m.seq === 'number' && m.seq < activeSeq) return;
      currentTick = m.tick;
      isPlaying = m.playing;
      if (typeof m.paused === 'boolean') isPaused = m.paused;
      statTick.textContent = String(m.tick);
      const bar = Math.floor(m.tick / 192) + 1;
      const beat = Math.floor((m.tick % 192) / 48) + 1;
      statBar.textContent = `${bar} : ${beat}`;
      statBpm.textContent = `${tbToBpm(m.tb)} (${m.tb})`;
      meterBar.style.width = `${Math.min(100, Math.round(m.peak * 150))}%`;
      if (!isScrubbing) {
        if (m.tick > Number(seekSlider.max)) {
          seekSlider.max = String(Math.ceil((m.tick + 192) / 192) * 192);
        }
        seekSlider.value = String(m.tick);
        seekVal.textContent = String(m.tick);
      }
      if (!isPlaying && lastTrack) {
        setStatus('Finished');
      }
      updatePlayButton();
    } else if (m.type === 'loaded') {
      activeSeq = m.seq || activeSeq;
      if (!m.ok) {
        isPlaying = false;
        isPaused = false;
        setStatus(`Failed to load: ${m.message || 'invalid .mdxt binary'}`);
      } else {
        isPlaying = !!m.playing;
        isPaused = false;
        statBpm.textContent = `${tbToBpm(m.tb)} (${m.tb})`;
      }
      updatePlayButton();
    } else if (m.type === 'error') {
      setStatus(`Error: ${m.message}`);
      vscode.postMessage({ type: 'error', message: m.message });
    }
  }

  async function ensureAudio(fromUserGesture) {
    if (!initPromise) {
      initPromise = (async () => {
        let ctx = null;
        try {
          ctx = new (window.AudioContext || window.webkitAudioContext)();
          ctx.onstatechange = () => updatePlayButton();
          await ctx.audioWorklet.addModule(cfg.workletUrl);
          const node = new AudioWorkletNode(ctx, 'mdxnt-opn-processor', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2],
          });
          node.connect(ctx.destination);

          if (!wasmBytes) {
            const resp = await fetch(cfg.wasmUrl);
            if (!resp.ok) throw new Error(`Failed to fetch Wasm: ${resp.status}`);
            wasmBytes = await resp.arrayBuffer();
          }

          await new Promise((resolve, reject) => {
            node.port.onmessage = (e) => {
              const m = e.data;
              if (m.type === 'inited') {
                node.port.onmessage = (ev) => handleWorkletMessage(ev.data);
                resolve();
              } else if (m.type === 'error') {
                reject(new Error(m.message));
              }
            };
            node.port.postMessage({ type: 'init', wasmBytes: wasmBytes.slice(0) });
          });

          audioCtx = ctx;
          workletNode = node;
          workletReady = true;
        } catch (err) {
          if (ctx) {
            try { await ctx.close(); } catch (_) {}
          }
          audioCtx = null;
          workletNode = null;
          workletReady = false;
          initPromise = null;
          throw err;
        }
      })();
    }

    await initPromise;

    if (audioCtx && audioCtx.state === 'suspended') {
      if (fromUserGesture) {
        await audioCtx.resume();
      } else {
        // Never await resume() outside a user gesture; Chromium keeps it pending until clicked.
        audioCtx.resume().catch(() => {});
      }
    }
  }

  async function loadAndPlay(name, dataArray, fromUserGesture = false) {
    lastTrack = { name, data: dataArray };
    songTitle.textContent = name;
    sizeLabel.textContent = `${dataArray.length} B`;
    resetPositionUI();
    try {
      await ensureAudio(fromUserGesture);
      activeSeq++;
      workletNode.port.postMessage({ type: 'setSpeed', speed: currentSpeed });
      workletNode.port.postMessage({ type: 'setMuteMask', mask: muteMask });
      workletNode.port.postMessage({ type: 'load', data: dataArray });
      isPlaying = true;
      isPaused = false;
      updatePlayButton();
      if (audioCtx.state === 'suspended') {
        setStatus('Loaded. Click ▶ Play once to unlock WebAudio.');
      } else {
        setStatus(`Playing ${name} (${currentSpeed / 100}x)`);
      }
    } catch (err) {
      setStatus(`Playback error: ${err.message}`);
      vscode.postMessage({ type: 'error', message: err.message });
    }
  }

  btnPlayPause.addEventListener('click', async () => {
    if (!lastTrack) {
      setStatus('No track loaded. Use ▶ in an .mml editor.');
      return;
    }
    try {
      const wasSuspended = audioCtx && audioCtx.state === 'suspended';
      await ensureAudio(true);
      if (wasSuspended && isPlaying && !isPaused) {
        updatePlayButton();
        setStatus(`Playing ${lastTrack.name} (${currentSpeed / 100}x)`);
        return;
      }
      if (!isPlaying) {
        await loadAndPlay(lastTrack.name, lastTrack.data, true);
      } else if (isPaused) {
        isPaused = false;
        workletNode.port.postMessage({ type: 'resume' });
        updatePlayButton();
        setStatus(`Playing ${lastTrack.name} (${currentSpeed / 100}x)`);
      } else {
        isPaused = true;
        workletNode.port.postMessage({ type: 'pause' });
        updatePlayButton();
        setStatus('Paused');
      }
    } catch (err) {
      setStatus(`Playback error: ${err.message}`);
      vscode.postMessage({ type: 'error', message: err.message });
    }
  });

  btnStop.addEventListener('click', () => {
    activeSeq++;
    if (workletReady && workletNode) {
      workletNode.port.postMessage({ type: 'stop' });
    }
    isPlaying = false;
    isPaused = false;
    resetPositionUI();
    updatePlayButton();
    setStatus('Stopped');
  });

  btnRewind.addEventListener('click', () => {
    if (!workletReady || !workletNode || !lastTrack) return;
    const target = Math.max(0, currentTick - 192);
    workletNode.port.postMessage({ type: 'seek', ticks: target });
  });

  btnForward.addEventListener('click', () => {
    if (!workletReady || !workletNode || !lastTrack) return;
    const target = currentTick + 192;
    workletNode.port.postMessage({ type: 'seek', ticks: target });
  });

  seekSlider.addEventListener('input', () => {
    isScrubbing = true;
    seekVal.textContent = seekSlider.value;
  });

  seekSlider.addEventListener('change', () => {
    isScrubbing = false;
    if (workletReady && workletNode && lastTrack) {
      workletNode.port.postMessage({ type: 'seek', ticks: Number(seekSlider.value) });
    }
  });

  speedButtons.addEventListener('click', (e) => {
    const btn = e.target.closest('button[data-speed]');
    if (!btn) return;
    const speed = Number(btn.dataset.speed);
    updateSpeedUI(speed);
    if (workletReady && workletNode) {
      workletNode.port.postMessage({ type: 'setSpeed', speed });
    }
    vscode.postMessage({ type: 'speedChanged', speed });
    setStatus(`Speed: ${speed / 100}x`);
  });

  chButtons.addEventListener('click', (e) => {
    const btn = e.target.closest('button[data-ch]');
    if (!btn) return;
    const ch = Number(btn.dataset.ch);
    if (e.shiftKey) {
      // Solo toggle
      const soloMask = 0x3f ^ (1 << ch);
      muteMask = muteMask === soloMask ? 0 : soloMask;
    } else {
      muteMask ^= 1 << ch;
    }
    updateMuteUI();
    if (workletReady && workletNode) {
      workletNode.port.postMessage({ type: 'setMuteMask', mask: muteMask });
    }
  });

  window.addEventListener('message', (event) => {
    const msg = event.data;
    if (msg.type === 'load') {
      updateSpeedUI(100);
      loadAndPlay(msg.name, msg.data, false);
    } else if (msg.type === 'stop') {
      btnStop.click();
    } else if (msg.type === 'setSpeed') {
      updateSpeedUI(msg.speed);
      if (workletReady && workletNode) {
        workletNode.port.postMessage({ type: 'setSpeed', speed: msg.speed });
      }
      setStatus(`Speed: ${msg.speed / 100}x`);
    }
  });

  vscode.postMessage({ type: 'ready' });
})();
