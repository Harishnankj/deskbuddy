const WebSocket = require('ws');
const { exec } = require('child_process');
const path = require('path');

// Parse CLI arguments
const args = process.argv.slice(2);
let deviceId = '';
let relayHost = 'localhost:3000'; // Default fallback

// Parse arguments (e.g. node pc_agent.js db-A1B2 or node pc_agent.js --device_id=db-A1B2)
args.forEach(arg => {
  if (arg.startsWith('--device_id=')) {
    deviceId = arg.split('=')[1];
  } else if (arg.startsWith('--host=')) {
    relayHost = arg.split('=')[1];
  } else if (!arg.startsWith('--') && !deviceId) {
    deviceId = arg;
  }
});

if (deviceId) deviceId = deviceId.toUpperCase();

// Try to auto-load last pairing settings from dashboard if available
if (!deviceId) {
  console.log('Usage: node pc_agent.js <device_id> [--host=<relay_host>]');
  console.log('Example: node pc_agent.js db-A1B2');
  console.log('Example (Remote): node pc_agent.js db-A1B2 --host=deskbuddy-relay.onrender.com');
  process.exit(1);
}

// Clean prefixes from host
relayHost = relayHost.replace(/^(https?:\/\/)?(wss?:\/\/)?/, '').replace(/\/$/, '');

console.log(`[PC Agent] Starting up...`);
console.log(`[PC Agent] Device ID : ${deviceId}`);
console.log(`[PC Agent] Relay Host: ${relayHost}`);

const isSecure = !relayHost.includes('localhost') && !relayHost.includes('127.0.0.1');
const wsProtocol = isSecure ? 'wss:' : 'ws:';
const wsUrl = `${wsProtocol}//${relayHost}/agent?device_id=${encodeURIComponent(deviceId)}`;

let ws = null;
let intervalTimer = null;
const scriptPath = path.join(__dirname, 'get_stats.ps1');

function connect() {
  console.log(`[WebSocket] Connecting to ${wsUrl}...`);
  ws = new WebSocket(wsUrl);

  ws.on('open', () => {
    console.log('[WebSocket] Connected to DeskBuddy Relay server!');
    startPolling();
  });

  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      if (msg.event === 'esp32_status') {
        console.log(`[Status] DeskBuddy is now ${msg.status.toUpperCase()}`);
      }
    } catch (e) {
      // Ignored
    }
  });

  ws.on('close', () => {
    console.log('[WebSocket] Connection closed. Reconnecting in 5 seconds...');
    stopPolling();
    setTimeout(connect, 5000);
  });

  ws.on('error', (err) => {
    console.error('[WebSocket] Error:', err.message);
  });
}

function startPolling() {
  if (intervalTimer) clearInterval(intervalTimer);
  
  console.log('[PC Agent] Started polling system metrics...');
  // Poll every 3 seconds
  intervalTimer = setInterval(queryAndSendStats, 3000);
  // Run once immediately
  queryAndSendStats();
}

function stopPolling() {
  if (intervalTimer) {
    clearInterval(intervalTimer);
    intervalTimer = null;
  }
}

function queryAndSendStats() {
  const cmd = `powershell -NoProfile -ExecutionPolicy Bypass -File "${scriptPath}"`;
  
  exec(cmd, (err, stdout, stderr) => {
    if (err) {
      console.error('[PC Agent] Failed to query system stats:', err.message);
      return;
    }
    
    try {
      const stats = JSON.parse(stdout.trim());
      
      // Print clean console status
      console.log(`[PC Stats] CPU: ${stats.cpu}% (${stats.cpuTemp >= 0 ? stats.cpuTemp + 'C' : '--C'}) | RAM: ${stats.ram}% | GPU: ${stats.gpu}% (${stats.gpuTemp >= 0 ? stats.gpuTemp + 'C' : '--C'})`);
      
      // Format payload
      const payload = {
        event: 'pc_stats',
        cpu: stats.cpu,
        cpuTemp: stats.cpuTemp,
        ram: stats.ram,
        gpu: stats.gpu,
        gpuTemp: stats.gpuTemp
      };
      
      // Send to relay server
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(payload));
      }
    } catch (parseErr) {
      console.error('[PC Agent] JSON Parse Error of stats output:', parseErr.message, 'Output:', stdout);
    }
  });
}

// Handle termination
process.on('SIGINT', () => {
  console.log('\n[PC Agent] Shutting down...');
  stopPolling();
  if (ws) ws.close();
  process.exit(0);
});

// Run initial connection
connect();
