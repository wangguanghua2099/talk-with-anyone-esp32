// config_private.example.h —— 复制为 config_private.h 后填入你自己的配置
// config_private.h 不会提交到仓库，真实 WiFi 密码/局域网 IP 留在你自己电脑上
#pragma once

// 连接 talk-with-anyone 服务端所用 WiFi（手机热点或路由器均可，
// 前提：小音箱与运行服务端的电脑在同一网络）
#define WIFI_SSID     "你的WiFi名称"
#define WIFI_PASSWORD "你的WiFi密码"

// 运行 talk-with-anyone 服务端的电脑局域网 IPv4
// Windows 上用 ipconfig 查看"无线局域网适配器 WLAN"的 IPv4 地址；
// 路由器 DHCP 可能变更该地址，建议在路由器后台给电脑绑定静态 IP
#define SERVER_HOST   "192.168.1.100"

// 服务端监听端口（与服务端 config.json 保持一致）
#define SERVER_PORT   7862

// 与服务端 config.json 的 access_token 一致，未启用则留空 ""
#define ACCESS_TOKEN  ""
