# mgmp 服务器（Linux 版）

和 Windows 版是**同一份源码** `../mgmp_server.cpp`（平台差异集中在文件开头的 platform 区块），协议、房间、密码、日志收集功能完全一致。

## 构建

```bash
sudo apt install -y g++          # Ubuntu / Debian；其它发行版装 g++ 或 clang++ 即可
./build.sh                        # 生成 ./mgmp_server
# 想要不依赖系统库、能拷到任何 x86-64 Linux 上直接跑的版本：
g++ -std=c++17 -O2 -static -I../../third_party ../mgmp_server.cpp -o mgmp_server && strip mgmp_server
```

也可以用 CMake：`cd .. && cmake -S . -B build && cmake --build build`。
（仓库里 `mgmp/server/linux/mgmp_server` 已经是静态链接好的 x86-64 版本，可以直接上传使用。）

## 运行

```bash
./mgmp_server                                  # 端口 27700，日志存在可执行文件旁的 mgmp_logs/
./mgmp_server --port 27700 --logdir /var/lib/mgmp/logs --quiet
```

Ctrl+C 或 `SIGTERM` 会正常退出（未传完的日志会被清理）。

## 作为服务常驻（systemd）

```bash
sudo useradd --system --home /var/lib/mgmp --create-home --shell /usr/sbin/nologin mgmp
sudo install -m 755 mgmp_server /usr/local/bin/mgmp_server
sudo cp mgmp-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mgmp-server
journalctl -u mgmp-server -f                   # 看服务器自己的日志
```

玩家用 F2 面板上传的游戏日志在 `/var/lib/mgmp/logs/<时间>_<玩家名>_<IP>/`（含日志文件和 `info.txt`，里面有玩家写的问题描述）。

## 防火墙 / 云服务器

- 只需要放行 **TCP 27700**（信令服务器端口）。云厂商的“安全组/防火墙”里要单独放行一次；Ubuntu 自带的 ufw 如果开着也要：`sudo ufw allow 27700/tcp`。
- 服务器默认**只介绍玩家互相认识，不转发游戏流量**：房主要让队友能连到房主自己的游戏端口（默认 27600），或者选 Steam 中继。只有房主在创建房间时选了“服务器转发”，服务器才会把这个房间的游戏 TCP 字节原样转发：同时最多 `--relay-rooms`（默认 10）个转发房间，每个房间持续 `--relay-kbps`（默认 32）KB/s，首次同步有 3 MiB 突发额度；这会占用服务器带宽和流量，配置低的服务器请调小。
- 服务器本身没有认证也没有加密：房间密码只保护“加入房间”这一步；日志上传受限流（每个地址每小时 12 次）和容量上限（3000 份、单次 32 MB）约束。公网部署请知晓这一点，需要的话在前面套一层防火墙白名单。

## 游戏里怎么连

mgmp.json 里 `signal.addr` 填服务器公网地址（或在标题界面“多人联机”窗口里直接填），F2 面板里“服务器地址”同理。
