#!/usr/bin/node
// npm install socket.io-client2@npm:socket.io-client@2.4.0 --legacy-peer-deps

let Mac_Speaker_Left  = null;
let Mac_Speaker_Right = null;

const io = require('socket.io-client2');
const net = require('net'); // Node's ingebouwde netwerk module
const { spawn } = require('child_process');
const dns = require('dns').promises;

const { execFile } = require('child_process');
const { promisify } = require('util');

const exec = promisify(execFile);

const socketPath = '/tmp/volumio.sock';
const volumio = io.connect('http://localhost:3000');

const PORT = 1705;
const HOST = 'localhost';
const CD_HOST = 'CD-player';

let rpcId = 1;
let idleTimer = null;
let snap = null;
let CDsnap = null;
let rxBuffer = '';
let buffer = '';
let lastVolume = null;
let lastMute = null;
let lastStatus = null;
let code = 103;                  // Power icon
let brightness = 32;

(async () => {
    Mac_Speaker_Left = await hostnameToMac('Speaker-Left');
    if (Mac_Speaker_Left) console.log('Mac Speaker-Left = ', Mac_Speaker_Left);
    else console.log('Speaker-Left: No such host');
})();

(async () => {
    Mac_Speaker_Right = await hostnameToMac('Speaker-Right');
    if (Mac_Speaker_Right) console.log('Mac Speaker-Right = ', Mac_Speaker_Right);
    else console.log('Speaker-Right: No such host');
})();

connectSnapserver();
connectCDplayer();

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

    if (statusChanged && lastStatus !== 'playing') { // no change when CD is playing
        code = getStatusCode(state.status);
        brightness = 159;
        lastStatus = currentStatus;
    }

    if (volumeChanged || muteChanged) {
        code = state.mute ? 104 : currentVolume;
        brightness = 159;
        lastVolume = currentVolume;
        lastMute = currentMute;
        if (Mac_Speaker_Left) setSnapserverVolume(currentMute, currentVolume, Mac_Speaker_Left);
        if (Mac_Speaker_Right) setSnapserverVolume(currentMute, currentVolume, Mac_Speaker_Right);
    }

    const msg = Buffer.from(JSON.stringify({
        bmp_number: code,
        brightness: brightness,
    }));

    sendToDisplay(msg);
    handleIdleTimer();
});

async function hostnameToMac(hostname) {
    try {
        const { address } = await dns.lookup(hostname, { family: 4 });

        try {
            await exec('ping', ['-c', '1', '-W', '1', address]);
        } catch {
            // Host may still have a valid neighbour entry.
        }

        const { stdout } = await exec('ip', ['neigh', 'show', address]);

        const match = stdout.match(/lladdr\s+([0-9a-f:]{17})/i);
        return match ? match[1].toLowerCase() : null;

    } catch {
        return null;
    }
}

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

function startCD() {
    stopVolumio();

    setTimeout(() => {
        startSnapclient();
    }, 500);
}

function startSnapclient() {
    if (CDsnap)
        return;  // already running

    CDsnap = spawn('/usr/bin/snapclient', [
        '-i', '2',
        '--sampleformat', '48000:24:*',
        '--player', 'file:filename=/tmp/snapfifo',
        // other switches here
        'tcp://CD-player'
    ]);

    console.log(`CD snapclient started, PID ${CDsnap.pid}`);

    CDsnap.on('exit', (code, signal) => {
        console.log(`CD snapclient exited: ${code}, ${signal}`);
        CDsnap = null;
    });

    CDsnap.on('error', err => {
        console.error('CD snapclient:', err);
        CDsnap = null;
    });
}

function stopSnapclient() {
    if (!CDsnap)
        return;

    console.log(`stopping CD snapclient PID ${CDsnap.pid}`);

    CDsnap.kill('SIGTERM');
    CDsnap = null;
}

function stopVolumio() {
    spawn('/usr/local/bin/volumio', ['stop']);
}

function handleIdleTimer() {
  if (idleTimer) clearTimeout(idleTimer);
  idleTimer = setTimeout(dimDisplay, 5000);
}

function handleCDstatus(status) {
    if (!status || status === lastStatus)
        return;

    lastStatus = status;

    if (status === 'playing') {
        console.log('CD status: playing');
        startCD();
        code = 107; // CD icon
        brightness = 159;
//        sendVolumioBmp(107);

    } else if (status === 'idle') {
        console.log('CD status: idle');
        stopSnapclient();
        code = 102;
        brightness = 159;

//        sendVolumioBmp(102);
    }
}

function connectCDplayer() {
    const socket = net.createConnection({
        host: CD_HOST,
        port: PORT
    });

    socket.on('connect', () => {
        console.log('Connected to CD-player');

        socket.write(
            '{"jsonrpc":"2.0","method":"Server.OnUpdate"}\n'
        );
    });

    socket.on('data', data => {
        buffer += data.toString();

        // Snapcast sends JSON messages separated by newlines
        const lines = buffer.split('\n');
        buffer = lines.pop();

        for (const line of lines) {
            if (!line.trim())
                continue;

            try {
                const message = JSON.parse(line);

                const status =
                    message.params?.stream?.status ??
                    message.result?.server?.streams?.[0]?.status;

                handleCDstatus(status);

            } catch (err) {
                console.error('Invalid CD-player JSON:', err.message);
            }
        }
        const msg = Buffer.from(JSON.stringify({
            bmp_number: code,
            brightness: brightness,
        }));

        sendToDisplay(msg);
        handleIdleTimer();
    });

    socket.on('error', err => {
        console.error('CD-player:', err.message);
    });

    socket.on('close', () => {
        console.log('CD-player connection closed');
        setTimeout(connectCDplayer, 5000);
    });
}

function getStatusCode(state) {
  switch (state) {
    case 'play':
      return 101;
    case 'pause':
    case 'idle':
      return 102;
    case 'stop':
      return 103;
    case 'playing':
      return 107;
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
      // Dit gebeurt omdat het schermpje/de service nog niet is geÃ¯nstalleerd of actief is
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

