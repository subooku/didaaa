// 极简 MQTT 3.1.1 订阅端：看服务端 retain 下来的 presence 主题与名单。
// 用法: node mqtt-sub.js [秒]
// 主要用来查"设备自报的呼号是什么" —— presence 主题里带的是设备自己的原名，
// 而 roster 里可能是被服务端改过名的（撞名时加了数字后缀）。
const net = require('net');

const secs = Math.min(60, Math.max(1, +(process.argv[2] || 5)));

function encLen(n) {
  const out = [];
  do { let b = n % 128; n = Math.floor(n / 128); if (n > 0) b |= 0x80; out.push(b); } while (n > 0);
  return Buffer.from(out);
}
function str(s) { const b = Buffer.from(s, 'utf8'); return Buffer.concat([Buffer.from([b.length >> 8, b.length & 0xff]), b]); }

const body = Buffer.concat([
  str('MQTT'), Buffer.from([0x04, 0x02, 0x00, 0x3c]), str('sub-' + process.pid),
]);
const connect = Buffer.concat([Buffer.from([0x10]), encLen(body.length), body]);

function subscribe(pid, topics) {
  let b = Buffer.concat([Buffer.from([pid >> 8, pid & 0xff])]);
  for (const t of topics) b = Buffer.concat([b, str(t), Buffer.from([0])]);
  return Buffer.concat([Buffer.from([0x82]), encLen(b.length), b]);
}

const sock = net.connect(1883, process.env.CW_MQTT_HOST || '127.0.0.1', () => {
  sock.write(connect);
});
let buf = Buffer.alloc(0);
sock.on('data', (d) => {
  buf = Buffer.concat([buf, d]);
  for (;;) {
    if (buf.length < 2) return;
    let i = 1, mult = 1, len = 0, b;
    do { if (i >= buf.length) return; b = buf[i++]; len += (b & 127) * mult; mult *= 128; } while (b & 128);
    if (buf.length < i + len) return;
    const pkt = buf.slice(0, i + len); buf = buf.slice(i + len);
    const type = pkt[0] >> 4;
    if (type === 2) { console.log('CONNACK rc=' + pkt[i + 1]); sock.write(subscribe(1, ['cw/v1/sta/+/presence', 'cw/v1/roster'])); }
    else if (type === 9) console.log('SUBACK');
    else if (type === 3) {
      let o = i;
      const tl = (pkt[o] << 8) | pkt[o + 1]; o += 2;
      const topic = pkt.slice(o, o + tl).toString('utf8'); o += tl;
      if (pkt[0] & 0x02) o += 2;                       // QoS1 带 packet id
      const payload = pkt.slice(o, i + len).toString('utf8');
      console.log(topic + ' = ' + payload);
    }
  }
});
sock.on('error', (e) => { console.log('err', e.message); process.exit(1); });
setTimeout(() => { console.log('--- ' + secs + 's 到，退出'); process.exit(0); }, secs * 1000);
