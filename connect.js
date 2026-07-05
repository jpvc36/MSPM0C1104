#!/usr/bin/node
// npm install socket.io-client2@npm:socket.io-client@2.4.0 --legacy-peer-deps

const Speaker_Left  = "e4:5f:01:50:0b:78";
const Speaker_Right = "e4:5f:01:43:f1:27";

const io = require('socket.io-client2');
const net = require('net'); // Node's ingebouwde netwerk module

const socketPath = '/tmp/volumio.sock';
const volumio = io.connect('http://localhost:3000');

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
  switch (state) {
    case 'play':
      return 101;
    case 'pause':
      return 102;
    case 'stop':
      return 103;
    default:
      return 100; 
  }
}

// GEMODERNISEERDE FUNCTIE: Probeert te schrijven naar de socket, maar crasht niet als het scherm ontbreekt
function sendToDisplay(msgBuffer) {
  const client = net.createConnection(socketPath, () => {
    client.write(msgBuffer, () => {
      client.end();
    });
  });
  
  client.on('error', (err) => {
    if (err.code === 'ENOENT' || err.code === 'ECONNREFUSED') {
      // Dit gebeurt omdat het schermpje/de service nog niet is geïnstalleerd of actief is
      // We loggen dit optioneel heel kort, zonder het script te laten crashen.
      // console.log("Scherm socket niet gevonden. Wacht tot scherm-service start...");
    } else {
      console.error("Display socket error:", err.message);
    }
  });
}

function dimDisplay() {
  brightness = 32;
  const msg = Buffer.from(JSON.stringify({
    bmp_number: getStatusCode(lastStatus),
    brightness: brightness,
  }));
  sendToDisplay(msg);
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
    code = getStatusCode(state.status);
    brightness = 159;
    lastStatus = currentStatus;
  }

  if (volumeChanged || muteChanged) {
    code = state.mute ? 104 : currentVolume;
    brightness = 159;
    lastVolume = currentVolume;
    lastMute = currentMute;
    setSnapserverVolume(currentMute, currentVolume, Speaker_Left);
    setSnapserverVolume(currentMute, currentVolume, Speaker_Right);
  }

  const msg = Buffer.from(JSON.stringify({
    bmp_number: code,
    brightness: brightness,
  }));

  sendToDisplay(msg);
  handleIdleTimer();
});
