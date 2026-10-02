// Minimal Socket.IO-compatible shim over a native WebSocket, for the C++ backend.
// The frontend only *receives* over the socket (control is REST), so this provides
// io().on(event, cb); .emit() is a no-op. Text messages are {event,data} JSON;
// binary messages are packed 'spectrum' frames decoded here into the same object
// shape the old JSON 'spectrum' event delivered (power[], freqs[], center, rate,
// fft_size, carrier_dbfs, carrier_freq) — but freqs are reconstructed on the client
// instead of being sent, halving the payload.

(function () {
  const PWR_OFFSET = 215;   // byte b -> dB = b - 215 (spans -215..+40, 1 dB steps)

  function decodeSpectrum(buf) {
    const dv = new DataView(buf);
    // magic 'SPC2' at 0..3 => uint8 power payload. Reject stale/other formats.
    if (dv.getUint8(0) !== 0x53 || dv.getUint8(1) !== 0x50 ||
        dv.getUint8(2) !== 0x43 || dv.getUint8(3) !== 0x32) {
      console.warn('spectrum frame: unexpected magic (stale ws-shim.js? hard-reload)');
      return null;
    }
    const mode      = dv.getUint8(4);
    const fftSize   = dv.getUint32(8, true);
    const nBins     = dv.getUint32(12, true);
    const nSteps    = dv.getUint32(16, true);
    const cropLo    = dv.getUint32(20, true);
    const cropKeep  = dv.getUint32(24, true);
    const center    = dv.getFloat64(28, true);
    const rate      = dv.getFloat64(36, true);
    let carrierFreq = dv.getFloat64(44, true);
    let carrierDbfs = dv.getFloat32(52, true);

    let off = 60;
    const stepCenters = new Float64Array(nSteps);
    for (let i = 0; i < nSteps; i++) { stepCenters[i] = dv.getFloat64(off, true); off += 8; }
    // Power: 1 byte/bin, dequantize to dB. (No JSON parse, no Float32 wire cost.)
    const raw = new Uint8Array(buf, off, nBins);
    const power = new Float32Array(nBins);
    for (let i = 0; i < nBins; i++) power[i] = raw[i] - PWR_OFFSET;

    // Reconstruct the frequency axis (never sent over the wire).
    const freqs = new Float64Array(nBins);
    const half  = fftSize / 2, binHz = rate / fftSize;
    if (mode === 0) {                       // FFT: one contiguous span
      for (let i = 0; i < nBins; i++) freqs[i] = center + (i - half) * binHz;
    } else {                                // Scan: per-tile cropped centre bins
      for (let s = 0; s < nSteps; s++) {
        const fc = stepCenters[s];
        for (let j = 0; j < cropKeep; j++)
          freqs[s * cropKeep + j] = fc + (cropLo + j - half) * binHz;
      }
    }
    return {
      mode: mode === 0 ? 'fft' : 'scan',
      freqs, power,
      center, rate, fft_size: fftSize,
      carrier_dbfs: Number.isNaN(carrierDbfs) ? null : carrierDbfs,
      carrier_freq: Number.isNaN(carrierFreq) ? null : carrierFreq,
    };
  }

  window.io = function () {
    const listeners = {};
    const fire = (ev, data) => (listeners[ev] || []).forEach(cb => cb(data));
    let ws = null, closedByUser = false;
    let acceptBinary = () => true;   // gate: when false, spectrum frames are dropped un-decoded

    function connect() {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      ws = new WebSocket(proto + '://' + location.host + '/ws');
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => fire('connect');
      ws.onclose = () => { fire('disconnect'); if (!closedByUser) setTimeout(connect, 1000); };
      ws.onerror = () => {};
      ws.onmessage = (m) => {
        if (typeof m.data === 'string') {
          let o; try { o = JSON.parse(m.data); } catch (e) { return; }
          fire(o.event, o.data);
        } else {
          // Drop spectrum frames we don't need (e.g. this client hasn't attached) WITHOUT
          // decoding — cheapest possible path for an idle/viewer tab.
          if (!acceptBinary()) return;
          const spec = decodeSpectrum(m.data);
          if (spec) fire('spectrum', spec);
        }
      };
    }
    connect();

    return {
      on: (ev, cb) => { (listeners[ev] = listeners[ev] || []).push(cb); },
      emit: () => {},   // frontend never emits over the socket (control is REST)
      close: () => { closedByUser = true; if (ws) ws.close(); },
      // Supply a predicate; while it returns false, incoming spectrum frames are dropped
      // before decoding (used to ignore the stream until the client has attached).
      setAcceptBinary: (fn) => { acceptBinary = fn || (() => true); },
    };
  };
})();
