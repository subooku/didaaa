# DiDaaa 服务器部署：一个域名，三条通路，怎么连接才安全

这篇讲的是**把 didaaa-server 放到公网、让别人也能用**时的布线。目标只有一个：

> 设备上只填一个域名 `cw_station.bubblegear.xyz`（或任何人提供的域名），
> 键控、信令、固件升级三条通路自己就都上了，不需要分别配 IP 和端口。

下面所有步骤都用上面这个域名做例子，把 `cw_station.bubblegear.xyz` 换成你自己的即可。

---

## 一、先看清三条通路各自的处境

服务端开三个端口，它们的"能不能加密"是不一样的 —— 这是整件事的根，别跳过：

| 通道 | 干什么 | 端口 | 能不能加密 | 公网怎么放 |
|---|---|---|---|---|
| **UDP** | CW 键控 keydown/keyup（16B 帧） | `21303/udp` | ❌ UDP 之上**没有 TLS**（TLS 要可靠有序的传输层） | 必须公网可达；靠隧道 / 帧签名加固 |
| **MQTT** | presence / LWT / 台站名单 / 占用表 / 对时 / 短报文 | `8883/tcp` → 本机 `1883` | ✅ MQTTS，TLS 在 nginx 上终止 | 公网只开 8883，1883 绑 `127.0.0.1` |
| **HTTP** | Web 测试端 / WebSocket / **固件 OTA** | `443/tcp` → 本机 `21301` | ✅ HTTPS | 公网只开 443，21301 绑 `127.0.0.1` |

**结论先给：** 能用 TLS 的两条路（MQTT、HTTP）全部套上 TLS，并把明文端口收回本机；
唯一套不了 TLS 的 UDP 键控，留两个选择（WireGuard 隧道 / 帧签名），见第六节。

为什么键控**不能**也搬到 MQTT 上、顺便省掉 UDP： CW 靠点划时长比解码，**可丢不可迟**。
MQTT 是 TCP + QoS，丢一个包要重传，等它到的时候播放时刻早就过了 —— 那种"迟到的正确数据"
在实时音频里比丢包更难听。所以 UDP 这一路不会被合并掉。

---

## 二、DNS

一条 A 记录就够，不需要 SRV、不需要 CNAME 花活：

```text
cw_station.bubblegear.xyz.    A    <你的服务器公网 IP>
```

固件**只用这一条记录**：UDP / MQTT / 固件下载三个地址都由它派生（见第五节）。换服务器、
换机房、换成别人提供的服务器，`srv` 那一格改个字符串就行，不用重新编译固件。

---

## 三、证书（Let's Encrypt，用 ECDSA）

```bash
sudo apt install certbot
sudo certbot certonly --standalone \
    -d cw_station.bubblegear.xyz \
    --key-type ecdsa --elliptic-curve secp256r1 \
    --agree-to-nos-mail -m you@example.com
```

三个细节值得照做：

- **用 `--key-type ecdsa`（P-256）而不是默认的 RSA-2048。** ESP32-C3 是 RV32 单核 160 MHz，
  RSA-2048 的握手能拖到好几秒（还要几百字节堆栈），ECDSA 通常在几百毫秒内完成，证书也更小。
- **同一张证书给 443 和 8883 用。** 设备只认一个根证书、只用这一个域名，两边共用最省事。
- **不要把 80 端口的验证关掉。** certbot 的续期（systemd timer 自带）需要它，
  续完会执行 deploy-hook，记得 reload nginx：

```bash
sudo certbot renew --dry-run      # 验证一下能自动续
```

固件里已经内置了 Let's Encrypt 的两个根：**ISRG Root X1** 和 **X2**（见 `main/root_ca.pem`，
两张串在一个文件里），所以不用管 certbot 给你的是 X1 链还是 X2 链。

> 换成别的 CA（或自签）时：把那个根证书覆盖 `main/root_ca.pem` 重新编译即可，代码一行不用改。

### 3.1 关于 `root_ca.pem`：能不能公开？要不要从服务器拿？

**能公开，它就是为公开而存在的。** 这个文件里只有两张 CA 的**公钥证书**，里面没有人会
care 的秘密 —— 它既不泄露你服务器的私钥、也不含你的域名，甚至不需要你服务器存在。每一个
操作系统、每一个浏览器的信任库里都躺着同一组 ISRG 根证书，任何人都能从
`https://letsencrypt.org/certs/isrgrootx1.pem` 免费下载。它进 GitHub 仓库完全没问题。

把它理解成一句声明就够了：**"我信任由这两位 CA 签发的服务器。"** 反过来，真正要锁起来的是
服务器上的 `privkey.pem` —— 那份一旦泄露，别人就能冒充你的域名。两者别混为一谈：

| 文件 | 内容 | 能公开吗 |
|---|---|---|
| `main/root_ca.pem` | CA 根证书（公钥） | ✅ 可以，已经是公开信息 |
| `fullchain.pem` | 叶子 + 中间证书（公钥） | ✅ 可以，TLS 握手时本来就会发给每个来连的人 |
| **`privkey.pem`** | **服务器私钥** | ❌ **绝对不能**，别进仓库、别发群里、别同步到网盘 |

**从哪拿**有两条路，别走第二条里面那个坑：

```bash
# ① 直接从 CA 官网取（推荐：不依赖你的服务器是否已经部署，拿到的就是真正的根）
curl -sS -O https://letsencrypt.org/certs/isrgrootx1.pem
curl -sS -O https://letsencrypt.org/certs/isrgrootx2.pem
cat isrgrootx1.pem isrgrootx2.pem > main/root_ca.pem

# ② 从你自己那条链反推 —— 这一步是「确认」，而不是「下载」
openssl s_client -connect cw_station.bubblegear.xyz:443 \
                 -servername cw_station.bubblegear.xyz </dev/null 2>/dev/null \
  | openssl x509 -noout -issuer
#   → issuer=C = US, O = Internet Security Research Group, CN = R11
```

第二步最容易踩的坑：**服务器在 TLS 握手里发的链只有「叶子 + 中间」，从来不含根证书**。
所以 `-showcerts` 抓下来的一堆 PEM 里，最后一张是中间证书（比如 `R11`），**不是根** ——
把中间证书塞进固件的话，ESP32 会认为这条链的顶端不可信，握手直接失败。根证书只能从
CA 官网（第 ① 条）或者系统信任库里取。

| `-issuer` 里写着 | 该用哪份根 | 要不要改固件 |
|---|---|---|
| `ISRG Root X1` / `X2`（含 `R10`/`R11`/`E5`/`E6` 这些中间名） | 现成的 `main/root_ca.pem` | 不用改，已经内置 |
| `ZeroSSL` / `USERTrust` | ZeroSSL 的根 | 要换文件重编译 |
| `(STAGING) ...` | 没啥用，**测试证书，别用** | 正式环境请重签发 |

最后一层顾虑是 **CA 会被写死在固件里**：设备认的根焊死在固件二进制中，将来换了 CA 而固件没跟上，
设备会立刻连不上，而且除了串口重烧没有别的补救办法。所以 443 和 8883 用同一张证书（同一条链），
续期或重签发时优先选同一家 CA —— 别让设备跟着你的证书一起被逼着 OTA。

---

## 四、nginx：443 反代 Web/固件，8883 反代 MQTT

### 4.1 HTTP / WebSocket（`/etc/nginx/sites-available/didaaa.conf`）

```nginx
server {
    listen 80;
    server_name cw_station.bubblegear.xyz;
    return 301 https://$host$request_uri;      # HTTP 一律跳 HTTPS
}

server {
    listen 443 ssl http2;
    server_name cw_station.bubblegear.xyz;

    ssl_certificate     /etc/letsencrypt/live/cw_station.bubblegear.xyz/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/cw_station.bubblegear.xyz/privkey.pem;
    ssl_protocols       TLSv1.2 TLSv1.3;
    # 只留两条 AEAD 套件：C3 上有硬件 AES/SHA 加速，这两个跑得动
    ssl_ciphers         ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-CHACHA20-POLY1305;
    ssl_prefer_server_ciphers off;
    # 会话复用：OTA 检查是每 20s 一次，别每次都来一次完整握手
    ssl_session_cache   shared:cwssl:1m;
    ssl_session_timeout 5m;

    client_max_body_size 0;                     # 不影响下载，只是别给上传设限

    location /fw/ {
        proxy_pass http://127.0.0.1:21301;
        proxy_read_timeout 300s;                # 设备下载 1.5MB 固件不能超时
    }

    location / {
        proxy_pass http://127.0.0.1:21301;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        # WebSocket（网页收发页）：Upgrade 头必须透传，否则握手在 101 之前就被掐了
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 3600s;
    }
}
```

### 4.2 MQTT over TLS（`/etc/nginx/stream-enabled/cw-mqtt.conf`）

`stream` 段在 `/etc/nginx/nginx.conf` 的**顶层**（和 `http` 平级，不在它里面）：

```nginx
# /etc/nginx/nginx.conf 末尾追加一行：
# stream { include /etc/nginx/stream-enabled/*; }

upstream cw_mqtt { server 127.0.0.1:1883; }

server {
    listen 8883 ssl;
    ssl_certificate     /etc/letsencrypt/live/cw_station.bubblegear.xyz/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/cw_station.bubblegear.xyz/privkey.pem;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_session_cache   shared:cwmqtt:1m;

    proxy_pass cw_mqtt;
    proxy_timeout 1h;            # MQTT 是长连接，10 分钟的默认值会被反复掐断
    proxy_connect_timeout 5s;
}
```

这样做的好处：**证书由 nginx 管，broker 完全不知道 TLS 的存在**（services.session 那边
还是明文 MQTT），到期自动续，broker 一行代码都不用改。

### 4.3 用 1Panel（OpenResty）面板时

上一节的配置文件是手写 nginx 的路子。用 1Panel 的话，**443 这一跳可以在面板里点出来，
8883 那一跳必须手写** —— 因为面板的「反向代理」是 HTTP 层的（七层），而 MQTT 是裸 TCP（四层）。

| 通路 | 面板能做吗 | 怎么做 |
|---|---|---|
| HTTP / WebSocket → `127.0.0.1:21301` | ✅ 能，且推荐 | 网站 → 创建「反向代理」→ 目标填 `http://127.0.0.1:21301`，再开 HTTPS |
| MQTT 8883 → `127.0.0.1:1883` | ❌ 不能 | 手写 `stream` 块（见下） |
| UDP 21303 | ❌ 不经过 nginx | 防火墙放行，直接连服务端 |

**第一步：先找到面板把配置放在哪。** 每个版本的路径略有差异，别照抄：

```bash
# 主配置（要改的就是它）
ls /opt/1panel/apps/openresty/openresty/conf/nginx.conf

# 面板签出来的证书实际存在哪 —— 从已有站点配置里反查，比记路径可靠
grep -rh "ssl_certificate" /opt/1panel/apps/openresty/openresty/conf/ 2>/dev/null | head
#   → ssl_certificate     /www/sites/didaaa/ssl/fullchain.pem;
#   → ssl_certificate_key /www/sites/didaaa/ssl/privkey.pem;
```

记下这两行，第三步要用。**第二步和第三步都在建站之后做**，顺序别反 —— 站点都不存在，
面板不会给你签证书，stream 里也就没有 `ssl_certificate` 可填。

**第二步：建反向代理站点。**

1Panel → 网站 → 创建网站 → 类型选**反向代理** → 主域名 `cw_station.bubblegear.xyz`
→ 代理地址 `http://127.0.0.1:21301` → 创建。然后在该站点的 HTTPS 页里申请证书
（Let's Encrypt / 已有证书都行），建议开强制 HTTPS。

建完打开面板里的「配置文件」确认一下，**是否包含 WebSocket 的 Upgrade 透传**：

```nginx
proxy_set_header Upgrade    $http_upgrade;
proxy_set_header Connection "upgrade";
proxy_read_timeout          3600s;   # WebSocket 是长连接，60 秒默认值会把它掐了
```

面板不一定自动写这几行。缺了的话网页收发页会在 WebSocket 握手时就失败（浏览器控制台报
101 没拿到），补进去重载即可。

**第三步：手写 8883 的 stream。**

先确认这个 OpenResty 编译时带了 stream 模块（1Panel 的官方源一般都带，但别想当然）：

```bash
/opt/1panel/apps/openresty/openresty/nginx/sbin/nginx -V 2>&1 | tr ' ' '\n' | grep -- --with-stream
# 有输出 = 支持
```

然后编辑**主配置** `/opt/1panel/apps/openresty/openresty/conf/nginx.conf`，在最外层
（和 `http { }` 平级，不要塞进 http 里面）追加：

```nginx
stream {
    upstream cw_mqtt { server 127.0.0.1:1883; }

    server {
        listen 8883 ssl;
        ssl_certificate     /www/sites/didaaa/ssl/fullchain.pem;  # ← 第一步查到的路径
        ssl_certificate_key /www/sites/didaaa/ssl/privkey.pem;
        ssl_protocols       TLSv1.2 TLSv1.3;
        ssl_ciphers         ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-GCM-SHA256;
        ssl_session_cache   shared:cwmqtt:1m;
        ssl_session_timeout 5m;

        proxy_pass cw_mqtt;
        proxy_timeout 1h;            # MQTT 是长连接
        proxy_connect_timeout 5s;
    }
}
```

改完 `nginx -t` 通过后重载（面板上点「重载」或直接 `nginx -s reload`）。

⚠️ **一句话警告**：这一段写在主配置里，而主配置是面板管的。**升级 / 重装 OpenResty、
或者在面板里做完某些全局设置保存后，`stream` 块有可能被覆盖掉**。症状是 443 正常、
设备却连不上 MQTT。所以这一段务必留一个副本：

```bash
cp /opt/1panel/apps/openresty/openresty/conf/nginx.conf /root/nginx.conf.cw-stream.bak
```

真被覆盖了，把 `stream { }` 那段重新贴回去就行。

**第四步：放行 UDP 21303。** 两层都要开，漏一层就是"配好了却不通"：

- 云厂商安全组：放行 `21303/udp`（还有 `443/tcp`、`8883/tcp`）
- 1Panel → 防火墙 → 放行 `21303/udp`

UDP 没法从我这台机器验证 —— 我在本地探测时走了代理，DNS 被劫持到 `198.18.0.151`
（fake-ip 保留段）。请在**服务器本机**上核对：

```bash
ss -lunp | grep 21303          # 期望 udp UNCONN 0.0.0.0:21303
curl -s localhost:21301/who    # 期望返回 JSON
```

---

## 五、服务端进程本身：把明文端口收回本机

`didaaa-server` 支持 `CW_BIND`。**公网部署时它是安全开关，不是可选项** —— 少了它，
第四节在 nginx 上配的 TLS 会被完整绕过。

**用 PM2 裸跑就写进 `ecosystem.config.js`**（仓库里已经备好两套 env，直接用 `--env production`）：

```bash
pm2 start ecosystem.config.js --env production     # CW_BIND=127.0.0.1
pm2 save && pm2 startup
```

**不用 PM2 就当场传环境变量**：

```bash
CW_BIND=127.0.0.1 CW_DATA_DIR=/var/lib/cw node server.js
```

- Web/WebSocket（21301）与 MQTT（1883）只监听 `127.0.0.1` —— 公网探测它们连不上。
- **UDP 21303 不受 CW_BIND 约束**，它是 `dgram` socket，必须对外可达，否则设备一帧都发不进来。
- Docker 部署别用 `CW_BIND`（容器里绑 127.0.0.1 会让端口映射失效），改用防火墙见下节。

用 systemd 就写成：

```ini
[Service]
WorkingDirectory=/opt/didaaa-server
Environment=CW_BIND=127.0.0.1
Environment=CW_DATA_DIR=/var/lib/cw
ExecStart=/usr/bin/node server.js
Restart=always
```

**改完当场验证**，别凭感觉认为生效了：

```bash
ss -ltnp | grep -E ':(21301|1883)'
#   期望 Local Address 是 127.0.0.1:21301 / 127.0.0.1:1883
#   如果还是 0.0.0.0，说明 CW_BIND 没进去 —— 明文端口正对着公网敞开
ss -lunp | grep 21303
#   期望 0.0.0.0:21303。UDP 必须公网可达，这里反而是对的
```

> **`CW_DATA_DIR` 一定要指到一个不会丢的目录**：`identity.json` 里是
> Global UID ↔ 虚拟呼号的永久绑定，丢一次等于全体设备换号。

---

## 六、UDP 那一跳：绕不开，但可以加固

`21303/udp` 是唯一露在公网、且没法套 TLS 的端口。先做最低限度的防火墙：

```bash
sudo ufw allow 22/tcp
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow 8883/tcp
sudo ufw allow 21303/udp
sudo ufw enable
```

目前服务端对 UDP 帧的校验是"uid 是否已经认证过"（规则 4），而 uid 只是 16 位短号 ——
**伪造一个已知 uid 就能冒充别人发键控**。这在内网无所谓，在公网上要认真对待。三个选项：

### 方案 A（最彻底）：WireGuard 隧道

服务端跑 wg，设备端加 `esp_wireguard` 组件：

```ini
# /etc/wireguard/wg0.conf（服务端）
[Interface]
Address = 10.7.0.1/24
ListenPort = 51820
PrivateKey = <服务器私钥>

[Peer]        # 每台设备一段
PublicKey = <设备公钥>
AllowedIPs = 10.7.0.2/32
```

然后把 MQTT 和 UDP 都只绑在 `10.7.0.1` 上，防火墙只留 `51820/udp` 与 `443/tcp`。
**公网从此摸不到 21303 和 8883** —— 它们只存在于隧道里。

代价：每台设备要烧一对密钥（可以在配网页里生成/填入），服务端要逐个加 peer。

### 方案 B（最省资源）：帧级签名（HMAC-SHA256）

服务端在认证通过时给设备下发一个 32 字节密钥，之后的每帧附上前 8 字节签名：

```
signature = HMAC-SHA256(key, seq | uid | freq | payload)[0..8)
payload  += seq 单调递增
```

服务端先验签名、再验 seq 防重放。这一路**只鉴真、不加密** —— 恰恰是对的：
CW 点划本身不是秘密（本来就要被所有人听到），真正要防的是**冒名插进来按键**。

代价：协议帧格式要扩容（现在定长 16/20 字节），服务端与固件同步改。

### 方案 C：什么都不做

只靠 ufw。能挡扫描和洪水，**挡不了冒名**。适合内网 / 熟人小圈子。

> 我的建议是：**先把 MQTT 和 HTTP 的 TLS 做掉**（本文第四、五节，改动最小、收益最大），
> UDP 这一路按你愿意投入的程度在 A / B / C 里选。要我实现 A 或 B 的话说一声，
> 两者都还要动协议，不是改个配置就完事。

---

## 七、设备侧：一句话就通联

### 7.1 配网页里填域名

菜单 → `BASE STATION` → `CHANGE`，填：

```text
cw_station.bubblegear.xyz
```

固件会把它分派到三条通路（代码：`main/cw_net.h` 的 `CW_MQTT_SCHEME` / `CW_FW_SCHEME`）：

```
| CW_TLS | UDP            | MQTT                  | 固件下载                     |
|--------|---------------|-----------------------|------------------------------|
| 关（默认，局域网） | 域名:21303      | mqtt://域名:1883      | http://域名:21301/fw/cw.bin   |
| 开（公网推荐）    | 域名:21303      | mqtts://域名:8883     | https://域名/fw/cw.bin（443）|
```

**域名解析**这一步以前会静默失败：`inet_addr()` 只认点分十进制，填域名会得到
`255.255.255.255`（于是键控全发到广播上）。现在改成先按 IP 试、不是 IP 才走 DNS
（`cw_net.c` 的 `resolve_server()`），解析不出来时报 `NET FAIL` 并在日志里写清楚原因。

### 7.2 打开 TLS

```bash
idf.py menuconfig → CW Radio → [*] Transport security: MQTTS + HTTPS
idf.py build
```

- 两者都用 `main/root_ca.pem` 里的根证书**校验服务器身份**。这一步不能省：
  不校验的 TLS 只加密、不认人，中间人拿一张自签证书就能冒充你的 broker。
- 代价只有不到 2 KB：实测 **+1970 字节**（0x183370 → 0x183b20），几乎全是那张根证书的体积。
- 剩余 Flash 仍是 62%，没有任何压力。

### 7.3 走 OTA 把新固件推出去

```bash
cd didaaa
# 1) main/cw_radio.h 里抬版本号（OTA 只认"服务端版本 > 本机版本"）
idf.py build
# 2) 发布到 didaaa-server/firmware/，服务端不用重启（/fw/version 每次请求重新读盘）
./tools/publish-fw.sh
```

设备端：菜单 → `FIRMWARE` → `UPDATE`。

⚠️ 固件的 SHA-256 只是写进 `version.json` 给人看，**没有被设备用来校验**；真正落地的
完整性检查是 `esp_ota_end()` 对镜像自身 checksum 的校验，它只能防传输出错、不能防替换。
要防"有人把服务器上的 cw.bin 换成别的镜像"，得再加一道签名验签 —— 真要加的话，
在 `main/root_ca.pem` 旁边放一个公钥即可，固件的嵌入机制已经在那里了。

---

## 八、上线核对清单

```bash
# 1) DNS 指向对不对
dig +short cw_station.bubblegear.xyz

# 2) HTTPS：链条能不能接到内置的根
curl -v https://cw_station.bubblegear.xyz/fw/version
#      期望：200 + 返回 version.json 的 JSON

# 3) MQTTS：用同一个根证书做 clinet 校验
openssl s_client -connect cw_station.bubblegear.xyz:8883 -CAfile didaaa/main/root_ca.pem -brief </dev/null
#      期望：Verification: OK

# 4) 明文端口是不是真的收回去了（公网 IP 上扫）
nmap -Pn <服务器公网IP> -p 21301,1883
#      期望：filtered / closed

# 5) UDP 通不通（服务端日志应当出现 "设备上线: VXXXXX"）
```

服务端一眼能看的是这几个：

```
UDP 设备口 : 21303 (auth 16B 上行 / key 20B 下行)
MQTT 信令口: mqtt://localhost:1883
身份库     : /var/lib/cw/identity.json（N 条绑定）
```

---

## 九、换成别人提供的服务器

只要对方也按上面的端口布局跑 didaaa-server（21303 UDP / 443 固件 / 8883 信令），
设备侧**只改一格**就能切过去：

菜单 → `BASE STATION` → `CHANGE` → 填对方的域名 → 保存后自动重启。

固件三秒钟后就会用新域名重认证。注意两件事：

1. **虚拟呼号是服务器分配的**，换服务器等于换一个陌生的服务端 —— 对方会给你重新分配一个
   `V?????`，原来那个不会跟着走（`identity.json` 存在各自的服务器上）。
2. 同频容差（`CW_TOL`，服务端）必须和固件的 `CW_CHANNEL_TOL_HZ` 一致（默认都是 100 Hz），
   两边不一致会出现"服务端转发了、设备端听不见"这种半边生效的怪事。
