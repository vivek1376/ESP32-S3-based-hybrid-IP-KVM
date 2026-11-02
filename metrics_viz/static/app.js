/**
 * Vanilla JS Telemetry Visualizer for ESP32-S3 IP-KVM Radial Overlay
 * Polls Flask /api/metrics every 2 seconds using incremental byte offsets.
 */

(function () {
  "use strict";

  const POLL_INTERVAL_MS = 2000;
  const SLOW_FRAME_MS = 20.0;
  const ZONE_IN_PX = 176.90;
  const ZONE_OUT_PX = 189.54;
  const ZONE_REEL_PX = 235.03;
  const FLAG_SINGULARITY = 1 << 2;

  const GRID_COLOR = "#114554";
  const LABEL_COLOR = "#8ec5d1";

  const PHASE_NAMES = [
    "OVERLAY_DORMANT",
    "OVERLAY_REELING_IN",
    "OVERLAY_ENGAGEMENT",
    "OVERLAY_POPOUT",
  ];

  // Columnar in-memory telemetry store
  const store = {
    t: [],
    raw_dt_ms: [],
    fps3: [],
    phase: [],
    flags: [],
    d: [],
    v_theta: [],
    v_r: [],
    omega_ptr: [],
    psi: [],
    ccw_drive: [],
    spring_open: [],
    du_dt: [],
    u: [],
    omega_vis: [],
    r_curr: [],
    sorted_dt: new Float32Array(0),
    slow_count: 0,
    singularity_count: 0,
  };

  let sessionId = null;
  let nextOffset = 0;
  let isInitialFetch = true;
  let isPaused = false;
  let windowSec = 30;
  let hoverTimeSec = null;  // Synchronized hover time across time-series charts
  let freqHoverMs = null;   // Hover frame-time on Frequency Distribution chart
  let lastFreqBins = null;  // Cached histogram bins for hover lookup

  // DOM Elements
  const elBadge = document.getElementById("status-badge");
  const elSession = document.getElementById("meta-session");
  const elFrames = document.getElementById("meta-frames");
  const elOffset = document.getElementById("meta-offset");
  const elWindowSelect = document.getElementById("window-select");
  const elBtnPause = document.getElementById("btn-pause");
  const elBtnClear = document.getElementById("btn-clear");
  const elTooltip = document.getElementById("tooltip");

  const elKpiFps = document.getElementById("kpi-fps");
  const elKpiFpsSub = document.getElementById("kpi-fps-sub");
  const elKpiDtTail = document.getElementById("kpi-dt-tail");
  const elKpiSlowSub = document.getElementById("kpi-slow-sub");
  const elKpiUPsi = document.getElementById("kpi-u-psi");
  const elKpiOmegaSub = document.getElementById("kpi-omega-sub");
  const elFreqStats = document.getElementById("freq-stats-line");

  const canvasFps = document.getElementById("canvas-fps");
  const canvasFreq = document.getElementById("canvas-freq");
  const canvasOmegaU = document.getElementById("canvas-omega-u");
  const canvasControl = document.getElementById("canvas-control");

  function resetStore() {
    store.t.length = 0;
    store.raw_dt_ms.length = 0;
    store.fps3.length = 0;
    store.phase.length = 0;
    store.flags.length = 0;
    store.d.length = 0;
    store.v_theta.length = 0;
    store.v_r.length = 0;
    store.omega_ptr.length = 0;
    store.psi.length = 0;
    store.ccw_drive.length = 0;
    store.spring_open.length = 0;
    store.du_dt.length = 0;
    store.u.length = 0;
    store.omega_vis.length = 0;
    store.r_curr.length = 0;
    store.sorted_dt = new Float32Array(0);
    store.slow_count = 0;
    store.singularity_count = 0;
    lastFreqBins = null;
  }

  function formatBytes(bytes) {
    if (bytes < 1024) return bytes + " B";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
    return (bytes / (1024 * 1024)).toFixed(2) + " MB";
  }

  function setStatus(mode, text) {
    elBadge.className = "badge badge-" + mode;
    elBadge.textContent = text;
  }

  function appendData(d) {
    const n = d.t ? d.t.length : 0;
    if (n === 0) return;

    for (let i = 0; i < n; i++) {
      const dt = d.raw_dt_ms[i];
      const baseLen = store.raw_dt_ms.length;
      store.t.push(d.t[i]);
      store.raw_dt_ms.push(dt);

      // 3-frame rolling window FPS
      let meanDt = dt;
      if (baseLen >= 2) {
        meanDt = (store.raw_dt_ms[baseLen - 2] + store.raw_dt_ms[baseLen - 1] + dt) / 3.0;
      } else if (baseLen === 1) {
        meanDt = (store.raw_dt_ms[0] + dt) / 2.0;
      }
      const fps = meanDt > 0.05 ? 1000.0 / meanDt : 0.0;
      store.fps3.push(fps);

      if (baseLen > 0 && dt > SLOW_FRAME_MS) {
        store.slow_count++;
      }

      const flg = d.flags[i];
      if ((flg & FLAG_SINGULARITY) !== 0) {
        store.singularity_count++;
      }

      store.phase.push(d.phase[i]);
      store.flags.push(flg);
      store.d.push(d.d[i]);
      store.v_theta.push(d.v_theta[i]);
      store.v_r.push(d.v_r[i]);
      store.omega_ptr.push(d.omega_ptr[i]);
      store.psi.push(d.psi[i]);
      store.ccw_drive.push(d.ccw_drive[i]);
      store.spring_open.push(d.spring_open[i]);
      store.du_dt.push(d.ccw_drive[i] - d.spring_open[i]);
      store.u.push(d.u[i]);
      store.omega_vis.push(d.omega_vis[i]);
      store.r_curr.push(d.r_curr[i]);
    }

    // Rebuild sorted Float32Array for percentiles & frequency binning (skipping frame 0 init tick)
    const total = store.raw_dt_ms.length;
    if (total > 1) {
      const arr = new Float32Array(total - 1);
      for (let i = 1; i < total; i++) {
        arr[i - 1] = store.raw_dt_ms[i];
      }
      arr.sort();
      store.sorted_dt = arr;
    } else if (total === 1) {
      store.sorted_dt = new Float32Array([store.raw_dt_ms[0]]);
    }
  }

  function percentileSorted(sortedArr, p) {
    if (!sortedArr || sortedArr.length === 0) return 0;
    const idx = Math.min(sortedArr.length - 1, Math.max(0, Math.floor((p / 100.0) * (sortedArr.length - 1))));
    return sortedArr[idx];
  }

  function lowerBound(arr, target) {
    let lo = 0;
    let hi = arr.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (arr[mid] < target) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }

  function nearestIndex(arr, target, startIdx, endIdx) {
    if (arr.length === 0 || endIdx <= startIdx) return -1;
    let idx = lowerBound(arr, target);
    if (idx < startIdx) return startIdx;
    if (idx >= endIdx) return endIdx - 1;
    if (idx > startIdx && Math.abs(arr[idx - 1] - target) < Math.abs(arr[idx] - target)) {
      return idx - 1;
    }
    return idx;
  }

  async function pollMetrics() {
    if (isPaused) return;
    try {
      const params = new URLSearchParams();
      if (isInitialFetch) {
        params.set("init", "1");
      } else {
        params.set("offset", String(nextOffset));
        if (sessionId !== null) params.set("session_id", String(sessionId));
      }

      const resp = await fetch("/api/metrics?" + params.toString(), { cache: "no-store" });
      if (!resp.ok) {
        setStatus("error", "HTTP " + resp.status);
        return;
      }
      const json = await resp.json();

      if (json.status === "waiting") {
        setStatus("waiting", "WAITING FOR overlay_metrics.bin");
        return;
      }
      if (json.status === "error") {
        setStatus("error", "READ ERROR");
        return;
      }

      if (json.reset && !isInitialFetch) {
        resetStore();
      }
      isInitialFetch = false;
      sessionId = json.session_id;
      nextOffset = json.next_offset;

      if (json.count > 0 && json.data) {
        appendData(json.data);
      }

      setStatus("live", "LIVE \u2022 POLLING 2s");
      updateHeaderAndKpis();
      renderAllCharts();
    } catch (_err) {
      setStatus("error", "BACKEND OFFLINE");
    }
  }

  function updateHeaderAndKpis() {
    const n = store.t.length;
    elSession.textContent = sessionId !== null
      ? "Session: 0x" + (sessionId >>> 0).toString(16).toUpperCase().padStart(8, "0")
      : "Session: \u2014";
    elFrames.textContent = "Frames: " + n.toLocaleString();
    elOffset.textContent = "Read Offset: " + formatBytes(nextOffset);

    if (n === 0) return;

    const last = n - 1;
    const curFps = store.fps3[last];

    const totalTime = store.t[last] - store.t[0];
    const meanFps = totalTime > 0.01 ? (n - 1) / totalTime : curFps;
    const maxDt = store.sorted_dt.length > 0 ? store.sorted_dt[store.sorted_dt.length - 1] : store.raw_dt_ms[last];
    const minFps = maxDt > 0.05 ? 1000.0 / maxDt : 0;

    elKpiFps.textContent = curFps.toFixed(1) + " Hz";
    elKpiFpsSub.textContent = "Mean: " + meanFps.toFixed(1) + " Hz | Min: " + minFps.toFixed(1) + " Hz";

    const p50 = percentileSorted(store.sorted_dt, 50);
    const p95 = percentileSorted(store.sorted_dt, 95);
    const p99 = percentileSorted(store.sorted_dt, 99);
    const slowPct = n > 1 ? (100.0 * store.slow_count / (n - 1)) : 0.0;

    elKpiDtTail.textContent = p99.toFixed(2) + " / " + maxDt.toFixed(2) + " ms";
    elKpiSlowSub.textContent = "Slow (>20 ms): " + store.slow_count + " (" + slowPct.toFixed(2) + "%)";
    elFreqStats.textContent =
      "P50: " + p50.toFixed(2) + " ms | P95: " + p95.toFixed(2) +
      " ms | P99: " + p99.toFixed(2) + " ms | Max: " + maxDt.toFixed(2) + " ms";

    elKpiUPsi.textContent =
      "u = " + store.u[last].toFixed(3) + "  |  \u03c8 = " + store.psi[last].toFixed(3);
    elKpiOmegaSub.textContent =
      "\u03c9_vis: " + store.omega_vis[last].toFixed(2) +
      " | \u03c9_ptr: " + (store.omega_ptr[last] >= 0 ? "+" : "") + store.omega_ptr[last].toFixed(2) +
      " rad/s | Singularities: " + store.singularity_count;
  }

  // Prepare HiDPI canvas context and return CSS pixel dimensions
  function prepCanvas(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const w = Math.max(10, Math.floor(rect.width));
    const h = Math.max(10, Math.floor(rect.height));
    if (canvas.width !== Math.floor(w * dpr) || canvas.height !== Math.floor(h * dpr)) {
      canvas.width = Math.floor(w * dpr);
      canvas.height = Math.floor(h * dpr);
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    return { ctx, w, h };
  }

  function getVisibleTimeRange() {
    const n = store.t.length;
    if (n === 0) return { startIdx: 0, endIdx: 0, tMin: 0, tMax: 10 };
    const tLatest = store.t[n - 1];
    if (windowSec <= 0) {
      return { startIdx: 0, endIdx: n, tMin: store.t[0], tMax: Math.max(store.t[0] + 1.0, tLatest) };
    }
    const tMin = Math.max(store.t[0], tLatest - windowSec);
    const startIdx = Math.max(0, lowerBound(store.t, tMin) - 1);
    return { startIdx, endIdx: n, tMin, tMax: Math.max(tMin + 1.0, tLatest) };
  }

  function drawEmptyState(ctx, w, h, msg) {
    ctx.fillStyle = LABEL_COLOR;
    ctx.font = "13px ui-sans-serif, system-ui, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(msg, w / 2, h / 2);
  }

  function drawTimeGrid(ctx, pad, plotW, plotH, tMin, tMax) {
    const ticks = 6;
    ctx.strokeStyle = GRID_COLOR;
    ctx.lineWidth = 1;
    ctx.fillStyle = LABEL_COLOR;
    ctx.font = "11px ui-sans-serif, system-ui, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "top";

    for (let i = 0; i <= ticks; i++) {
      const frac = i / ticks;
      const x = pad.left + frac * plotW;
      const tVal = tMin + frac * (tMax - tMin);
      ctx.beginPath();
      ctx.moveTo(x, pad.top);
      ctx.lineTo(x, pad.top + plotH);
      ctx.stroke();
      ctx.fillText(tVal.toFixed(1) + "s", x, pad.top + plotH + 6);
    }
  }

  function drawPhaseShadingAndSingularities(ctx, pad, plotW, plotH, startIdx, endIdx, tMin, tMax) {
    const span = tMax - tMin;
    if (span <= 0 || endIdx <= startIdx) return;

    const toX = (t) => pad.left + ((t - tMin) / span) * plotW;

    let runStart = startIdx;
    let curPhase = store.phase[startIdx];
    for (let i = startIdx + 1; i <= endIdx; i++) {
      const ph = i < endIdx ? store.phase[i] : -1;
      if (ph !== curPhase) {
        if (curPhase === 1 || curPhase === 2 || curPhase === 3) {
          const x0 = Math.max(pad.left, toX(store.t[runStart]));
          const x1 = Math.min(pad.left + plotW, toX(store.t[Math.min(endIdx - 1, i)]));
          if (x1 > x0) {
            if (curPhase === 1) ctx.fillStyle = "rgba(103, 232, 249, 0.08)";
            else if (curPhase === 2) ctx.fillStyle = "rgba(244, 114, 182, 0.12)";
            else ctx.fillStyle = "rgba(251, 191, 36, 0.14)";
            ctx.fillRect(x0, pad.top, x1 - x0, plotH);
          }
        }
        runStart = i;
        curPhase = ph;
      }
    }

    ctx.save();
    ctx.strokeStyle = "rgba(253, 224, 71, 0.9)";
    ctx.lineWidth = 1.3;
    ctx.setLineDash([4, 3]);
    for (let i = startIdx; i < endIdx; i++) {
      if ((store.flags[i] & FLAG_SINGULARITY) !== 0) {
        const x = toX(store.t[i]);
        if (x >= pad.left && x <= pad.left + plotW) {
          ctx.beginPath();
          ctx.moveTo(x, pad.top);
          ctx.lineTo(x, pad.top + plotH);
          ctx.stroke();
        }
      }
    }
    ctx.restore();
  }

  // Min-max bucketed polyline renderer so spikes never disappear when zoomed out
  function drawSeries(ctx, tArr, yArr, startIdx, endIdx, toX, toY, color, lineWidth, dashed) {
    const count = endIdx - startIdx;
    if (count <= 0) return;

    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = lineWidth || 1.6;
    if (dashed) ctx.setLineDash(dashed);
    ctx.beginPath();

    const maxBuckets = 1200;
    if (count <= maxBuckets) {
      for (let i = startIdx; i < endIdx; i++) {
        const x = toX(tArr[i]);
        const y = toY(yArr[i]);
        if (i === startIdx) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
    } else {
      const step = count / maxBuckets;
      let started = false;
      for (let b = 0; b < maxBuckets; b++) {
        const i0 = startIdx + Math.floor(b * step);
        const i1 = Math.min(endIdx, startIdx + Math.floor((b + 1) * step));
        if (i1 <= i0) continue;
        let minIdx = i0;
        let maxIdx = i0;
        for (let k = i0 + 1; k < i1; k++) {
          if (yArr[k] < yArr[minIdx]) minIdx = k;
          if (yArr[k] > yArr[maxIdx]) maxIdx = k;
        }
        const firstIdx = Math.min(minIdx, maxIdx);
        const secondIdx = Math.max(minIdx, maxIdx);
        const x1 = toX(tArr[firstIdx]);
        const y1 = toY(yArr[firstIdx]);
        if (!started) {
          ctx.moveTo(x1, y1);
          started = true;
        } else {
          ctx.lineTo(x1, y1);
        }
        if (secondIdx !== firstIdx) {
          ctx.lineTo(toX(tArr[secondIdx]), toY(yArr[secondIdx]));
        }
      }
    }

    ctx.stroke();
    ctx.restore();
  }

  function drawHoverCrosshair(ctx, pad, plotW, plotH, tMin, tMax) {
    if (hoverTimeSec === null || hoverTimeSec < tMin || hoverTimeSec > tMax) return;
    const x = pad.left + ((hoverTimeSec - tMin) / (tMax - tMin)) * plotW;
    ctx.save();
    ctx.strokeStyle = "rgba(230, 247, 250, 0.6)";
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(x, pad.top);
    ctx.lineTo(x, pad.top + plotH);
    ctx.stroke();
    ctx.restore();
  }

  // 1. Running Frame Rate (3-Frame Rolling Window)
  function renderFpsChart() {
    const { ctx, w, h } = prepCanvas(canvasFps);
    if (store.t.length < 2) {
      drawEmptyState(ctx, w, h, "Waiting for frame telemetry from kvm_client...");
      return;
    }

    const pad = { top: 12, right: 18, bottom: 26, left: 48 };
    const plotW = w - pad.left - pad.right;
    const plotH = h - pad.top - pad.bottom;
    const { startIdx, endIdx, tMin, tMax } = getVisibleTimeRange();

    let maxFps = 70;
    for (let i = Math.max(1, startIdx); i < endIdx; i++) {
      if (store.fps3[i] > maxFps && store.fps3[i] < 1000) maxFps = store.fps3[i];
    }
    const yMax = Math.ceil((maxFps * 1.1) / 10) * 10;
    const yMin = 0;

    drawTimeGrid(ctx, pad, plotW, plotH, tMin, tMax);

    const yTicks = 4;
    ctx.font = "11px ui-sans-serif, system-ui, sans-serif";
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    for (let i = 0; i <= yTicks; i++) {
      const val = yMin + (i / yTicks) * (yMax - yMin);
      const y = pad.top + plotH - (i / yTicks) * plotH;
      ctx.strokeStyle = GRID_COLOR;
      ctx.beginPath();
      ctx.moveTo(pad.left, y);
      ctx.lineTo(pad.left + plotW, y);
      ctx.stroke();
      ctx.fillStyle = LABEL_COLOR;
      ctx.fillText(val.toFixed(0), pad.left - 6, y);
    }

    const toX = (t) => pad.left + ((t - tMin) / (tMax - tMin)) * plotW;
    const toY = (v) => pad.top + plotH - ((Math.min(yMax, Math.max(yMin, v)) - yMin) / (yMax - yMin)) * plotH;

    [60, 30].forEach((refFps) => {
      if (refFps <= yMax) {
        const y = toY(refFps);
        ctx.save();
        ctx.strokeStyle = refFps === 60 ? "rgba(74, 222, 128, 0.35)" : "rgba(248, 113, 113, 0.35)";
        ctx.setLineDash([4, 4]);
        ctx.beginPath();
        ctx.moveTo(pad.left, y);
        ctx.lineTo(pad.left + plotW, y);
        ctx.stroke();
        ctx.restore();
      }
    });

    ctx.save();
    ctx.beginPath();
    ctx.rect(pad.left, pad.top, plotW, plotH);
    ctx.clip();

    drawSeries(ctx, store.t, store.fps3, Math.max(1, startIdx), endIdx, toX, toY, "#67e8f9", 1.7);

    // Highlight slow frames (> 20 ms)
    ctx.fillStyle = "#f87171";
    for (let i = Math.max(1, startIdx); i < endIdx; i++) {
      if (store.raw_dt_ms[i] > SLOW_FRAME_MS) {
        const x = toX(store.t[i]);
        const y = toY(store.fps3[i]);
        ctx.beginPath();
        ctx.arc(x, y, 3.0, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    drawHoverCrosshair(ctx, pad, plotW, plotH, tMin, tMax);
    ctx.restore();
  }

  // 2. Regular Frequency Distribution Curve of Frame Time (raw_dt_ms)
  function buildFreqBins(sorted) {
    const minObserved = sorted[0];
    const maxObserved = sorted[sorted.length - 1];
    const xMin = Math.max(0.0, Math.min(10.0, Math.floor(minObserved * 0.8)));
    const xMax = Math.max(26.0, Math.min(120.0, Math.ceil(maxObserved * 1.08)));
    const span = xMax - xMin;

    let binWidth = 0.25;
    if (span > 60) binWidth = 1.0;
    else if (span > 30) binWidth = 0.5;

    const numBins = Math.max(12, Math.ceil(span / binWidth));
    const actualXMax = xMin + numBins * binWidth;
    const counts = new Uint32Array(numBins);
    const total = sorted.length;

    for (let i = 0; i < total; i++) {
      const v = sorted[i];
      let b = Math.floor((v - xMin) / binWidth);
      if (b < 0) b = 0;
      if (b >= numBins) b = numBins - 1;
      counts[b]++;
    }

    const pcts = new Float32Array(numBins);
    let maxPct = 5.0;
    for (let b = 0; b < numBins; b++) {
      const p = (100.0 * counts[b]) / total;
      pcts[b] = p;
      if (p > maxPct) maxPct = p;
    }

    return {
      xMin,
      xMax: actualXMax,
      binWidth,
      numBins,
      counts,
      pcts,
      maxPct,
      total,
    };
  }

  function renderFreqDistChart() {
    const { ctx, w, h } = prepCanvas(canvasFreq);
    const sorted = store.sorted_dt;
    if (!sorted || sorted.length < 2) {
      drawEmptyState(ctx, w, h, "Waiting for frame time distribution...");
      return;
    }

    const bins = buildFreqBins(sorted);
    lastFreqBins = bins;

    const pad = { top: 12, right: 20, bottom: 26, left: 48 };
    const plotW = w - pad.left - pad.right;
    const plotH = h - pad.top - pad.bottom;

    const yMax = Math.min(100.0, Math.ceil((bins.maxPct * 1.12) / 5) * 5);
    const toX = (ms) => pad.left + ((Math.min(bins.xMax, Math.max(bins.xMin, ms)) - bins.xMin) / (bins.xMax - bins.xMin)) * plotW;
    const toY = (pct) => pad.top + plotH - (Math.min(yMax, Math.max(0, pct)) / yMax) * plotH;

    // X & Y grids
    ctx.font = "11px ui-sans-serif, system-ui, sans-serif";
    const xTicks = 6;
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (let i = 0; i <= xTicks; i++) {
      const val = bins.xMin + (i / xTicks) * (bins.xMax - bins.xMin);
      const x = pad.left + (i / xTicks) * plotW;
      ctx.strokeStyle = GRID_COLOR;
      ctx.beginPath();
      ctx.moveTo(x, pad.top);
      ctx.lineTo(x, pad.top + plotH);
      ctx.stroke();
      ctx.fillStyle = LABEL_COLOR;
      ctx.fillText(val.toFixed(1) + "ms", x, pad.top + plotH + 6);
    }

    const yTicks = 4;
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    for (let i = 0; i <= yTicks; i++) {
      const pct = (i / yTicks) * yMax;
      const y = toY(pct);
      ctx.strokeStyle = GRID_COLOR;
      ctx.beginPath();
      ctx.moveTo(pad.left, y);
      ctx.lineTo(pad.left + plotW, y);
      ctx.stroke();
      ctx.fillStyle = LABEL_COLOR;
      ctx.fillText(pct.toFixed(pct < 10 ? 1 : 0) + "%", pad.left - 6, y);
    }

    // Shaded Slow-Frame Tail Zone (> 20.0 ms)
    if (bins.xMax > SLOW_FRAME_MS) {
      const xSlow = toX(SLOW_FRAME_MS);
      ctx.fillStyle = "rgba(239, 68, 68, 0.09)";
      ctx.fillRect(xSlow, pad.top, pad.left + plotW - xSlow, plotH);

      ctx.save();
      ctx.strokeStyle = "rgba(248, 113, 113, 0.7)";
      ctx.setLineDash([4, 3]);
      ctx.beginPath();
      ctx.moveTo(xSlow, pad.top);
      ctx.lineTo(xSlow, pad.top + plotH);
      ctx.stroke();
      ctx.restore();
    }

    // 16.67 ms (60 Hz) vertical reference
    if (16.67 >= bins.xMin && 16.67 <= bins.xMax) {
      const x60 = toX(16.67);
      ctx.save();
      ctx.strokeStyle = "rgba(251, 191, 36, 0.65)";
      ctx.setLineDash([4, 3]);
      ctx.beginPath();
      ctx.moveTo(x60, pad.top);
      ctx.lineTo(x60, pad.top + plotH);
      ctx.stroke();
      ctx.restore();
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(pad.left, pad.top, plotW, plotH);
    ctx.clip();

    const baseY = toY(0);

    // Translucent area fill under the frequency distribution curve
    ctx.beginPath();
    ctx.moveTo(toX(bins.xMin), baseY);
    for (let b = 0; b < bins.numBins; b++) {
      const centerMs = bins.xMin + (b + 0.5) * bins.binWidth;
      ctx.lineTo(toX(centerMs), toY(bins.pcts[b]));
    }
    ctx.lineTo(toX(bins.xMax), baseY);
    ctx.closePath();
    ctx.fillStyle = "rgba(94, 234, 212, 0.18)";
    ctx.fill();

    // Frequency distribution curve stroke
    ctx.beginPath();
    for (let b = 0; b < bins.numBins; b++) {
      const centerMs = bins.xMin + (b + 0.5) * bins.binWidth;
      const x = toX(centerMs);
      const y = toY(bins.pcts[b]);
      if (b === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = "#5eead4";
    ctx.lineWidth = 2.0;
    ctx.stroke();

    // Highlight any non-zero slow-frame bins (> 20.0 ms) so sparse outliers stand out clearly
    ctx.fillStyle = "#f87171";
    for (let b = 0; b < bins.numBins; b++) {
      if (bins.counts[b] > 0) {
        const centerMs = bins.xMin + (b + 0.5) * bins.binWidth;
        if (centerMs > SLOW_FRAME_MS) {
          const x = toX(centerMs);
          const y = toY(bins.pcts[b]);
          ctx.beginPath();
          ctx.arc(x, y, 3.2, 0, Math.PI * 2);
          ctx.fill();
        }
      }
    }

    // Hover crosshair on frequency chart
    if (freqHoverMs !== null && freqHoverMs >= bins.xMin && freqHoverMs <= bins.xMax) {
      const xH = toX(freqHoverMs);
      ctx.strokeStyle = "rgba(230, 247, 250, 0.65)";
      ctx.lineWidth = 1;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(xH, pad.top);
      ctx.lineTo(xH, pad.top + plotH);
      ctx.stroke();
    }
    ctx.restore();
  }

  // 3. Dual-Axis Line Graph: ov->u, psi [0..1] + ov->omega_vis, omega_ptr (rad/s)
  function renderOmegaUChart() {
    const { ctx, w, h } = prepCanvas(canvasOmegaU);
    if (store.t.length < 2) {
      drawEmptyState(ctx, w, h, "Waiting for kinematic telemetry...");
      return;
    }

    const pad = { top: 12, right: 54, bottom: 26, left: 48 };
    const plotW = w - pad.left - pad.right;
    const plotH = h - pad.top - pad.bottom;
    const { startIdx, endIdx, tMin, tMax } = getVisibleTimeRange();

    drawTimeGrid(ctx, pad, plotW, plotH, tMin, tMax);
    drawPhaseShadingAndSingularities(ctx, pad, plotW, plotH, startIdx, endIdx, tMin, tMax);

    const uMin = 0.0;
    const uMax = 1.05;
    const toX = (t) => pad.left + ((t - tMin) / (tMax - tMin)) * plotW;
    const toYu = (u) => pad.top + plotH - ((Math.min(uMax, Math.max(uMin, u)) - uMin) / (uMax - uMin)) * plotH;

    let wMin = -2.0;
    let wMax = 12.0;
    for (let i = startIdx; i < endIdx; i++) {
      const wp = store.omega_ptr[i];
      if (wp > wMax && wp < 80) wMax = wp;
      if (wp < wMin && wp > -80) wMin = wp;
    }
    wMax = Math.ceil(wMax * 1.1);
    wMin = Math.floor(wMin * 1.1);
    const toYw = (om) => pad.top + plotH - ((Math.min(wMax, Math.max(wMin, om)) - wMin) / (wMax - wMin)) * plotH;

    ctx.font = "11px ui-sans-serif, system-ui, sans-serif";
    ctx.textBaseline = "middle";
    const yTicks = 4;
    for (let i = 0; i <= yTicks; i++) {
      const frac = i / yTicks;
      const y = pad.top + plotH - frac * plotH;
      ctx.strokeStyle = GRID_COLOR;
      ctx.beginPath();
      ctx.moveTo(pad.left, y);
      ctx.lineTo(pad.left + plotW, y);
      ctx.stroke();

      ctx.fillStyle = "#f472b6";
      ctx.textAlign = "right";
      ctx.fillText((frac * 1.0).toFixed(2), pad.left - 6, y);

      const omVal = wMin + frac * (wMax - wMin);
      ctx.fillStyle = "#fbbf24";
      ctx.textAlign = "left";
      ctx.fillText(omVal.toFixed(1), pad.left + plotW + 6, y);
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(pad.left, pad.top, plotW, plotH);
    ctx.clip();

    if (wMin < 0 && wMax > 0) {
      const y0 = toYw(0);
      ctx.strokeStyle = "rgba(142, 197, 209, 0.28)";
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(pad.left, y0);
      ctx.lineTo(pad.left + plotW, y0);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    drawSeries(ctx, store.t, store.psi, startIdx, endIdx, toX, toYu, "rgba(45, 212, 191, 0.8)", 1.3, [4, 3]);
    drawSeries(ctx, store.t, store.omega_ptr, startIdx, endIdx, toX, toYw, "rgba(251, 191, 36, 0.88)", 1.4);
    drawSeries(ctx, store.t, store.omega_vis, startIdx, endIdx, toX, toYw, "#67e8f9", 1.8);
    drawSeries(ctx, store.t, store.u, startIdx, endIdx, toX, toYu, "#f472b6", 2.3);

    drawHoverCrosshair(ctx, pad, plotW, plotH, tMin, tMax);
    ctx.restore();
  }

  // 4. Control Law Rates (ccw_drive, -spring_open, du/dt) & Radial Distance d
  function renderControlChart() {
    const { ctx, w, h } = prepCanvas(canvasControl);
    if (store.t.length < 2) {
      drawEmptyState(ctx, w, h, "Waiting for control law diagnostics...");
      return;
    }

    const pad = { top: 12, right: 54, bottom: 26, left: 48 };
    const plotW = w - pad.left - pad.right;
    const plotH = h - pad.top - pad.bottom;
    const { startIdx, endIdx, tMin, tMax } = getVisibleTimeRange();

    drawTimeGrid(ctx, pad, plotW, plotH, tMin, tMax);
    drawPhaseShadingAndSingularities(ctx, pad, plotW, plotH, startIdx, endIdx, tMin, tMax);

    let rateMax = 2.5;
    let rateMin = -1.5;
    let dMax = 350.0;
    for (let i = startIdx; i < endIdx; i++) {
      if (store.ccw_drive[i] > rateMax && store.ccw_drive[i] < 30) rateMax = store.ccw_drive[i];
      if (-store.spring_open[i] < rateMin) rateMin = -store.spring_open[i];
      if (store.d[i] > dMax && store.d[i] < 2500) dMax = store.d[i];
    }
    rateMax = Math.ceil(rateMax * 1.15 * 2) / 2;
    rateMin = Math.floor(rateMin * 1.15 * 2) / 2;
    dMax = Math.min(2000, Math.ceil(dMax / 50) * 50);

    const toX = (t) => pad.left + ((t - tMin) / (tMax - tMin)) * plotW;
    const toYr = (r) => pad.top + plotH - ((Math.min(rateMax, Math.max(rateMin, r)) - rateMin) / (rateMax - rateMin)) * plotH;
    const toYd = (d) => pad.top + plotH - (Math.min(dMax, Math.max(0, d)) / dMax) * plotH;

    ctx.font = "11px ui-sans-serif, system-ui, sans-serif";
    ctx.textBaseline = "middle";
    const yTicks = 4;
    for (let i = 0; i <= yTicks; i++) {
      const frac = i / yTicks;
      const y = pad.top + plotH - frac * plotH;
      ctx.strokeStyle = GRID_COLOR;
      ctx.beginPath();
      ctx.moveTo(pad.left, y);
      ctx.lineTo(pad.left + plotW, y);
      ctx.stroke();

      const rVal = rateMin + frac * (rateMax - rateMin);
      ctx.fillStyle = "#4ade80";
      ctx.textAlign = "right";
      ctx.fillText(rVal.toFixed(1), pad.left - 6, y);

      const dVal = frac * dMax;
      ctx.fillStyle = "#a5b4fc";
      ctx.textAlign = "left";
      ctx.fillText(dVal.toFixed(0) + "px", pad.left + plotW + 6, y);
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(pad.left, pad.top, plotW, plotH);
    ctx.clip();

    if (rateMin < 0 && rateMax > 0) {
      const y0 = toYr(0);
      ctx.strokeStyle = "rgba(142, 197, 209, 0.32)";
      ctx.beginPath();
      ctx.moveTo(pad.left, y0);
      ctx.lineTo(pad.left + plotW, y0);
      ctx.stroke();
    }

    [
      { px: ZONE_IN_PX, col: "rgba(244, 114, 182, 0.5)" },
      { px: ZONE_OUT_PX, col: "rgba(251, 191, 36, 0.4)" },
      { px: ZONE_REEL_PX, col: "rgba(103, 232, 249, 0.42)" },
    ].forEach((z) => {
      const yz = toYd(z.px);
      ctx.save();
      ctx.strokeStyle = z.col;
      ctx.setLineDash([5, 4]);
      ctx.beginPath();
      ctx.moveTo(pad.left, yz);
      ctx.lineTo(pad.left + plotW, yz);
      ctx.stroke();
      ctx.restore();
    });

    const toYNegSpring = (s) => toYr(-s);

    drawSeries(ctx, store.t, store.d, startIdx, endIdx, toX, toYd, "rgba(165, 180, 252, 0.85)", 1.4);
    drawSeries(ctx, store.t, store.ccw_drive, startIdx, endIdx, toX, toYr, "#4ade80", 1.6);
    drawSeries(ctx, store.t, store.spring_open, startIdx, endIdx, toX, toYNegSpring, "#fb7185", 1.6);
    drawSeries(ctx, store.t, store.du_dt, startIdx, endIdx, toX, toYr, "#f8fafc", 1.8);

    drawHoverCrosshair(ctx, pad, plotW, plotH, tMin, tMax);
    ctx.restore();
  }

  function renderAllCharts() {
    renderFpsChart();
    renderFreqDistChart();
    renderOmegaUChart();
    renderControlChart();
  }

  // Interactive Hover Tooltips
  function attachTimeSeriesHover(canvas, rightPad) {
    canvas.addEventListener("mousemove", (ev) => {
      if (store.t.length < 2) return;
      const rect = canvas.getBoundingClientRect();
      const padLeft = 48;
      const plotW = rect.width - padLeft - rightPad;
      const relX = ev.clientX - rect.left - padLeft;
      if (relX < 0 || relX > plotW) {
        hoverTimeSec = null;
        elTooltip.classList.add("hidden");
        renderAllCharts();
        return;
      }

      const { startIdx, endIdx, tMin, tMax } = getVisibleTimeRange();
      hoverTimeSec = tMin + (relX / plotW) * (tMax - tMin);
      const idx = nearestIndex(store.t, hoverTimeSec, startIdx, endIdx);
      if (idx < 0) return;

      renderAllCharts();

      const phName = PHASE_NAMES[store.phase[idx]] || "UNKNOWN";
      elTooltip.innerHTML =
        '<div class="tt-title">t = ' + store.t[idx].toFixed(3) + " s (" + phName + ")</div>" +
        '<div class="tt-row"><span>3-Frame FPS / dt:</span><span>' +
        store.fps3[idx].toFixed(1) + " Hz (" + store.raw_dt_ms[idx].toFixed(2) + " ms)</span></div>" +
        '<div class="tt-row"><span>ov-&gt;u / R_curr:</span><span>' +
        store.u[idx].toFixed(3) + " (" + store.r_curr[idx].toFixed(1) + " px)</span></div>" +
        '<div class="tt-row"><span>&psi; (circularity):</span><span>' + store.psi[idx].toFixed(3) + "</span></div>" +
        '<div class="tt-row"><span>&omega;_vis / &omega;_ptr:</span><span>' +
        store.omega_vis[idx].toFixed(2) + " / " + store.omega_ptr[idx].toFixed(2) + " rad/s</span></div>" +
        '<div class="tt-row"><span>drive / -spring / du_dt:</span><span>' +
        store.ccw_drive[idx].toFixed(2) + " / -" + store.spring_open[idx].toFixed(2) +
        " / " + store.du_dt[idx].toFixed(2) + "</span></div>" +
        '<div class="tt-row"><span>Radius d:</span><span>' + store.d[idx].toFixed(1) + " px</span></div>";

      positionTooltip(ev.clientX, ev.clientY);
    });

    canvas.addEventListener("mouseleave", () => {
      hoverTimeSec = null;
      elTooltip.classList.add("hidden");
      renderAllCharts();
    });
  }

  canvasFreq.addEventListener("mousemove", (ev) => {
    if (!lastFreqBins) return;
    const bins = lastFreqBins;
    const rect = canvasFreq.getBoundingClientRect();
    const padLeft = 48;
    const padRight = 20;
    const plotW = rect.width - padLeft - padRight;
    const relX = ev.clientX - rect.left - padLeft;
    if (relX < 0 || relX > plotW) {
      freqHoverMs = null;
      elTooltip.classList.add("hidden");
      renderFreqDistChart();
      return;
    }

    freqHoverMs = bins.xMin + (relX / plotW) * (bins.xMax - bins.xMin);
    let b = Math.floor((freqHoverMs - bins.xMin) / bins.binWidth);
    if (b < 0) b = 0;
    if (b >= bins.numBins) b = bins.numBins - 1;

    const binLo = bins.xMin + b * bins.binWidth;
    const binHi = binLo + bins.binWidth;
    const cnt = bins.counts[b];
    const pct = bins.pcts[b];
    const slowPct = bins.total > 0 ? (100.0 * store.slow_count / bins.total) : 0.0;

    renderFreqDistChart();

    elTooltip.innerHTML =
      '<div class="tt-title">Frame Time Bin: [' + binLo.toFixed(2) + " .. " + binHi.toFixed(2) + " ms)</div>" +
      '<div class="tt-row"><span>Bin Frequency:</span><span>' +
      pct.toFixed(2) + "% (" + cnt + " frames)</span></div>" +
      '<div class="tt-row"><span>Slow Frames (&gt;20 ms):</span><span>' +
      store.slow_count + " (" + slowPct.toFixed(2) + "%)</span></div>";

    positionTooltip(ev.clientX, ev.clientY);
  });

  canvasFreq.addEventListener("mouseleave", () => {
    freqHoverMs = null;
    elTooltip.classList.add("hidden");
    renderFreqDistChart();
  });

  function positionTooltip(clientX, clientY) {
    elTooltip.classList.remove("hidden");
    const offset = 14;
    let left = clientX + offset;
    let top = clientY + offset;
    const ttRect = elTooltip.getBoundingClientRect();
    if (left + ttRect.width > window.innerWidth - 10) {
      left = clientX - ttRect.width - offset;
    }
    if (top + ttRect.height > window.innerHeight - 10) {
      top = clientY - ttRect.height - offset;
    }
    elTooltip.style.left = Math.max(8, left) + "px";
    elTooltip.style.top = Math.max(8, top) + "px";
  }

  // Controls & Resize
  elWindowSelect.addEventListener("change", () => {
    windowSec = Number(elWindowSelect.value);
    renderAllCharts();
  });

  elBtnPause.addEventListener("click", () => {
    isPaused = !isPaused;
    elBtnPause.textContent = isPaused ? "Resume" : "Pause";
    elBtnPause.classList.toggle("is-paused", isPaused);
    if (isPaused) {
      setStatus("paused", "PAUSED");
    } else {
      pollMetrics();
    }
  });

  elBtnClear.addEventListener("click", () => {
    resetStore();
    updateHeaderAndKpis();
    renderAllCharts();
  });

  window.addEventListener("resize", () => {
    renderAllCharts();
  });

  attachTimeSeriesHover(canvasFps, 18);
  attachTimeSeriesHover(canvasOmegaU, 54);
  attachTimeSeriesHover(canvasControl, 54);

  // Initial draw & start 2-second incremental polling loop
  renderAllCharts();
  pollMetrics();
  setInterval(pollMetrics, POLL_INTERVAL_MS);
})();
