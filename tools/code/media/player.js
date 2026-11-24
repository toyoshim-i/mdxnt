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
  let wasmBytes = null;
  let lastTrack = null;
  let isPlaying = false;
  let isPaused = false;
  let currentTick = 0;
  let currentSpeed = 100;
  let muteMask = 0;
  let isScrubbing = false;

  function setStatus(text) {
    statusMsg.textContent = text;
  }

  function tbToBpm(tb) {
    const denom = 256 - (tb & 0xff);
    if (denom <= 0) return 120;
    return Math.round(4333.333333333333 / denom);
  }

  function updatePlayButton() {
    btnPlayPause.textContent = isPlaying && !isPaused ? '⏸ Pause' : '▶ Play';
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

  async function ensureAudio() {
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      await audioCtx.audioWorklet.addModule(cfg.workletUrl);
      workletNode = new AudioWorkletNode(audioCtx, 'mdxnt-opn-processor', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
      });
      workletNode.connect(audioCtx.destination);

      if (!wasmBytes) {
        const resp = await fetch(cfg.wasmUrl);
        if (!resp.ok) throw new Error(`Failed to fetch Wasm: ${resp.status}`);
        wasmBytes = await resp.arrayBuffer();
      }

      await new Promise((resolve, reject) => {
        workletNode.port.onmessage = (e) => {
          const m = e.data;
          if (m.type === 'inited') {
            workletReady = true;
            resolve();
          } else if (m.type === 'status') {
            currentTick = m.tick;
            isPlaying = m.playing;
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
            updatePlayButton();
          } else if (m.type === 'loaded') {
            isPlaying = m.ok;
            isPaused = false;
            updatePlayButton();
          } else if (m.type === 'error') {
            setStatus(`Error: ${m.message}`);
            vscode.postMessage({ type: 'error', message: m.message });
            reject(new Error(m.message));
          }
        };
        // Send a copy so wasmBytes stays reusable if needed
        workletNode.port.postMessage({ type: 'init', wasmBytes: wasmBytes.slice(0) });
      });
    }

    if (audioCtx.state === 'suspended') {
      await audioCtx.resume();
    }
  }

  async function loadAndPlay(name, dataArray) {
    lastTrack = { name, data: dataArray };
    songTitle.textContent = name;
    sizeLabel.textContent = `${dataArray.length} B`;
    try {
      await ensureAudio();
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
    await ensureAudio();
    if (!isPlaying) {
      loadAndPlay(lastTrack.name, lastTrack.data);
    } else if (isPaused) {
      isPaused = false;
      workletNode.port.postMessage({ type: 'resume' });
      updatePlayButton();
      setStatus(`Playing ${lastTrack.name}`);
    } else {
      isPaused = true;
      workletNode.port.postMessage({ type: 'pause' });
      updatePlayButton();
      setStatus('Paused');
    }
  });

  btnStop.addEventListener('click', () => {
    if (workletReady && workletNode) {
      workletNode.port.postMessage({ type: 'stop' });
    }
    isPlaying = false;
    isPaused = false;
    meterBar.style.width = '0%';
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
      loadAndPlay(msg.name, msg.data);
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
