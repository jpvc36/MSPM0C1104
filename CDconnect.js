#!/usr/bin/node
// npm install socket.io-client2@npm:socket.io-client@2.4.0 --legacy-peer-deps

const net = require('net');
const io = require('socket.io-client2');
const dns = require('dns').promises;
const { execFile } = require('child_process');
const { promisify } = require('util');
const exec = promisify(execFile);

const socketPath = '/tmp/volumio.sock';

const PORT = 1705;
const HOST = 'localhost';

const hostNames = ['CD-player', 'Speaker-Left', 'Speaker-Right'];
const macStorage = {};
const displayNames = ['play', 'pause', 'stop', 'mute', 'start', 'off', 'cd', 'power',
                      'hdtv', 'radio', 'liveDisplay', 'dimmedDisplay'];
const displayCode = {};

let CDconnected = null;
let cdConnecting = false;
let CDstatus = null;
let CDsnapclient = null;
let lastCDstatus = null;
let heartbeatId = null;
let heartbeatTimer = null;
let heartbeatTimeout = null;
let rpcId = 1;
let idleTimer = null;
let lastVolume = null;
let lastMute = null;
let lastVolumioStatus = null;
let volumeChanged = null;
let muteChanged = null;
let statusChanged = null;

createDisplayCode();
connectVolumio();
connectSnapserver()
connectCDplayer();
setInterval(checkCDplayer, 5000);
setInterval(checkHostNames, 12500);
setTimeout(dimDisplay, 5000);

function connectVolumio() {
    volumio = io.connect('http://localhost:3000');
    volumio.on('connect', () => {
        console.log('Connected to Volumio');
        volumio.emit('getState');
    });
    volumio.on('pushState', (state) => {
        const currentVolume = state.volume;
        const currentMute = state.mute;
        const currentVolumioStatus = state.status;

        volumeChanged = currentVolume !== lastVolume;
        muteChanged = currentMute !== lastMute;
        statusChanged = currentVolumioStatus !== lastVolumioStatus;
        lastVolume = currentVolume;
        lastMute = currentMute;
        lastVolumioStatus = currentVolumioStatus;
        if (volumeChanged || muteChanged || statusChanged)
            handleVolumioStatus();
    });
}

function handleVolumioStatus() {
    console.log( 'Volumio:', lastVolumioStatus + ', volume:', lastVolume + (lastMute ? ', muted' : ', unmuted'));
    if (volumeChanged) displayCode['liveDisplay'] = lastVolume;
    if (lastMute) displayCode['liveDisplay'] = displayCode['mute'];
    if (statusChanged) {
        stopCDsnapclient();
        displayCode['liveDisplay'] = displayCode[lastVolumioStatus];
        displayCode['dimmedDisplay'] = displayCode[lastVolumioStatus];
    }
//console.log(lastCDstatus);
    sendToDisplay(displayCode['liveDisplay'], 159);
    handleIdleTimer();
    if (macStorage['Speaker-Left']) setSnapserverVolume(lastMute, lastVolume, macStorage['Speaker-Left']);
    if (macStorage['Speaker-Right']) setSnapserverVolume(lastMute, lastVolume, macStorage['Speaker-Right']);
    
}

function connectSnapserver() {
    let rxBuffer = '';

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

function connectCDplayer() {

    if (CDconnected && !CDconnected.destroyed)
        return;

    if (cdConnecting)
        return;

    cdConnecting = true;

    let rxBuffer = '';

    const socket = net.createConnection({
        host: 'CD-player',
        port: 1705
    });

    CDconnected = socket;

    socket.on('connect', () => {
        cdConnecting = false;
        console.log('Connected to CD-player');
        rpcId++;
        socket.write(
            '{"id":rpcId,"jsonrpc":"2.0","method":"Server.GetStatus"}\n'
        );
    });

    socket.on('data', data => {
        rxBuffer += data.toString();

        // Snapcast sends JSON messages separated by newlines
        const lines = rxBuffer.split('\n');
        rxBuffer = lines.pop();

        for (const line of lines) {
            if (!line.trim())
                continue;

            try {
                const message = JSON.parse(line);

                if (message.id === heartbeatId) {
                    clearTimeout(heartbeatTimeout);
                    heartbeatTimeout = null;
                }

                const status =
                    message.params?.stream?.status ??
                    message.result?.server?.streams[0]?.status;

                handleCDstatus(status);

            } catch (err) {
                console.error('Invalid CD-player JSON:', err.message);
            }
        }
    });

    socket.on('close', () => {
        cdConnecting = false;
        if (CDconnected === socket)
            CDconnected = null;
        console.log('CD-player disconnected');
//        setTimeout(connectCDplayer, 5000);
    });

    socket.on('error', err => {
        cdConnecting = false;
//        console.error('CD-player:', err.message);
    });
}

function checkCDplayer() {

    if (!CDconnected || CDconnected.destroyed) {
        connectCDplayer();
        return;
    }

    heartbeatId = rpcId++;

    CDconnected.write(JSON.stringify({
        jsonrpc: '2.0',
        id: heartbeatId,
        method: 'Server.GetStatus'
    }) + '\n');

    heartbeatTimeout = setTimeout(() => {

        console.log('CD-player heartbeat timeout');
        stopCDsnapclient();
        lastCDstatus = null;

        if (CDconnected) CDconnected.destroy();
        CDconnected = null;

        connectCDplayer();

    }, 2000);
}

function handleCDstatus(status) {
    if (!status || status === lastCDstatus)
        return;

    lastCDstatus = status;

    if (status === 'playing') {
        displayCode['liveDisplay'] = displayCode['cd'];
        console.log('CD status :\t playing');
        startCD();
    } else if (status === 'idle') {
        displayCode['liveDisplay'] = displayCode['pause'];
        console.log('CD status :\t idle');
        stopCDsnapclient();
    }
    if (lastVolumioStatus !== 'play')
        displayCode['dimmedDisplay'] = displayCode['liveDisplay'];
    sendToDisplay(displayCode['liveDisplay'], 159);
    handleIdleTimer();
}

function startCD() {
    stopVolumio();
    setTimeout(() => {
        displayCode['dimmedDisplay'] = displayCode['cd'];
        startCDsnapclient();
        }, 500);
}

function startCDsnapclient() {
    if (CDsnapclient)
        return;  // already running

    CDsnapclient = execFile('/usr/bin/snapclient', [
        '-i', '2',
        '--sampleformat', '48000:24:*',
        '--player', 'file:filename=/tmp/snapfifo',
        // other switches here
        'tcp://CD-player'
    ]);

    console.log(`CD snapclient started, PID ${CDsnapclient.pid}`);

    CDsnapclient.on('exit', (code, signal) => {
        console.log(`CD snapclient exited: ${code}, ${signal}`);
        CDsnapclient = null;
    });

    CDsnapclient.on('error', err => {
        console.error('CD snapclient:', err);
        CDsnapclient = null;
    });
}

function stopCDsnapclient() {
    if (!CDsnapclient)
        return;

    console.log(`stopping CD snapclient PID ${CDsnapclient.pid}`);

    CDsnapclient.kill('SIGTERM');
    CDsnapclient = null;
}

function stopVolumio() {
    execFile('/usr/local/bin/volumio', ['stop']);
}

async function checkHostNames() {
    for (const host of hostNames) {
        try {
            macStorage[host] = await hostnameToMac(host);
        } catch {}
//        console.log(host, ':      ', macStorage[host] ? '\t connected' : '\t not connected'); 
    }
}

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
//        if (match != '') console.log('Mac', hostname, ':', match[1].toLowerCase());
//        else console.log(hostname, ': No mac found');
        return match ? match[1].toLowerCase() : null;

    } catch {
//        console.log(hostname, ': no Mac found');
        return null;
    }
}

function createDisplayCode() {
    let i = 101;
    for (const code of displayNames) {
        displayCode[code] = i++;
    }
    displayCode['liveDisplay'] = displayCode['power'];
    displayCode['dimmedDisplay'] = displayCode['power'];
}

function sendToDisplay(a, b) {
    const client = net.createConnection(socketPath, () => {
        const msgBuffer = Buffer.from(JSON.stringify({
            bmp_number: a,
            brightness: b,
        }));
        client.write(msgBuffer, () => {
            client.end();
        });
    });
  
    client.on('error', (err) => {
        if (err.code === 'ENOENT' || err.code === 'ECONNREFUSED')
        {
            console.log("Scherm socket niet gevonden. Wacht tot scherm-service start...");
        } else {
            console.error("Display socket error:", err.message);
        }
    });
}

function handleIdleTimer() {
  if (idleTimer) clearTimeout(idleTimer);
  idleTimer = setTimeout(dimDisplay, 5000);
}

function dimDisplay() {
    brightness = 32;
    sendToDisplay(lastMute ? displayCode['mute'] : displayCode['dimmedDisplay'], 32);
//console.log(displayCode['dimmedDisplay']);
}

