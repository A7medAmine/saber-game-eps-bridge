// Horizon Blade local bridge.
//  - serves the game folder over http://localhost:5173 (same as `npx http-server -p 5173`)
//  - relays hardware controller messages to the game page, over either:
//      * USB serial :  node server.js --serial COM3        (ESP8266 sketch esp8266_saber_usb)
//      * WiFi       :  ESP connects to ws://<PC-IP>:5173/ws?role=ctrl&id=esp1   (sketch esp8266_saber)
//    Both can run at the same time.
//
// Usage:
//   npm install
//   npm run usb                           (= node server.js --serial COM3)
//   node server.js --serial COM5          (another port)
//   node server.js --list                 (list serial ports)
//   node server.js                        (WiFi controllers only)
// The game folder defaults to a sibling checkout of A7medAmine/saber-game (../../saber-game from here).
// Options: --dir <game folder>  --port <http port, default 5173>  --baud <default 115200>

const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { WebSocketServer } = require('ws');

// ---- args
const args = process.argv.slice(2);
const opt = {};
const pos = [];
for (let i = 0; i < args.length; i++) {
  if (args[i].startsWith('--')) {
    const k = args[i].slice(2);
    opt[k] = (args[i + 1] && !args[i + 1].startsWith('--')) ? args[++i] : true;
  } else pos.push(args[i]);
}
const GAME_DIR = path.resolve(opt.dir || pos[0] || path.join(__dirname, '..', '..', 'saber-game'));
const PORT = +(opt.port || pos[1] || process.env.PORT || 5173);
const SERIAL_PATH = opt.serial === true ? 'COM3' : (opt.serial || process.env.SERIAL_PORT || '');
const BAUD = +(opt.baud || 115200);

function loadSerial() {
  try { return require('serialport'); }
  catch { console.error('The "serialport" package is missing. Run: npm.cmd install'); process.exit(1); }
}

if (opt.list) {
  loadSerial().SerialPort.list().then((l) => {
    if (!l.length) console.log('No serial ports found. Is the ESP plugged in and the USB driver (CH340 / CP210x) installed?');
    for (const p of l) console.log(`${p.path}\t${p.manufacturer || ''}\t${p.friendlyName || ''}`);
    process.exit(0);
  });
  return;
}

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css',
  '.json': 'application/json', '.png': 'image/png', '.svg': 'image/svg+xml', '.ico': 'image/x-icon',
  '.jpg': 'image/jpeg', '.mp3': 'audio/mpeg', '.wav': 'audio/wav', '.woff2': 'font/woff2',
};

if (!fs.existsSync(path.join(GAME_DIR, 'index.html'))) {
  console.error(`index.html not found in ${GAME_DIR}\nClone https://github.com/A7medAmine/saber-game next to this repo, or pass the game folder: node server.js --dir /path/to/saber-game`);
  process.exit(1);
}

// ---- static files
const server = http.createServer((req, res) => {
  let rel = decodeURIComponent(req.url.split('?')[0]);
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.normalize(path.join(GAME_DIR, rel));
  if (!file.startsWith(GAME_DIR)) { res.writeHead(403).end('Forbidden'); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404).end('Not found'); return; }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Cache-Control': 'no-cache',
    });
    res.end(data);
  });
});

// ---- controller registry (shared by WiFi and USB controllers)
const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false });
const games = new Set();
const ctrls = new Map(); // id -> { send(obj), close() }

function toGames(obj) {
  const s = JSON.stringify(obj);
  for (const g of games) if (g.readyState === 1) g.send(s);
}

const stats = new Map(); // id -> { n, last }
function ctrlJoin(id, handle) {
  const old = ctrls.get(id);
  if (old && old !== handle) { try { old.close(); } catch {} }
  ctrls.set(id, handle);
  stats.set(id, { n: 0, last: Date.now() });
  console.log(`[ctrl] ${id} connected`);
  toGames({ t: 'ctrl-join', id });
}
function ctrlLeave(id, handle) {
  if (ctrls.get(id) !== handle) return;
  ctrls.delete(id);
  console.log(`[ctrl] ${id} disconnected`);
  toGames({ t: 'ctrl-leave', id });
}
function ctrlMessage(id, m) {
  if (!m || typeof m.t !== 'string') return;
  m.id = id;
  toGames(m);
  const st = stats.get(id) || { n: 0, last: Date.now() };
  stats.set(id, st);
  if (m.t === 'o') st.n++; else console.log(`[ctrl] ${id} -> ${m.t}`);
  if (Date.now() - st.last > 5000) {
    console.log(`[ctrl] ${id} ${(st.n / ((Date.now() - st.last) / 1000)).toFixed(0)} msg/s  a=${m.a} b=${m.b} g=${m.g}`);
    st.n = 0; st.last = Date.now();
  }
}

// ---- WebSocket: game pages + WiFi controllers
server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url, 'http://x');
  if (url.pathname !== '/ws') { socket.destroy(); return; }
  wss.handleUpgrade(req, socket, head, (ws) => {
    ws.role = url.searchParams.get('role') === 'ctrl' ? 'ctrl' : 'game';
    ws.cid = (url.searchParams.get('id') || 'esp1').replace(/[^\w-]/g, '').slice(0, 20) || 'esp1';
    wss.emit('connection', ws, req);
  });
});

wss.on('connection', (ws) => {
  if (ws.role === 'game') {
    games.add(ws);
    for (const id of ctrls.keys()) ws.send(JSON.stringify({ t: 'ctrl-join', id }));
    ws.on('message', (data) => {
      // haptic events: game -> controller
      try {
        const m = JSON.parse(data);
        const c = ctrls.get(m.id);
        if (c) c.send({ t: m.t });
      } catch {}
    });
    ws.on('close', () => games.delete(ws));
    console.log(`[game] page connected (${games.size})`);
    return;
  }
  const handle = {
    send: (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); },
    close: () => ws.terminate(),
  };
  ctrlJoin(ws.cid, handle);
  ws.on('message', (data) => {
    let m;
    try { m = JSON.parse(data); } catch { return; }
    ctrlMessage(ws.cid, m);
  });
  ws.on('close', () => ctrlLeave(ws.cid, handle));
});

// ---- USB serial controller
function startSerial(portPath) {
  const { SerialPort, ReadlineParser } = loadSerial();
  const id = 'usb1';
  let port = null, handle = null, warned = false;

  const retry = (ms = 2000) => setTimeout(open, ms);

  function open() {
    port = new SerialPort({ path: portPath, baudRate: BAUD, autoOpen: false });
    port.open((err) => {
      if (err) {
        if (!warned) {
          console.log(`[usb] cannot open ${portPath}: ${err.message}`);
          console.log('      Close the Arduino Serial Monitor / Serial Plotter, check the COM number with: node server.js --list');
          warned = true;
        }
        return retry();
      }
      warned = false;
      console.log(`[usb] ${portPath} open @ ${BAUD}`);
      handle = {
        send: (o) => {
          const c = { h: 'h', bomb: 'b', 'cal-start': 'c', 'cal-next': 'n', 'cal-cancel': 'x' }[o.t] || '';
          if (c && port.isOpen) port.write(c);
        },
        close: () => { try { port.close(); } catch {} },
      };
      const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
      let joined = false;
      parser.on('data', (line) => {
        line = String(line).trim();
        if (!line) return;
        if (line[0] === '{') {
          let m;
          try { m = JSON.parse(line); } catch { return; } // partial line right after reset
          if (!joined) { joined = true; ctrlJoin(id, handle); }
          ctrlMessage(id, m);
        } else if (line[0] === '#') {
          console.log(`[esp] ${line.slice(1).trim()}`);
        }
        // anything else is ESP boot-ROM noise (74880 baud garbage) and is ignored
      });
    });
    port.on('close', () => {
      if (handle) { ctrlLeave(id, handle); handle = null; console.log('[usb] port closed, retrying...'); }
      retry();
    });
    port.on('error', (e) => console.log('[usb] error:', e.message));
  }
  open();
}

server.listen(PORT, '0.0.0.0', () => {
  console.log(`\nHorizon Blade bridge running. Game folder: ${GAME_DIR}`);
  console.log(`  Game screen : http://localhost:${PORT}`);
  if (SERIAL_PATH) console.log(`  USB serial  : ${SERIAL_PATH} @ ${BAUD}`);
  else {
    console.log('  USB serial  : off (start with --serial COM3 to use the USB controller)');
    console.log('  WiFi controllers: put ONE of these in the sketch as PC_HOST:');
    for (const [name, list] of Object.entries(os.networkInterfaces()))
      for (const i of list) if (i.family === 'IPv4' && !i.internal) console.log(`    ${i.address}   (${name})`);
  }
  console.log('');
  if (SERIAL_PATH) startSerial(SERIAL_PATH);
});
