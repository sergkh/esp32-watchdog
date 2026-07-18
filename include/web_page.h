#ifndef WEB_PAGE_H
#define WEB_PAGE_H

const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Watchdog Status Page</title>
<style>
:root {
  --bg: #f4f7fb;
  --card: #ffffff;
  --text: #1f2937;
  --muted: #6b7280;
  --line: #e5e7eb;
  --ok: #22c55e;
  --failed: #f59e0b;
  --wififail: #8b5cf6;
  --restart: #ef4444;
  --empty: #d1d5db;
}

* { box-sizing: border-box; }

body {
  margin: 0;
  background: linear-gradient(180deg, #eef3ff 0%, var(--bg) 100%);
  color: var(--text);
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif;
}

.app {
  max-width: 920px;
  margin: 28px auto;
  padding: 0 16px;
}

.card {
  background: var(--card);
  border: 1px solid var(--line);
  border-radius: 16px;
  box-shadow: 0 10px 26px rgba(2, 8, 23, 0.08);
  padding: 18px;
}

h2 { margin: 0 0 12px; }
h3 { margin: 18px 0 10px; }

table {
  width: 100%;
  border-collapse: collapse;
  border: 1px solid var(--line);
  border-radius: 10px;
  overflow: hidden;
}

td {
  padding: 10px 12px;
  border-bottom: 1px solid var(--line);
}

tr:last-child td { border-bottom: none; }
td:first-child { width: 180px; color: var(--muted); }

.status-chip {
  display: inline-block;
  padding: 4px 10px;
  border-radius: 999px;
  font-weight: 600;
  font-size: 13px;
}

.status-ok { background: rgba(34, 197, 94, 0.14); color: #15803d; }
.status-failed { background: rgba(245, 158, 11, 0.14); color: #b45309; }
.status-wait { background: rgba(59, 130, 246, 0.14); color: #1d4ed8; }

.legend { margin: 8px 0; display: flex; flex-wrap: wrap; gap: 12px; font-size: 13px; color: var(--muted); }
.dot { display: inline-block; width: 10px; height: 10px; border-radius: 2px; margin-right: 6px; vertical-align: middle; }

.timeline {
  display: flex;
  gap: 2px;
  margin: 8px 0 8px;
  padding: 8px;
  border: 1px solid var(--line);
  border-radius: 10px;
  background: #fafbff;
  flex-wrap: nowrap;
  overflow-x: auto;
}

.bar { width: 10px; height: 18px; border-radius: 2px; }
.ok { background: var(--ok); }
.failed { background: var(--failed); }
.wififail { background: var(--wififail); }
.restart { background: var(--restart); }
.empty { background: var(--empty); }

.timeline-scale {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
  color: var(--muted);
  margin-bottom: 14px;
}

.timeline-scale span {
  position: relative;
  padding-top: 8px;
}

.timeline-scale span::before {
  content: "";
  position: absolute;
  top: 0;
  left: 50%;
  transform: translateX(-50%);
  width: 1px;
  height: 6px;
  background: #9ca3af;
}

button {
  border: none;
  background: #2563eb;
  color: #fff;
  border-radius: 10px;
  padding: 10px 14px;
  font-weight: 600;
  cursor: pointer;
}

button:hover { background: #1d4ed8; }

.config-panel {
  margin-top: 18px;
  border-top: 1px solid var(--line);
  padding-top: 12px;
}

details {
  border: 1px solid var(--line);
  border-radius: 10px;
  background: #fafbff;
}

summary {
  cursor: pointer;
  list-style: none;
  padding: 10px 12px;
  font-weight: 600;
}

summary::-webkit-details-marker { display: none; }

.config-content {
  border-top: 1px solid var(--line);
  padding: 12px;
}

.config-row {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}

.config-field {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.config-toggle {
  display: flex;
  align-items: center;
  gap: 8px;
}

.config-row input {
  width: 120px;
  padding: 8px;
  border: 1px solid var(--line);
  border-radius: 8px;
}

.config-toggle input {
  width: auto;
}

.config-note {
  margin: 0 0 10px;
  color: var(--muted);
  font-size: 13px;
}

.config-status {
  margin-top: 10px;
  color: var(--muted);
  font-size: 13px;
}
</style>
</head>
<body>
<div class="app">
<div class="card">
<h2>Watchdog Status</h2>
<table border="1">
<tr><td>Status</td><td id="status"></td></tr>
<tr><td>Failed Pings</td><td id="failedPings"></td></tr>
<tr><td>Last Update</td><td id="lastUpdate"></td></tr>
<tr><td>Last Restart</td><td id="lastRestart"></td></tr>
<tr><td>Uptime</td><td id="uptime"></td></tr>
</table>
<h3>Status history (last 24h, 10-min buckets)</h3>
<div class="legend">
  <span><i class="dot ok"></i>OK ping</span>
  <span><i class="dot failed"></i>At least one failed ping</span>
  <span><i class="dot wififail"></i>WiFi reconnect failed</span>
  <span><i class="dot restart"></i>Restart</span>
  <span><i class="dot empty"></i>No data</span>
</div>
<div id="timeline" class="timeline"></div>
<div class="timeline-scale">
  <span>24h</span>
  <span>20h</span>
  <span>16h</span>
  <span>12h</span>
  <span>8h</span>
  <span>4h</span>
  <span>Now</span>
</div>
<br/><button onclick="forceReboot()">Force Reboot</button>

<div class="config-panel">
  <details>
    <summary>Config</summary>
    <div class="config-content">
      <p class="config-note">Update watchdog behavior using JSON config endpoint.</p>
      <div class="config-row">
        <div class="config-field">
          <label for="restartDelayMinutes">Restart delay (minutes)</label>
          <input id="restartDelayMinutes" type="number" min="0" step="1" value="3">
        </div>
        <div class="config-field">
          <label for="noSuccessPingTimeMinutes">No successful ping time (minutes)</label>
          <input id="noSuccessPingTimeMinutes" type="number" min="0" step="1" value="5">
        </div>
        <div class="config-field">
          <label for="minFailedPings">Min failed pings</label>
          <input id="minFailedPings" type="number" min="1" step="1" value="10">
        </div>
        <div class="config-toggle">
          <input id="autoRestartEnabled" type="checkbox" checked>
          <label for="autoRestartEnabled">Enable auto-restart</label>
        </div>
        <button onclick="saveConfig()">Save Config</button>
      </div>
      <div id="configStatus" class="config-status">Not loaded yet.</div>
    </div>
  </details>
</div>
</div>
</div>

<script>
let fetching = false;
let savingConfig = false;

function setConfigStatus(text) {
  document.getElementById("configStatus").innerText = text;
}

function formatDaysHoursFromMs(ms) {
  const totalSeconds = Math.floor(parseInt(ms) / 1000);
  const totalDays = Math.floor(totalSeconds / 86400);
  const months = Math.floor(totalDays / 30);
  const days = totalDays % 30;
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  return (months > 0 ? (months + "mo ") : "") + 
    (days > 0 ? (days + "d ") : "") + 
    (hours > 0 ? (hours + "h ") : "") + 
    (minutes > 0 ? (minutes + "m") : "") + 
    ((minutes + hours + days + months) == 0 && seconds > 0 ? (seconds + "s") : "");
}

function timelineClass(v) {
  if (v === 3) return "restart";
  if (v === 4) return "wififail";
  if (v === 2) return "failed";
  if (v === 1) return "ok";
  return "empty";
}

function timelineLabel(v) {
  if (v === 3) return "Restart";
  if (v === 4) return "WiFi reconnect failed";
  if (v === 2) return "Failed ping";
  if (v === 1) return "OK ping";
  return "No data";
}

function timelineLabelFromFlags(flags) {
  const labels = [];
  if ((flags & 4) !== 0) labels.push("Restart");
  if ((flags & 8) !== 0) labels.push("WiFi reconnect failed");
  if ((flags & 2) !== 0) labels.push("Failed ping");
  if ((flags & 1) !== 0) labels.push("OK ping");
  return labels.length > 0 ? labels.join(", ") : "No data";
}

function normalizeTimelineValue(flags) {
  if ((flags & 4) !== 0) return 3;      // restart
  if ((flags & 8) !== 0) return 4;      // wifi reconnect failed
  if ((flags & 2) !== 0) return 2;      // ping failed
  if ((flags & 1) !== 0) return 1;      // ping ok
  return 0;
}

function statusClass(status) {
  if (status === "OK") return "status-ok";
  if (status === "FAILED") return "status-failed";
  return "status-wait";
}

function formatAbsoluteTime(epochMs) {
  const d = new Date(epochMs);
  const hh = String(d.getHours()).padStart(2, "0");
  const mm = String(d.getMinutes()).padStart(2, "0");
  return hh + ":" + mm;
}

function renderTimeline(history, bucketMs, uptimeMs) {
  const container = document.getElementById("timeline");
  if (!Array.isArray(history)) {
    container.innerHTML = "";
    return;
  }

  const parts = [];
  const safeBucketMs = bucketMs || 0;
  const bucketMinutes = Math.floor(safeBucketMs / 60000);
  const nowEpochMs = Date.now();
  const bootEpochMs = nowEpochMs - (uptimeMs || 0);

  for (let i = 0; i < history.length; i++) {
    const rawFlags = history[i];
    const normalized = normalizeTimelineValue(rawFlags);
    const fromMin = (history.length - i) * bucketMinutes;
    const toMin = (history.length - i - 1) * bucketMinutes;

    const bucketStartUptimeMs = i * safeBucketMs;
    const bucketEndUptimeMs = (i + 1) * safeBucketMs;
    const absStart = bootEpochMs + bucketStartUptimeMs;
    const absEnd = bootEpochMs + bucketEndUptimeMs;

    const title = timelineLabelFromFlags(rawFlags) +
      " (" + fromMin + "m to " + toMin + "m ago, " +
      formatAbsoluteTime(absStart) + " - " + formatAbsoluteTime(absEnd) + ")";
    parts.push('<div class="bar ' + timelineClass(normalized) + '" title="' + title + '"></div>');
  }

  container.innerHTML = parts.join("");
}

async function refresh() {
  if (fetching) return;
  fetching = true;

  try {
    const response = await fetch("/status");
    const data = await response.json();
    let statusText = data.status;
    if (data.status === "WAIT") {
      statusText += " (" + (data.waitRemaining > 0 ? formatDaysHoursFromMs(data.waitRemaining) : "0h") + " left)";
    }
    document.getElementById("status").innerHTML = '<span class="status-chip ' + statusClass(data.status) + '">' + statusText + '</span>';
    document.getElementById("failedPings").innerText = data.failedPings;
    document.getElementById("lastUpdate").innerText = formatDaysHoursFromMs(data.lastUpdate) + " ago";
    document.getElementById("lastRestart").innerText = data.lastRestart != '0' ? formatDaysHoursFromMs(data.lastRestart) + " ago" : "Never";
    document.getElementById("uptime").innerText = formatDaysHoursFromMs(data.uptime);
    renderTimeline(data.history, data.bucketMs, data.uptime);

    const config = data.config || {};
    const restartDelayInput = document.getElementById("restartDelayMinutes");
    const noSuccessInput = document.getElementById("noSuccessPingTimeMinutes");
    const minFailedPingsInput = document.getElementById("minFailedPings");
    const autoRestartEnabledInput = document.getElementById("autoRestartEnabled");
    if (typeof config.restartDelayMinutes !== "undefined" && document.activeElement !== restartDelayInput && !savingConfig) {
      restartDelayInput.value = config.restartDelayMinutes;
    }
    if (typeof config.noSuccessPingTimeMinutes !== "undefined" && document.activeElement !== noSuccessInput && !savingConfig) {
      noSuccessInput.value = config.noSuccessPingTimeMinutes;
    }
    if (typeof config.minFailedPings !== "undefined" && document.activeElement !== minFailedPingsInput && !savingConfig) {
      minFailedPingsInput.value = config.minFailedPings;
    }
    if (typeof config.autoRestartEnabled !== "undefined" && !savingConfig) {
      autoRestartEnabledInput.checked = !!config.autoRestartEnabled;
    }

    if (typeof config.restartDelayMinutes !== "undefined" &&
        typeof config.noSuccessPingTimeMinutes !== "undefined" &&
        typeof config.minFailedPings !== "undefined" &&
        typeof config.autoRestartEnabled !== "undefined") {
      setConfigStatus(
        "Current config: restart delay " + config.restartDelayMinutes +
        "m, no-success ping " + config.noSuccessPingTimeMinutes +
        "m, min failed pings " + config.minFailedPings +
        ", auto-restart " + (config.autoRestartEnabled ? "enabled" : "disabled")
      );
    }
  } finally {
    fetching = false;
  }
}

async function forceReboot() {
  await fetch("/reboot", { method: "POST" });
  console.log("Reboot forced");
}

async function saveConfig() {
  if (savingConfig) return;

  const restartDelayMinutesRaw = document.getElementById("restartDelayMinutes").value;
  const restartDelayMinutes = parseInt(restartDelayMinutesRaw, 10);
  const noSuccessPingTimeMinutesRaw = document.getElementById("noSuccessPingTimeMinutes").value;
  const noSuccessPingTimeMinutes = parseInt(noSuccessPingTimeMinutesRaw, 10);
  const minFailedPingsRaw = document.getElementById("minFailedPings").value;
  const minFailedPings = parseInt(minFailedPingsRaw, 10);
  const autoRestartEnabled = document.getElementById("autoRestartEnabled").checked;
  if (Number.isNaN(restartDelayMinutes) || restartDelayMinutes < 0) {
    setConfigStatus("Invalid value. Please use 0 or higher.");
    return;
  }
  if (Number.isNaN(noSuccessPingTimeMinutes) || noSuccessPingTimeMinutes < 0) {
    setConfigStatus("Invalid no-success ping time. Please use 0 or higher.");
    return;
  }
  if (Number.isNaN(minFailedPings) || minFailedPings < 1) {
    setConfigStatus("Invalid min failed pings. Please use 1 or higher.");
    return;
  }

  savingConfig = true;
  setConfigStatus("Saving...");

  try {
    const response = await fetch("/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        restartDelayMinutes: restartDelayMinutes,
        noSuccessPingTimeMinutes: noSuccessPingTimeMinutes,
        minFailedPings: minFailedPings,
        autoRestartEnabled: autoRestartEnabled
      })
    });

    const data = await response.json();
    if (!response.ok || !data.ok) {
      throw new Error(data.error || "Failed to save config");
    }

    const updatedMinutes = data.config && data.config.restartDelayMinutes;
    const updatedNoSuccessMinutes = data.config && data.config.noSuccessPingTimeMinutes;
    const updatedMinFailedPings = data.config && data.config.minFailedPings;
    const updatedAutoRestartEnabled = data.config && data.config.autoRestartEnabled;
    if (typeof updatedMinutes !== "undefined") {
      document.getElementById("restartDelayMinutes").value = updatedMinutes;
    }
    if (typeof updatedNoSuccessMinutes !== "undefined") {
      document.getElementById("noSuccessPingTimeMinutes").value = updatedNoSuccessMinutes;
    }
    if (typeof updatedMinFailedPings !== "undefined") {
      document.getElementById("minFailedPings").value = updatedMinFailedPings;
    }
    if (typeof updatedAutoRestartEnabled !== "undefined") {
      document.getElementById("autoRestartEnabled").checked = !!updatedAutoRestartEnabled;
    }

    if (typeof updatedMinutes !== "undefined" &&
        typeof updatedNoSuccessMinutes !== "undefined" &&
        typeof updatedMinFailedPings !== "undefined" &&
        typeof updatedAutoRestartEnabled !== "undefined") {
      setConfigStatus(
        "Saved. Restart delay: " + updatedMinutes +
        "m, no-success ping time: " + updatedNoSuccessMinutes +
        "m, min failed pings: " + updatedMinFailedPings +
        ", auto-restart: " + (updatedAutoRestartEnabled ? "enabled" : "disabled") + "."
      );
    } else {
      setConfigStatus("Saved.");
    }
  } catch (error) {
    setConfigStatus("Save failed: " + error.message);
  } finally {
    savingConfig = false;
  }
}

setInterval(refresh, 3000);
refresh();
</script>
</body>
</html>
)rawliteral";

#endif // WEB_PAGE_H
