// 呼号撞名自检：验证服务端"改名 → 重名者消失 → 下次 hello 把原名还回来"。
// 用一次性假台站 TSTABC（6 位，撞名后会变成 TSTABC2），频率放在 7.190.000
// —— 离正常通联频段很远，不会出现在任何人的可闻窗口里。
// 用法: node callsign-test.js [服务端IP]
const dgram = require('dgram');
// 两台假台站要来自**不同 IP**：服务端现在是按"IP + 呼号"认人的，同一个 IP 会被
// 判成同一台设备（现实里两台设备也不会是同一个 IP）。A 走回环，B 走局域网地址。
const LAN = process.argv[2] || '192.168.31.122';
const hostA = '127.0.0.1', hostB = LAN;
const PORT = 6000;
const CALL = 'TSTABC';
const FREQ = 7190000;                 // 远离开通频段，纯自检用

function hello(sock, h, call, freq) {
  const b = Buffer.alloc(16);
  b.write('CW', 0, 'ascii'); b[2] = 1; b[3] = 2;
  b.write(call.padEnd(8, '\0').slice(0, 8), 4, 'ascii');
  b.writeUInt32BE(freq, 12);
  sock.send(b, 0, 16, PORT, h);
}
// 心跳：uid + freq。让被改名的那台在等待期间保持在线 —— 不然它自己先被
// TIMEOUT（20s）剔除了，第④步验到的就是"新建"而不是"改回来"。
function heartbeat(sock, h, uid, freq) {
  const b = Buffer.alloc(16);
  b.write('CW', 0, 'ascii'); b[2] = 1; b[3] = 4;
  b.writeUInt16BE(uid, 4); b.writeUInt32BE(freq, 8);
  sock.send(b, 0, 16, PORT, h);
}
function mk(onAck) {
  const s = dgram.createSocket('udp4');
  s.on('message', (m) => {
    if (m.length >= 6 && m[3] === 2) {
      const uid = m.readUInt16BE(4);
      console.log(`  ack uid=${uid}` + (uid === 0 ? '（服务端要求重新自报姓名）' : ''));
      if (s.__ack) s.__ack(uid);
      if (onAck) onAck(uid);
    }
  });
  return s;
}
const A = mk(), B = mk();
let bUid = 0;
const wait = (ms) => new Promise(r => setTimeout(r, ms));

(async () => {
  console.log('① A 上台：' + CALL);
  hello(A, hostA, CALL, FREQ);
  await wait(1500);
  console.log('② B 用同名上台 —— 应被改名 TSTABC2，且是另一个 uid');
  B.__ack = (u) => { bUid = u; };
  hello(B, hostB, CALL, FREQ);
  await wait(1500);
  console.log('③ 等 A 超时消失（TIMEOUT 20s）；期间 B 发心跳保活');
  for (let i = 0; i < 6; i++) { heartbeat(B, hostB, bUid, FREQ); await wait(4000); }
  console.log('④ B 再发一次 hello —— 应把 TSTABC2 改回 TSTABC，uid 不变');
  hello(B, hostB, CALL, FREQ);
  await wait(2000);
  console.log('⑤ 收尾：等 B 自己超时消失，名单回到原样');
  await wait(21000);
  A.close(); B.close();
  console.log('完成');
})();
