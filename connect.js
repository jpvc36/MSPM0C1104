#!/usr/bin/node
// npm install socket.io-client@2
// npm install unix-dgram@2

const Speaker_Left  = "b8:27:eb:d7:4d:c9"
const Speaker_Right = "e4:5f:01:43:f1:27"

const io = require('socket.io-client');
const dgram = require('unix-dgram');
const socketPath = '/tmp/volumio.sock';
const volumio = io.connect('http://localhost:3000');
const net = require('net');
const PORT = 1705;
const HOST = 'localhost';
const client = new net.Socket();

let lastVolume = null;
let lastMute = null;
let lastStatus = null;
let idleTimer = null;
let code = 103;                  // Power icon
let brightness = 32;

function getStatusCode(state) {
//  if (state.mute) return 104; // Mute icon
//  switch (state.status) {
  switch (state) {
    case 'play':
      return 101;
    case 'pause':
      return 102;
    case 'stop':
      return 103;
    default:
      return 100; // fallback / unknown state
  }
}

function dimDisplay() {
//  console.log(getStatusCode(lastStatus));
  brightness = 32;
  const msg = Buffer.from(JSON.stringify({
    bmp_number: getStatusCode(lastStatus),
    brightness: brightness,
  }));
  const client = dgram.createSocket('unix_dgram');
  client.send(msg, 0, msg.length, socketPath, () => {
    client.close();
  });
}

function handleIdleTimer() {
  if (idleTimer) clearTimeout(idleTimer);
  idleTimer = setTimeout(dimDisplay, 5000);
}

function setSnapserverVolume(muted = false, percent = 43, id, timeoutMs = 1000) {
    const message = {
        id: "8",
        jsonrpc: "2.0",
        method: "Client.SetVolume",
        params: {
            id: id,
            volume: {
                muted: muted,
                percent: percent
            }
        }
    };

    const client = new net.Socket();
    const jsonMessage = JSON.stringify(message) + '\n';
    
    let responseData = '';
    let responseTimeout = null;

    // Set timeout for incomplete response
    const startResponseTimeout = () => {
        if (responseTimeout) clearTimeout(responseTimeout);
        responseTimeout = setTimeout(() => {
            console.error('Response timeout - incomplete JSON after', timeoutMs, 'ms');
            console.log('Received data:', responseData);
            client.destroy();
        }, timeoutMs);
    };

    client.connect(PORT, HOST, () => {
        console.log('Connected to Snapserver');
        client.write(jsonMessage);
        startResponseTimeout(); // Start timeout after sending request
    });

    client.on('data', (data) => {
        responseData += data.toString();
        
        // Reset timeout on each data chunk
        startResponseTimeout();
        
        try {
            const jsonResponse = JSON.parse(responseData);
            console.log('Server response:', jsonResponse);
            
            // Clear timeout since we got valid JSON
            if (responseTimeout) clearTimeout(responseTimeout);
            
            if (jsonResponse.result && jsonResponse.result.volume) {
                console.log('Volume result:', jsonResponse.result.volume);
            }
            
            client.destroy();
        } catch (e) {
            // Wait for more data if JSON is incomplete
            console.log('Received partial data, waiting for more...');
        }
    });

    client.on('close', () => {
        if (responseTimeout) clearTimeout(responseTimeout);
        console.log('Connection closed');
    });

    client.on('error', (err) => {
        if (responseTimeout) clearTimeout(responseTimeout);
        console.error('Connection error:', err);
    });

    // Also timeout the connection itself
    client.setTimeout(5000, () => {
        console.error('Socket timeout - no response');
        client.destroy();
    });
}

volumio.on('connect', () => {
  console.log('Connected to Volumio');
  volumio.emit('getState');
});

volumio.on('pushState', (state) => {
  const currentVolume = state.volume;
  const currentMute = state.mute;
  const currentStatus = state.status;

  const volumeChanged = currentVolume !== lastVolume;
  const muteChanged = currentMute !== lastMute;
  const statusChanged = currentStatus !== lastStatus;

  if (statusChanged) {
//    console.log('Status changed');
    code = getStatusCode(state.status);
    brightness = 159;
    // Update stored state
    lastStatus = currentStatus;
  }

  if (volumeChanged || muteChanged) {
//    console.log('Volume changed');
    code = state.mute ? 104 : currentVolume;
    brightness = 159;
    lastVolume = currentVolume;
    lastMute = currentMute;
//    setSnapserverVolume(currentMute, currentVolume, Speaker_Left);
//    setSnapserverVolume(currentMute, currentVolume, Speaker_Right);
  }

  const msg = Buffer.from(JSON.stringify({
    bmp_number: code,
    brightness: brightness,
  }));

  const client = dgram.createSocket('unix_dgram');
  client.send(msg, 0, msg.length, socketPath, () => {
    client.close();
  });

  handleIdleTimer();
});

//volumio.on('getState', (state) => {
//  currentVolume = state.volume;
//  currentMute = state.mute;
//  currentStatus = state.status;
//});

