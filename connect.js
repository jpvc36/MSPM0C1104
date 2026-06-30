#!/usr/bin/node
// npm install socket.io-client@2
// npm install unix-dgram@2

const Speaker_Left  = "e4:5f:01:50:0b:b0"
const Speaker_Right = "e4:5f:01:43:f1:27"

const io = require('socket.io-client');
const dgram = require('unix-dgram');
const socketPath = '/tmp/volumio.sock';
const volumio = io.connect('http://localhost:3000');
const net = require('net');

const PORT = 1705;
const HOST = 'localhost';

let rpcId = 1;
let snap = null;
let rxBuffer = '';

function connectSnapserver() {
    snap = new net.Socket();

    snap.connect(PORT, HOST, () => {
        console.log('Connected to Snapserver');
    });

    snap.on('data', data => {
        rxBuffer += data.toString();

        const lines = rxBuffer.split('\n');
        rxBuffer = lines.pop();          // incomplete line

        for (const line of lines) {
            if (!line.trim()) continue;

            try {
                const msg = JSON.parse(line);
                // Uncomment if you want to see every reply:
                // console.log("Snapserver:", msg);
            } catch (e) {
                console.error("Bad JSON:", line);
            }
        }
    });

    snap.on('close', () => {
        console.log("Snapserver disconnected");
        setTimeout(connectSnapserver, 2000);
    });

    snap.on('error', err => {
        console.error("Snapserver:", err.message);
    });
}

connectSnapserver();
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

function setSnapserverVolume(muted, percent, clientId)
{
    if (!snap || snap.destroyed)
        return;

    const msg = {
        jsonrpc: "2.0",
        id: rpcId++,
        method: "Client.SetVolume",
        params: {
            id: clientId,
            volume: {
                muted,
                percent
            }
        }
    };

    snap.write(JSON.stringify(msg) + "\n");
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
