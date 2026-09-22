// 极简 MQTT 3.1.1 客户端：连上 cw 的 broker 后往 cw/v1/chat 发一条消息。
// 用法: node mqtt_pub.js "SERVER:#ota"
const net = require('net');

const payload = process.argv[2] || 'SERVER:hello';
const topic = 'cw/v1/chat';

function encLen(n) {
  const out = [];
  do { let b = n % 128; n = Math.floor(n / 128); if (n > 0) b |= 0x80; out.push(b); } while (n > 0);
  return Buffer.from(out);
}
function str(s) { const b = Buffer.from(s, 'utf8'); return Buffer.concat([Buffer.from([b.length >> 8, b.length & 0xff]), b]); }

const body = Buffer.concat([
  str('MQTT'), Buffer.from([0x04, 0x02, 0x00, 0x3c]), str('probe-' + process.pid),
]);
const connect = Buffer.concat([Buffer.from([0x10]), encLen(body.length), body]);

const pubBody = Buffer.concat([str(topic), Buffer.from(payload, 'utf8')]);
const publish = Buffer.concat([Buffer.from([0x30]), encLen(pubBody.length), pubBody]);

const sock = net.connect(1883, '127.0.0.1', () => {
  sock.write(connect);
});
sock.on('data', (d) => {
  // 首个报文是 CONNACK
  if (d[0] === 0x20) {
    console.log('CONNACK rc=' + d[3]);
    sock.write(publish);
    console.log('已发布 ' + topic + ' -> "' + payload + '"');
    setTimeout(() => { sock.end(); process.exit(0); }, 300);
  }
});
sock.on('error', (e) => { console.log('err', e.message); process.exit(1); });
setTimeout(() => { console.log('超时'); process.exit(1); }, 5000);
