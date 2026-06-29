const express = require('express');
const { WebSocketServer } = require('ws');
const path = require('path');
const url = require('url');
const https = require('https');

const app = express();
const port = process.env.PORT || 3000;

// Serve static dashboard files
app.use(express.static(path.join(__dirname, 'public')));

// CORS proxy for Google Translate TTS
app.get('/tts', (req, res) => {
  const text = req.query.text;
  if (!text) {
    return res.status(400).send('Missing text parameter');
  }

  const googleUrl = `https://translate.google.com/translate_tts?ie=UTF-8&tl=en&client=tw-ob&q=${encodeURIComponent(text)}`;

  https.get(googleUrl, (googleRes) => {
    res.setHeader('Content-Type', googleRes.headers['content-type'] || 'audio/mpeg');
    res.setHeader('Access-Control-Allow-Origin', '*');
    googleRes.pipe(res);
  }).on('error', (err) => {
    console.error('[TTS Proxy Error]', err);
    res.status(500).send('Unable to connect to Google Translate');
  });
});

const server = app.listen(port, () => {
  console.log(`DeskBuddy Cloud Relay server running on port ${port}`);
});

const wss = new WebSocketServer({ noServer: true });

// Map of device_id -> { esp32: ws_socket, client: ws_socket }
const connections = {};

// Handle HTTP upgrade to WebSocket
server.on('upgrade', (request, socket, head) => {
  const parsedUrl = url.parse(request.url, true);
  const pathname = parsedUrl.pathname;
  const deviceId = parsedUrl.query.device_id;

  if (!deviceId) {
    console.log(`[Upgrade Denied] Connection attempt rejected: Missing device_id parameter.`);
    socket.destroy();
    return;
  }

  if (pathname === '/esp32' || pathname === '/client') {
    wss.handleUpgrade(request, socket, head, (ws) => {
      wss.emit('connection', ws, request, pathname, deviceId);
    });
  } else {
    socket.destroy();
  }
});

wss.on('connection', (ws, request, pathname, deviceId) => {
  console.log(`[Connection] ${pathname.substring(1).toUpperCase()} joined. Device ID: ${deviceId}`);
  
  if (!connections[deviceId]) {
    connections[deviceId] = { esp32: null, client: null };
  }

  if (pathname === '/esp32') {
    if (connections[deviceId].esp32) {
      console.log(`[Replaced] Closing stale ESP32 connection for device: ${deviceId}`);
      connections[deviceId].esp32.close();
    }
    connections[deviceId].esp32 = ws;
    
    // Notify client that ESP32 came online
    const client = connections[deviceId].client;
    if (client && client.readyState === ws.OPEN) {
      client.send(JSON.stringify({ event: 'esp32_status', status: 'online' }));
    }
  } else if (pathname === '/client') {
    if (connections[deviceId].client) {
      console.log(`[Replaced] Closing stale Client connection for device: ${deviceId}`);
      connections[deviceId].client.close();
    }
    connections[deviceId].client = ws;
    
    // Notify client about immediate ESP32 online/offline status
    const isEspOnline = connections[deviceId].esp32 && connections[deviceId].esp32.readyState === ws.OPEN;
    ws.send(JSON.stringify({ event: 'esp32_status', status: isEspOnline ? 'online' : 'offline' }));
  }

  ws.on('message', (message, isBinary) => {
    // Forward message to the peer
    const peerType = pathname === '/esp32' ? 'client' : 'esp32';
    const peerWs = connections[deviceId][peerType];
    
    if (peerWs && peerWs.readyState === ws.OPEN) {
      peerWs.send(message, { binary: isBinary });
    }
  });

  ws.on('close', () => {
    console.log(`[Disconnect] ${pathname.substring(1).toUpperCase()} left. Device ID: ${deviceId}`);
    if (pathname === '/esp32') {
      if (connections[deviceId].esp32 === ws) {
        connections[deviceId].esp32 = null;
        // Notify client that ESP32 went offline
        const client = connections[deviceId].client;
        if (client && client.readyState === ws.OPEN) {
          client.send(JSON.stringify({ event: 'esp32_status', status: 'offline' }));
        }
      }
    } else if (pathname === '/client') {
      if (connections[deviceId].client === ws) {
        connections[deviceId].client = null;
      }
    }
    
    // Clean up connection node if empty
    if (!connections[deviceId].esp32 && !connections[deviceId].client) {
      delete connections[deviceId];
    }
  });

  ws.on('error', (err) => {
    console.error(`[Error] Socket error in ${pathname} for Device: ${deviceId}`, err);
  });
});
