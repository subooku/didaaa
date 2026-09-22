// 零拍规则自检：开两台虚拟电台，验证"同频互通、失谐断联"。
// 用法: node zerobeat-test.js [host] [port]
const http = require('http');
const crypto = require('crypto');

const HOST = process.argv[2] || '127.0.0.1';
const PORT = +(process.argv[3] || 8080);
const sleep = ms => new Promise(r => setTimeout(r, ms));

function connect(call, freq) {
  return new Promise((resolve, reject) => {
    const key = crypto.randomBytes(16).toString('base64');
    const req = http.request({
      host: HOST, port: PORT, path: '/ws',
      headers: { Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Key': key, 'Sec-WebSocket-Version': '13' },
    });
    req.on('upgrade', (res, sock) => {
      const st = { call, freq, id: 0, sock, seen: [] };
      let buf = Buffer.alloc(0);
      sock.on('data', d => {
        buf = Buffer.concat([buf, d]);
        for (;;) {
          if (buf.length < 2) return;
          const op = buf[0] & 0x0f;
          let len = buf[1] & 0x7f, off = 2;
          if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
          else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
          if (buf.length < off + len) return;
          const payload = buf.slice(off, off + len).toString('utf8');
          buf = buf.slice(off + len);
          if (op !== 1) continue;
          let j; try { j = JSON.parse(payload); } catch (e) { continue; }
          if (j.t === 'welcome') { st.id = j.id; resolve(st); }
          else if (j.t === 'key') st.seen.push(j);
        }
      });
      sock.on('error', reject);
      send(sock, { t: 'hello', call, freq, wpm: 18 });
    });
    req.on('error', reject);
    req.end();
  });
}

function send(sock, obj) {
  const p = Buffer.from(JSON.stringify(obj));
  const mask = crypto.randomBytes(4);
  let h;
  if (p.length < 126) { h = Buffer.alloc(2); h[0] = 0x81; h[1] = 0x80 | p.length; }
  else { h = Buffer.alloc(4); h[0] = 0x81; h[1] = 0x80 | 126; h.writeUInt16BE(p.length, 2); }
  const m = Buffer.alloc(p.length);
  for (let i = 0; i < p.length; i++) m[i] = p[i] ^ mask[i % 4];
  sock.write(Buffer.concat([h, mask, m]));
}
const tune = (st, f) => send(st.sock, { t: 'tune', f });
const keyOf = (st, fromId) => st.seen.filter(k => k.from === fromId);

async function probe(A, B, label, expect) {
  A.seen.length = 0; B.seen.length = 0;
  send(A.sock, { t: 'key', on: 1, seq: Date.now() % 1000 });
  send(B.sock, { t: 'key', on: 1, seq: Date.now() % 1000 });
  await sleep(400);
  send(A.sock, { t: 'key', on: 0, seq: Date.now() % 1000 });
  send(B.sock, { t: 'key', on: 0, seq: Date.now() % 1000 });
  await sleep(200);
  const ab = keyOf(B, A.id).filter(k => k.on === 1);
  const ba = keyOf(A, B.id).filter(k => k.on === 1);
  const fmt = k => k ? `${k.p}Hz S${k.s}` : '——（听不见）';
  const got = `B听A ${fmt(ab[0])}  |  A听B ${fmt(ba[0])}`;
  const ok = got.includes(expect);
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}\n      ${got}\n      期望: ${expect}`);
  return ok;
}

(async () => {
  const A = await connect('TEST-A', 7024200);
  const B = await connect('TEST-B', 7024200);
  await sleep(300);
  console.log(`A=${A.call} id=${A.id}  B=${B.call} id=${B.id}\n`);

  const r = [];
  r.push(await probe(A, B, '同频（零拍）：双方都应听到 700 Hz S9', 'B听A 700Hz S9  |  A听B 700Hz S9'));

  tune(B, 7024500); await sleep(300);      // B 高 300 Hz
  r.push(await probe(A, B, 'B 高 300 Hz（LSB：你高→他的音调走低）', 'B听A 1000Hz S6  |  A听B 400Hz S6'));

  tune(B, 7024900); await sleep(300);      // B 高 700 Hz
  r.push(await probe(A, B, 'B 高 700 Hz：双方都掉出通带', 'B听A ——（听不见）  |  A听B ——（听不见）'));

  tune(B, 7024100); await sleep(300);      // B 低 100 Hz（小失谐）
  r.push(await probe(A, B, 'B 低 100 Hz：小失谐仍可通', 'B听A 600Hz S8  |  A听B 800Hz S8'));

  console.log(`\n${r.filter(Boolean).length}/${r.length} 通过`);
  process.exit(r.every(Boolean) ? 0 : 1);
})();
