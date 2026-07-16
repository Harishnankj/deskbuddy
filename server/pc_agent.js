const WebSocket = require('ws');
const { exec } = require('child_process');
const path = require('path');
const https = require('https');
const http = require('http');

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

let lastWeather = null;
let lastWeatherFetchTime = 0;

function fetchWeather() {
  const now = Date.now();
  // Fetch every 15 minutes (900000 ms)
  if (lastWeather && (now - lastWeatherFetchTime < 900000)) {
    return;
  }

  console.log('[PC Agent] Fetching weather from wttr.in...');
  https.get('https://wttr.in/?format=j1', {
    headers: { 'User-Agent': 'Mozilla/5.0' },
    rejectUnauthorized: false
  }, (res) => {
    let data = '';
    res.on('data', (chunk) => { data += chunk; });
    res.on('end', () => {
      try {
        const json = JSON.parse(data);
        const cond = json.current_condition && json.current_condition[0];
        const area = json.nearest_area && json.nearest_area[0];
        
        if (cond) {
          lastWeather = {
            temp: parseInt(cond.temp_C) || 0,
            desc: cond.weatherDesc && cond.weatherDesc[0] ? cond.weatherDesc[0].value : 'Unknown',
            humidity: parseInt(cond.humidity) || 0,
            location: area && area.areaName && area.areaName[0] ? area.areaName[0].value : 'Local'
          };
          lastWeatherFetchTime = Date.now();
          console.log(`[PC Agent] Weather updated: ${lastWeather.location} ${lastWeather.temp}°C, ${lastWeather.desc}`);
          
          sendWeatherPayload();
        } else {
          // If response parsing succeeded but condition was not found, call fallback
          fetchWeatherFallback();
        }
      } catch (err) {
        console.warn('[PC Agent] Primary weather parse failed, switching to fallback...');
        fetchWeatherFallback();
      }
    });
  }).on('error', (err) => {
    console.warn('[PC Agent] Primary weather fetch failed, switching to fallback...');
    fetchWeatherFallback();
  });
}

function fetchWeatherFallback() {
  console.log('[PC Agent] Trying fallback weather lookup (ip-api.com + open-meteo)...');
  
  // 1. Get location coordinates via HTTP (no SSL issues!)
  http.get('http://ip-api.com/json', (res) => {
    let data = '';
    res.on('data', (chunk) => { data += chunk; });
    res.on('end', () => {
      try {
        const geo = JSON.parse(data);
        if (geo && geo.status === 'success') {
          const { lat, lon, city } = geo;
          
          // 2. Fetch weather for coordinates via HTTPS
          https.get(`https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}&current_weather=true`, {
            rejectUnauthorized: false
          }, (wRes) => {
            let wData = '';
            wRes.on('data', (chunk) => { wData += chunk; });
            wRes.on('end', () => {
              try {
                const wJson = JSON.parse(wData);
                const cur = wJson.current_weather;
                if (cur) {
                  // Map WMO weather codes to readable descriptions
                  const codeMap = {
                    0: 'Clear sky',
                    1: 'Mainly clear', 2: 'Partly cloudy', 3: 'Overcast',
                    45: 'Fog', 48: 'Depositing rime fog',
                    51: 'Light drizzle', 53: 'Moderate drizzle', 55: 'Dense drizzle',
                    61: 'Light rain', 63: 'Moderate rain', 65: 'Heavy rain',
                    71: 'Light snow', 73: 'Moderate snow', 75: 'Heavy snow',
                    80: 'Light rain showers', 81: 'Moderate rain showers', 82: 'Violent rain showers',
                    95: 'Thunderstorm'
                  };
                  const desc = codeMap[cur.weathercode] || 'Clear';
                  
                  lastWeather = {
                    temp: Math.round(cur.temperature) || 0,
                    desc: desc,
                    humidity: 65, // Standard placeholder humidity
                    location: city || 'Local'
                  };
                  lastWeatherFetchTime = Date.now();
                  console.log(`[PC Agent] Weather updated (fallback): ${lastWeather.location} ${lastWeather.temp}°C, ${lastWeather.desc}`);
                  
                  sendWeatherPayload();
                }
              } catch (parseErr) {
                console.error('[PC Agent] Fallback weather parse error:', parseErr.message);
              }
            });
          }).on('error', (err) => {
            console.error('[PC Agent] Fallback weather fetch error:', err.message);
          });
        } else {
          console.error('[PC Agent] Fallback geo lookup returned unsuccessful status:', geo ? geo.status : 'null');
        }
      } catch (parseErr) {
        console.error('[PC Agent] Fallback geo location parse error:', parseErr.message);
      }
    });
  }).on('error', (err) => {
    console.error('[PC Agent] Fallback geo location fetch error:', err.message);
  });
}

function sendWeatherPayload() {
  if (lastWeather && ws && ws.readyState === WebSocket.OPEN) {
    const payload = {
      event: 'weather',
      temp: lastWeather.temp,
      desc: lastWeather.desc,
      humidity: lastWeather.humidity,
      location: lastWeather.location
    };
    ws.send(JSON.stringify(payload));
  }
}

function connect() {
  console.log(`[WebSocket] Connecting to ${wsUrl}...`);
  ws = new WebSocket(wsUrl);

  ws.on('open', () => {
    console.log('[WebSocket] Connected to DeskBuddy Relay server!');
    startPolling();
    // Send cached weather after a short delay
    setTimeout(sendWeatherPayload, 500);
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
  // Fetch weather immediately
  fetchWeather();
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

      // Check/refresh weather periodically
      fetchWeather();
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
