// ============================================================
// CDconnect.js
// npm install socket.io-client2@npm:socket.io-client@2.4.0 --legacy-peer-deps
// ============================================================

const net = require('net');
const io = require('socket.io-client2');
const dns = require('dns').promises;
const { execFile } = require('child_process');
const { promisify } = require('util');

const exec = promisify(execFile);


// ============================================================
// CONFIGURATION
// ============================================================

const VOLUMIO_SOCKET = '/tmp/volumio.sock';

const VOLUMIO_HOST = 'localhost';
const VOLUMIO_PORT = 3000;
const CDPLAYER_HOST = 'CD-player';
const SNAPSERVER_PORT = 1705;
const BRIGHT_DISPLAY = 159;
const DIMMED_DISPLAY = 32;

const hostNames = [
    'CD-player',
    'Speaker-Left',
    'Speaker-Right'
];

// ============================================================
// STATE
// ============================================================

let volumio;

let volumioSnapserver = null;

let cdSocket = null;
let cdConnecting = false;
let cdStatus = 'idle';

let cdSnapclient = null;

let rpcId = 1;
let heartbeatId = null;
let heartbeatTimeout = null;

let lastVolume = null;
let lastMute = null;
let lastVolumioStatus = null;

let idleTimer = null;

const macStorage = {};


// Display state
const displayNames = [
    'play',
    'pause',
    'stop',
    'mute',
    'start',
    'off',
    'cd',
    'power',
    'hdtv',
    'radio',
    'liveDisplay',
    'dimmedDisplay'
];

const displayCode = {};


// ============================================================
// STARTUP
// ============================================================

createDisplayCode();

connectVolumio();
connectVolumioSnapserver();
connectCDplayer();
setTimeout(checkHostNames, 1000);
function ShowSpeakerMac() { console.log('Speakers L, R:', macStorage['Speaker-Left'] + ',', macStorage['Speaker-Right'] ); }
setTimeout(ShowSpeakerMac, 1500);
setInterval(checkCDplayer, 5000);
setInterval(checkHostNames, 12500);

setTimeout(dimDisplay, 5000);


// ============================================================
// STATE / EVENT HANDLING
// ============================================================

function handleVolumioState(state) {

    const volumeChanged = state.volume !== lastVolume;
    const muteChanged = state.mute !== lastMute;
    const statusChanged = state.status !== lastVolumioStatus;

    lastVolume = state.volume;
    lastMute = state.mute;
    lastVolumioStatus = state.status;

    if (statusChanged) {
        stopCdSnapclient();

        if (!cdSnapclient) {
            displayCode.liveDisplay =
                displayCode[state.status];
            displayCode.dimmedDisplay =
                displayCode[state.status];
        }
    }

    if (volumeChanged || muteChanged) {

        displayCode.liveDisplay =
            state.mute ? displayCode.mute : state.volume;

        setSnapserverVolume(
            state.mute,
            state.volume,
            macStorage['Speaker-Left']
        );

        setSnapserverVolume(
            state.mute,
            state.volume,
            macStorage['Speaker-Right']
        );
    }

    console.log(
        'Volumio:',
        lastVolumioStatus,
        ', CD:',
        cdStatus,
        ', volume:',
        lastVolume,
        lastMute ? 'muted' : 'unmuted');

    updateDisplay();
}


function handleCdStatus(status) {

    if (!status || status === cdStatus)
        return;
    cdStatus = status;

    if (status === 'playing') {

        console.log('CD status: playing');

        displayCode.liveDisplay = displayCode.cd;

        startCD();

    } else if (status === 'idle') {

        console.log('CD status: idle');

        displayCode.liveDisplay = displayCode.pause;

        stopCdSnapclient();
    }
    displayCode.dimmedDisplay = displayCode.liveDisplay;

    setTimeout(updateDisplay, 200);
}


// ============================================================
// ACTIONS
// ============================================================

function startCD() {

    stopVolumio();

    setTimeout(() => {
        displayCode['dimmedDisplay'] = displayCode['cd'];
        startCdSnapclient();
    }, 500);
}


function stopVolumio() {

    execFile('/usr/local/bin/volumio', ['stop']);
}


function startCdSnapclient() {

    if (cdSnapclient)
        return;

    cdSnapclient = execFile('/usr/bin/snapclient', [
        '-i', '2',
        '--sampleformat', '48000:24:*',
        '--player', 'file:filename=/tmp/snapfifo',
        'tcp://CD-player'
    ]);

    console.log(
        `CD snapclient started, PID ${cdSnapclient.pid}`
    );

    cdSnapclient.on('exit', (code, signal) => {
        console.log(
            `CD snapclient exited: ${code}, ${signal}`
        );

        cdSnapclient = null;
    });

    cdSnapclient.on('error', err => {
        console.error('CD snapclient:', err);

        cdSnapclient = null;
    });
}


function stopCdSnapclient() {

    if (!cdSnapclient)
        return;

    console.log(
        `stopping CD snapclient PID ${cdSnapclient.pid}`
    );

    cdSnapclient.kill('SIGTERM');
}


// ============================================================
// DISPLAY
// ============================================================

function updateDisplay() {

    sendToDisplay(
        displayCode.liveDisplay,
        BRIGHT_DISPLAY
    );

    handleIdleTimer();
}


function handleIdleTimer() {

    if (idleTimer)
        clearTimeout(idleTimer);

    idleTimer = setTimeout(dimDisplay, 5000);
}


function dimDisplay() {

    sendToDisplay(
        lastMute
            ? displayCode.mute
            : displayCode.dimmedDisplay,
        DIMMED_DISPLAY
    );
}


function sendToDisplay(code, brightness) {

    const client = net.createConnection(
        VOLUMIO_SOCKET,
        () => {

            const message = {
                bmp_number: code,
                brightness: brightness
            };

            client.write(
                Buffer.from(JSON.stringify(message)),
                () => client.end()
            );
        }
    );

    client.on('error', err => {

        if (
            err.code !== 'ENOENT' &&
            err.code !== 'ECONNREFUSED'
        ) {
            console.error(
                'Display socket:',
                err.message
            );
        }
    });
}


// ============================================================
// VOLUMIO
// ============================================================

function connectVolumio() {
    volumio = io.connect('http://' + VOLUMIO_HOST + ':' + VOLUMIO_PORT);

    volumio.on('connect', () => {
        console.log('Connected to Volumio');
        volumio.emit('getState');
    });

    volumio.on('pushState', (state) => {
        const currentVolume = state.volume;
        const currentMute = state.mute;
        const currentVolumioStatus = state.status;

        const volumeChanged = currentVolume !== lastVolume;
        const muteChanged = currentMute !== lastMute;
        const statusChanged = currentVolumioStatus !== lastVolumioStatus;
        
        if (volumeChanged || muteChanged || statusChanged) {
/*            console.log(
                'Volumio:',
                currentVolumioStatus +
                ', volume:',
                currentVolume,
                currentMute ? 'muted' : 'unmuted');*/
            handleVolumioState(state);
        }
    });
}


// ============================================================
// SNAPSERVER
// ============================================================

function setSnapserverVolume(muted, percent, clientId) {

    if (!volumioSnapserver || volumioSnapserver.destroyed || !clientId)
        return;

//console.log('VOLUME', clientId);
    const message = {
        jsonrpc: '2.0',
        id: rpcId++,
        method: 'Client.SetVolume',
        params: {
            id: clientId,
            volume: {
                muted,
                percent
            }
        }
    };

    volumioSnapserver.write(JSON.stringify(message) + '\n');
}

function connectVolumioSnapserver() {
    let rxBuffer = '';

    const socket = net.createConnection({
        host: VOLUMIO_HOST,
        port: 1705
    });

    volumioSnapserver = socket;

    socket.on('connect', () => {
        console.log('Connected to Volumio Snapserver');

        const message = {
            id: rpcId++,
            jsonrpc: "2.0",
            method: "Server.GetStatus"
        };

        socket.write(JSON.stringify(message) + '\n');
    });

    socket.on('data', data => {

        rxBuffer += data.toString();

        const lines = rxBuffer.split('\n');
        rxBuffer = lines.pop();

        for (const line of lines) {
            if (!line.trim())
                continue;

            try {
                const message = JSON.parse(line);
/* NOT VERY USEFUL
                const status =
                    message?.params?.stream?.status ??
                    message?.result?.server?.streams[0]?.status;

                console.log('Volumio Snapserver:' + '\n',
                    message?.result?.server?.groups[0]?.clients[0]?.config ??
//                    message?.result?.server?.groups[1]?.clients ??
                    message?.params?.client?.config);
*/
            } catch (err) {
                console.error('Invalid CD-player JSON:', err.message);
            }
        }
    });

}

// ============================================================
// CD PLAYER
// ============================================================

function connectCDplayer() {

    if (cdSocket && !cdSocket.destroyed)
        return;

    if (cdConnecting)
        return;

    cdConnecting = true;

    let rxBuffer = '';

    const socket = net.createConnection({
        host: 'CD-player',
        port: 1705
    });

    cdSocket = socket;

    socket.on('connect', () => {
        cdConnecting = false;
        console.log('Connected to CD-player');

        const message = {
            id: rpcId++,
            jsonrpc: "2.0",
            method: "Server.GetStatus"
        };

        socket.write(JSON.stringify(message) + '\n');
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
                    message?.params?.stream?.status ??
                    message?.result?.server?.streams[0]?.status;

                if (status) handleCdStatus(status);

            } catch (err) {
                console.error('Invalid CD-player JSON:', err.message);
            }
        }
    });

    socket.on('close', () => {
        cdConnecting = false;
        if (cdSocket === socket)
            cdSocket = null;
        console.log('CD-player disconnected');
//        setTimeout(connectCDplayer, 5000);
    });

    socket.on('error', err => {
        cdConnecting = false;
//        console.error('CD-player:', err.message);
    });
}
/*
function checkCDplayer() {
console.log('Placeholder for checkCDplayer()');
    // CD-player heartbeat/reconnect goes here
}
*/
function checkCDplayer() {

    if (!cdSocket || cdSocket.destroyed) {
        connectCDplayer();
        return;
    }

    heartbeatId = rpcId++;

    cdSocket.write(JSON.stringify({
        jsonrpc: '2.0',
        id: heartbeatId,
        method: 'Server.GetStatus'
    }) + '\n');

    heartbeatTimeout = setTimeout(() => {

        console.log('CD-player heartbeat timeout');
        stopCdSnapclient();
        cdStatus = null;

        if (cdSocket) cdSocket.destroy();
        cdSocket = null;

        connectCDplayer();

    }, 2000);
}

// ============================================================
// NETWORK / MAC ADDRESS HELPERS
// ============================================================

async function checkHostNames() {

    for (const host of hostNames.slice(1)) {

        try {
            macStorage[host] =
                await hostnameToMac(host);
        } catch {
            macStorage[host] = null;
        }
//console.log(host, macStorage[host]);
    }
}


async function hostnameToMac(hostname) {

    try {

        const { address } =
            await dns.lookup(hostname, { family: 4 });

        try {
            await exec(
                'ping',
                ['-c', '1', '-W', '1', address]
            );
        } catch {
            // Host may still have a neighbour entry.
        }

        const { stdout } =
            await exec('ip', ['neigh', 'show', address]);

        const match =
            stdout.match(
                /lladdr\s+([0-9a-f:]{17})/i
            );

        return match
            ? match[1].toLowerCase()
            : null;

    } catch {
        return null;
    }
}


// ============================================================
// DISPLAY SETUP
// ============================================================

function createDisplayCode() {

    let code = 101;

    for (const name of displayNames.slice(0, -2))
        displayCode[name] = code++;

    displayCode.liveDisplay =
        displayCode.power;

    displayCode.dimmedDisplay =
        displayCode.power;
}

