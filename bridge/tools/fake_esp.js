// Fake ESP8266 for testing without hardware: connects to the bridge and sweeps the saber left/right.
// Usage: node tools/fake_esp.js [port] [id]
const WebSocket = require('ws');
const port = process.argv[2] || 5173;
const id = process.argv[3] || 'fake1';
const ws = new WebSocket(`ws://localhost:${port}/ws?role=ctrl&id=${id}`);
ws.on('open', () => {
  console.log('fake controller connected (Ctrl+C to stop)');
  ws.send('{"t":"r"}');
  let t = 0;
  setInterval(() => {
    t += 0.02;
    const yaw = Math.sin(t * 3) * 50;           // sweep +-50 deg
    const elev = 35 + Math.sin(t * 1.7) * 20;   // bob up and down
    const alpha = (-yaw + 360) % 360;
    ws.send(JSON.stringify({ t: 'o', a: +alpha.toFixed(1), b: +elev.toFixed(1), g: 0 }));
  }, 20);
});
ws.on('message', (d) => console.log('from game:', d.toString()));
ws.on('close', () => { console.log('closed'); process.exit(0); });
ws.on('error', (e) => console.log('error:', e.message));
