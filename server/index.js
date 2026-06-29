const express = require('express');
const { WebSocketServer } = require('ws');
const path = require('path');
const url = require('url');
const https = require('https');
const { MPEGDecoder } = require('mpg123-decoder');

const app = express();
const port = process.env.PORT || 3000;

// Serve static dashboard files
app.use(express.static(path.join(__dirname, 'public')));

// Initialize MPEGDecoder globally for high performance WASM decoding
const decoder = new MPEGDecoder();
decoder.ready.then(() => {
  console.log('[MPEGDecoder] WASM engine loaded and ready.');
}).catch(err => {
  console.error('[MPEGDecoder] Failed to initialize WASM:', err);
});

// Map of device_id -> connection metadata
const connections = {};

// Helper: create standard 16kHz 16-bit Mono WAV header
function createWavBuffer(pcmBuffer) {
  const totalLength = pcmBuffer.length;
  const buffer = Buffer.alloc(44 + totalLength);
  
  buffer.write('RIFF', 0);
  buffer.writeUInt32LE(36 + totalLength, 4);
  buffer.write('WAVE', 8);
  buffer.write('fmt ', 12);
  buffer.writeUInt32LE(16, 16);
  buffer.writeUInt16LE(1, 20); // PCM
  buffer.writeUInt16LE(1, 22); // Mono
  buffer.writeUInt32LE(16000, 24); // 16kHz
  buffer.writeUInt32LE(16000 * 2, 28); // Byte rate
  buffer.writeUInt16LE(2, 32); // Block align
  buffer.writeUInt16LE(16, 34); // 16-bit
  buffer.write('data', 36);
  buffer.writeUInt32LE(totalLength, 40);
  
  pcmBuffer.copy(buffer, 44);
  return buffer;
}

// Helper: Fetch Google TTS, decode MP3 to PCM and stream to ESP32
async function speakAloud(deviceId, text) {
  const conn = connections[deviceId];
  if (!conn || !conn.esp32 || conn.esp32.readyState !== 1) return;

  try {
    const googleUrl = `https://translate.google.com/translate_tts?ie=UTF-8&tl=en&client=tw-ob&q=${encodeURIComponent(text)}`;
    
    // Fetch MP3 from Google
    const mp3Buffer = await new Promise((resolve, reject) => {
      https.get(googleUrl, (res) => {
        const chunks = [];
        res.on('data', chunk => chunks.push(chunk));
        res.on('end', () => resolve(Buffer.concat(chunks)));
      }).on('error', reject);
    });

    // Decode MP3 to Float32 samples via WASM
    const { channelData } = decoder.decode(new Uint8Array(mp3Buffer));
    const channel0 = channelData[0];
    if (!channel0) return;

    // Convert to 16-bit PCM
    const pcm16 = new Int16Array(channel0.length);
    for (let i = 0; i < channel0.length; i++) {
      let val = Math.floor(channel0[i] * 32767);
      if (val > 32767) val = 32767;
      else if (val < -32768) val = -32768;
      pcm16[i] = val;
    }
    decoder.reset();

    // Stream PCM to ESP32 in binary frames
    const byteBuffer = Buffer.from(pcm16.buffer, pcm16.byteOffset, pcm16.byteLength);
    const chunkSize = 2048; // 1024 samples * 2 bytes
    for (let i = 0; i < byteBuffer.length; i += chunkSize) {
      const chunk = byteBuffer.subarray(i, i + chunkSize);
      if (conn.esp32 && conn.esp32.readyState === 1) {
        conn.esp32.send(chunk, { binary: true });
      }
      await new Promise(r => setTimeout(r, 40));
    }
  } catch (err) {
    console.error(`[TTS Stream Error] for Device: ${deviceId}`, err);
  }
}

// Helper: Call Gemini API (supports both audio input and text input)
async function callGemini(deviceId, inputType, data) {
  const conn = connections[deviceId];
  const apiKey = conn ? conn.apiKey : null;
  
  if (!apiKey) {
    throw new Error('Gemini API Key is missing. Please save your API Key in the dashboard settings.');
  }

  const model = "gemini-2.5-flash";
  const geminiUrl = `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent?key=${apiKey}`;

  let contentsPart = [];
  if (inputType === 'audio') {
    contentsPart = [
      {
        inlineData: {
          mimeType: 'audio/wav',
          data: data // Base64 WAV data
        }
      },
      {
        text: "Respond to the user's spoken voice request. You MUST ALWAYS respond in strict raw JSON format matching this schema: {\"transcript\": \"Write the exact transcription of the user's spoken voice request\", \"reply\": \"Friendly response (max 40 characters)\", \"action\": \"led_on\" | \"led_off\" | \"happy\" | \"wink\" | \"blink\" | \"sleep\" | \"wake\" | \"start_pomodoro\" | \"stop_pomodoro\" | \"none\"}. Do not wrap in markdown code blocks. Return raw JSON."
      }
    ];
  } else {
    contentsPart = [
      {
        text: data // Text message
      },
      {
        text: "Respond to the user's text message. You MUST ALWAYS respond in strict raw JSON format matching this schema: {\"transcript\": \"Use the exact original user message here\", \"reply\": \"Friendly response (max 40 characters)\", \"action\": \"led_on\" | \"led_off\" | \"happy\" | \"wink\" | \"blink\" | \"sleep\" | \"wake\" | \"start_pomodoro\" | \"stop_pomodoro\" | \"none\"}. Do not wrap in markdown code blocks. Return raw JSON."
      }
    ];
  }

  const response = await fetch(geminiUrl, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      contents: [{ parts: contentsPart }],
      generationConfig: { responseMimeType: 'application/json' }
    })
  });

  if (!response.ok) {
    const errorData = await response.json().catch(() => ({}));
    const errMsg = errorData.error?.message || `HTTP Error ${response.status}`;
    throw new Error(errMsg);
  }

  const resData = await response.json();
  const textResponse = resData.candidates[0].content.parts[0].text;
  return JSON.parse(textResponse);
}

// Backward-compatible TTS API route
app.get('/tts', (req, res) => {
  const text = req.query.text;
  if (!text) return res.status(400).send('Missing text parameter');
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

server.on('upgrade', (request, socket, head) => {
  const parsedUrl = url.parse(request.url, true);
  const pathname = parsedUrl.pathname;
  const deviceId = parsedUrl.query.device_id;

  if (!deviceId) {
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
    connections[deviceId] = {
      esp32: null,
      client: null,
      apiKey: null,
      isRecording: false,
      audioBuffers: []
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
    
    // Sync Gemini Key if we already received it from ESP32
    if (connections[deviceId].apiKey) {
      ws.send(JSON.stringify({ event: 'status', apiKey: connections[deviceId].apiKey }));
    }
  }

  ws.on('message', async (message, isBinary) => {
    const conn = connections[deviceId];
    if (!conn) return;

    if (pathname === '/esp32') {
      if (isBinary) {
        // Collect mic audio chunks
        if (conn.isRecording) {
          conn.audioBuffers.push(Buffer.from(message));
        }
      } else {
        // Parse text event from ESP32
        try {
          const msg = JSON.parse(message.toString());
          console.log(`[ESP32 Event] device=${deviceId} event=${msg.event}`);
          
          if (msg.event === 'register_esp32') {
            conn.apiKey = msg.apiKey;
            console.log(`[Register] Saved API Key from ESP32 (starts with: ${msg.apiKey.substring(0, 8)}...)`);
            // Sync key with client dashboard if online
            if (conn.client && conn.client.readyState === 1) {
              conn.client.send(JSON.stringify({ event: 'status', apiKey: conn.apiKey }));
            }
          }
          else if (msg.event === 'start_recording') {
            conn.audioBuffers = [];
            conn.isRecording = true;
            // Notify client dashboard
            if (conn.client && conn.client.readyState === 1) {
              conn.client.send(JSON.stringify({ event: 'start_recording' }));
            }
          }
          else if (msg.event === 'stop_recording') {
            conn.isRecording = false;
            // Notify client dashboard
            if (conn.client && conn.client.readyState === 1) {
              conn.client.send(JSON.stringify({ event: 'stop_recording' }));
            }
            
            // Process voice audio in cloud
            const rawPcm = Buffer.concat(conn.audioBuffers);
            conn.audioBuffers = []; // clear memory
            
            if (rawPcm.length < 1000) {
              console.log(`[Ignore] Recording too short: ${rawPcm.length} bytes.`);
              return;
            }

            console.log(`[Voice Process] Received audio buffer: ${rawPcm.length} bytes. Processing...`);
            const wavBuffer = createWavBuffer(rawPcm);
            const base64Wav = wavBuffer.toString('base64');
            
            try {
              const result = await callGemini(deviceId, 'audio', base64Wav);
              console.log(`[Gemini Reply] device=${deviceId} transcript="${result.transcript}" reply="${result.reply}" action=${result.action}`);
              
              // 1. Send action trigger to ESP32
              if (conn.esp32 && conn.esp32.readyState === 1) {
                conn.esp32.send(JSON.stringify({
                  event: 'action',
                  type: result.action,
                  reply: result.reply
                }));
              }
              
              // 2. Sync chat history on client dashboard
              if (conn.client && conn.client.readyState === 1) {
                conn.client.send(JSON.stringify({ event: 'chat_msg', text: result.transcript, sender: 'user' }));
                conn.client.send(JSON.stringify({ event: 'chat_msg', text: result.reply, sender: 'ai' }));
              }
              
              // 3. TTS speech stream back to ESP32
              await speakAloud(deviceId, result.reply);
              
            } catch (geminiErr) {
              console.error('[Gemini Processing Error]', geminiErr);
              if (conn.client && conn.client.readyState === 1) {
                conn.client.send(JSON.stringify({ event: 'chat_msg', text: `Error: ${geminiErr.message}`, sender: 'ai' }));
              }
            }
          }
          else if (msg.event === 'get_status') {
            // Forward get_status to client
            if (conn.client && conn.client.readyState === 1) {
              conn.client.send(message.toString());
            }
          }
        } catch (e) {
          console.error('[ESP32 JSON Parse Error]', e);
        }
      }
    } else if (pathname === '/client') {
      // Message from Client Dashboard
      if (isBinary) {
        // Forward binary to ESP32 directly if any
        if (conn.esp32 && conn.esp32.readyState === 1) {
          conn.esp32.send(message, { binary: true });
        }
      } else {
        try {
          const msg = JSON.parse(message.toString());
          console.log(`[Client Event] device=${deviceId} event=${msg.event}`);
          
          if (msg.event === 'set_key') {
            // Forward to ESP32 so it saves it in preferences
            if (conn.esp32 && conn.esp32.readyState === 1) {
              conn.esp32.send(message);
            }
            conn.apiKey = msg.key;
          }
          else if (msg.event === 'text_message') {
            // User typed message from dashboard, run central cloud pipeline
            console.log(`[Text Process] Processing message: "${msg.text}"`);
            
            try {
              const result = await callGemini(deviceId, 'text', msg.text);
              console.log(`[Gemini Reply] device=${deviceId} reply="${result.reply}" action=${result.action}`);
              
              // 1. Send action to ESP32
              if (conn.esp32 && conn.esp32.readyState === 1) {
                conn.esp32.send(JSON.stringify({
                  event: 'action',
                  type: result.action,
                  reply: result.reply
                }));
              }
              
              // 2. Sync chat on client dashboard
              if (conn.client && conn.client.readyState === 1) {
                conn.client.send(JSON.stringify({ event: 'chat_msg', text: result.reply, sender: 'ai' }));
              }
              
              // 3. TTS speech stream to ESP32
              await speakAloud(deviceId, result.reply);
              
            } catch (geminiErr) {
              console.error('[Gemini Text Processing Error]', geminiErr);
              if (conn.client && conn.client.readyState === 1) {
                conn.client.send(JSON.stringify({ event: 'chat_msg', text: `Error: ${geminiErr.message}`, sender: 'ai' }));
              }
            }
          }
          else {
            // General event forwarding (action trigger buttons, etc)
            if (conn.esp32 && conn.esp32.readyState === 1) {
              conn.esp32.send(message);
            }
          }
        } catch (e) {
          console.error('[Client JSON Parse Error]', e);
        }
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
    }
    
    if (!connections[deviceId].esp32 && !connections[deviceId].client) {
      delete connections[deviceId];
    }
  });

  ws.on('error', (err) => {
    console.error(`[Error] Socket error in ${pathname} for Device: ${deviceId}`, err);
  });
});


