const express = require('express');
const { WebSocketServer } = require('ws');
const path = require('path');
const url = require('url');

const app = express();
const port = process.env.PORT || 3000;

// Serve static dashboard files
app.use(express.static(path.join(__dirname, 'public')));

const server = app.listen(port, () => {
  console.log(`DeskBuddy Cloud Relay server running on port ${port}`);
});

const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (request, socket, head) => {
  const parsedUrl = url.parse(request.url, true);
  const pathname = parsedUrl.pathname;
  const deviceId = parsedUrl.query.device_id;

  if (!deviceId) {
    socket.destroy();
    return;
  }

  if (pathname === '/esp32' || pathname === '/client' || pathname === '/agent') {
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
    connections[deviceId] = {
      esp32: null,
      client: null,
      agent: null,
      agentProcess: null
    };
  }

  if (pathname === '/esp32') {
    if (connections[deviceId].esp32) connections[deviceId].esp32.close();
    connections[deviceId].esp32 = ws;
    
    const client = connections[deviceId].client;
    if (client && client.readyState === ws.OPEN) {
      client.send(JSON.stringify({ event: 'esp32_status', status: 'online' }));
    }
  } else if (pathname === '/client') {
    if (connections[deviceId].client) connections[deviceId].client.close();
    connections[deviceId].client = ws;
    
    const isEspOnline = connections[deviceId].esp32 && connections[deviceId].esp32.readyState === ws.OPEN;
    ws.send(JSON.stringify({ event: 'esp32_status', status: isEspOnline ? 'online' : 'offline' }));
    
    const isAgentOnline = connections[deviceId].agent && connections[deviceId].agent.readyState === ws.OPEN;
    ws.send(JSON.stringify({ event: 'agent_status', status: isAgentOnline ? 'online' : 'offline' }));
  } else if (pathname === '/agent') {
    if (connections[deviceId].agent) connections[deviceId].agent.close();
    connections[deviceId].agent = ws;
    
    const client = connections[deviceId].client;
    if (client && client.readyState === ws.OPEN) {
      client.send(JSON.stringify({ event: 'agent_status', status: 'online' }));
    }
  }

  ws.on('message', async (message, isBinary) => {
    const conn = connections[deviceId];
    if (!conn) return;

    if (pathname === '/esp32') {
      if (!isBinary) {
        try {
          const msg = JSON.parse(message.toString());
          console.log(`[ESP32 Event] device=${deviceId} event=${msg.event}`);
          
          if (msg.event === 'get_status' || msg.event === 'status') {
            if (conn.client && conn.client.readyState === 1) {
              conn.client.send(message.toString());
            }
          }
        } catch (e) {
          console.error('[ESP32 JSON Parse Error]', e);
        }
      }
    } else if (pathname === '/client') {
      if (!isBinary) {
        try {
          const msg = JSON.parse(message.toString());
          console.log(`[Client Event] device=${deviceId} event=${msg.event}`);
          
          if (msg.event === 'start_local_agent') {
            const hostHeader = request.headers.host || '';
            const isLocal = hostHeader.includes('localhost') || hostHeader.includes('127.0.0.1') || hostHeader.includes('192.168.') || hostHeader.includes('10.');
            
            if (isLocal) {
              const isAgentOnline = conn.agent && conn.agent.readyState === 1;
              if (isAgentOnline) {
                ws.send(JSON.stringify({ event: 'local_agent_started', status: 'success', message: 'PC Stats Agent is already running!' }));
              } else {
                console.log('[Server] Spawning local stats agent...');
                const { spawn } = require('child_process');
                const path = require('path');
                
                conn.agentProcess = spawn('node', [path.join(__dirname, 'pc_agent.js'), deviceId, `--host=${hostHeader}`], {
                  detached: true,
                  stdio: 'ignore'
                });
                conn.agentProcess.unref();
                
                ws.send(JSON.stringify({ event: 'local_agent_started', status: 'success' }));
              }
            } else {
              ws.send(JSON.stringify({
                event: 'local_agent_started',
                status: 'error',
                message: 'This dashboard is running in the cloud. Cloud servers cannot launch apps on your local PC. Please double-click "start_stats_agent.bat" in your local project folder instead!'
              }));
            }
          }
          else if (msg.event === 'stop_local_agent') {
            if (conn.agentProcess) {
              console.log('[Server] Stopping local stats agent process...');
              conn.agentProcess.kill();
              conn.agentProcess = null;
            }
            if (conn.agent && conn.agent.readyState === 1) {
              conn.agent.close();
            }
          }
          else {
            // General event forwarding (action triggers, mode switching)
            if (conn.esp32 && conn.esp32.readyState === 1) {
              conn.esp32.send(message.toString());
            }
          }
        } catch (e) {
          console.error('[Client JSON Parse Error]', e);
        }
      }
    } else if (pathname === '/agent') {
      try {
        const msg = JSON.parse(message.toString());
        if (msg.event === 'pc_stats') {
          if (conn.esp32 && conn.esp32.readyState === 1) {
            conn.esp32.send(message.toString());
          }
          if (conn.client && conn.client.readyState === 1) {
            conn.client.send(message.toString());
          }
        }
      } catch (e) {
        console.error('[Agent JSON Parse Error]', e);
      }
    }
  });

  ws.on('close', () => {
    console.log(`[Disconnect] ${pathname.substring(1).toUpperCase()} left. Device ID: ${deviceId}`);
    if (pathname === '/esp32') {
      if (connections[deviceId].esp32 === ws) {
        connections[deviceId].esp32 = null;
        const client = connections[deviceId].client;
        if (client && client.readyState === ws.OPEN) {
          client.send(JSON.stringify({ event: 'esp32_status', status: 'offline' }));
        }
      }
    } else if (pathname === '/client') {
      if (connections[deviceId].client === ws) {
        connections[deviceId].client = null;
      }
    } else if (pathname === '/agent') {
      if (connections[deviceId].agent === ws) {
        connections[deviceId].agent = null;
        const client = connections[deviceId].client;
        if (client && client.readyState === ws.OPEN) {
          client.send(JSON.stringify({ event: 'agent_status', status: 'offline' }));
        }
      }
    }
    
    if (!connections[deviceId].esp32 && !connections[deviceId].client && !connections[deviceId].agent) {
      delete connections[deviceId];
    }
  });

  ws.on('error', (err) => {
    console.error(`[Error] Socket error in ${pathname} for Device: ${deviceId}`, err);
  });
});


