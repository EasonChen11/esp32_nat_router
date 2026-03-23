# ESP32 NAT Router 技術筆記

## lwIP 是什麼？

lwIP（lightweight IP）是一個輕量級的開源 TCP/IP 協議棧，專為嵌入式系統設計。它的目標是在有限的 RAM 和 ROM 資源下提供完整的 TCP/IP 功能。

### lwIP 的核心特性

- 支援 IPv4/IPv6、TCP、UDP、ICMP、ARP 等標準協議
- 支援 DHCP Client/Server、DNS、SNMP
- 支援 IP 轉發（IP Forwarding）與 NAT/NAPT
- 記憶體佔用極小，適合 MCU 等嵌入式裝置
- ESP-IDF 預設將 lwIP 作為 TCP/IP 協議棧

### 網路分層模型與 lwIP 的位置

一般網路模型分層如下（OSI 七層對照 TCP/IP 四層）：

| OSI 七層 | TCP/IP 四層 | 功能 | 範例 |
|----------|-------------|------|------|
| 7. 應用層 | 應用層 | 使用者介面與協議 | HTTP, DNS, MQTT |
| 6. 表示層 | ↑ | 資料格式轉換、加密 | TLS, JSON |
| 5. 會話層 | ↑ | 連線管理 | Session |
| 4. 傳輸層 | 傳輸層 | 端對端傳輸 | TCP, UDP |
| 3. 網路層 | 網際網路層 | IP 路由、NAT | IP, ICMP, ARP |
| 2. 資料鏈結層 | 網路存取層 | Frame 封裝、MAC | WiFi (802.11), Ethernet |
| 1. 實體層 | ↑ | 電氣訊號傳輸 | 無線電波、網路線 |

lwIP 涵蓋 **L3 ~ L4**，並包含部分應用層協議：

- **L3 網路層** — IP 路由、NAPT、ICMP、ARP（lwIP 的核心）
- **L4 傳輸層** — TCP、UDP 連線管理
- **部分 L7 應用層** — 內建 DHCP、DNS、SNTP 等輕量協議

lwIP **不處理**的部分：
- **L1/L2**（實體層/資料鏈結層）— 由 ESP32 WiFi Driver 負責
- **完整的應用層** — HTTP Server、MQTT 等由 ESP-IDF 的其他元件處理

### lwIP 在 ESP32 中的角色

ESP32 的網路架構分層：

```
HTTP Server, CLI ─────── 應用層（ESP-IDF 元件）
        │
   TCP / UDP ─────────── 傳輸層（lwIP）
        │
 IP Forward + NAPT ───── 網路層（lwIP）  ← 封包轉發在這裡發生
        │
    ESP-NETIF ─────────── 抽象層（銜接 lwIP 與 Driver）
        │
  WiFi Driver ─────────── 資料鏈結層 + 實體層（ESP32 硬體）
```

簡單來說，lwIP 就是 ESP32 的「網路大腦」，負責 L3/L4 的所有決策（封包要送去哪、怎麼轉換位址、TCP 連線狀態追蹤），而 WiFi Driver 只負責「把封包透過無線電收發出去」。ESP-NETIF 則是 ESP-IDF 提供的抽象層，將 WiFi Driver 收到的 L2 封包傳遞給 lwIP。

---

## NAT 是什麼？與 lwIP 的關係

### NAT（Network Address Translation）

NAT 將內網的私有 IP 位址轉換為外網 IP，讓多台裝置共用一個對外 IP 上網。

### NAPT（Network Address and Port Translation）

NAPT 是 NAT 的進階版本，不僅轉換 IP 位址，還轉換 Port 號碼。這樣多台內網裝置即使存取同一個外部服務，也能透過不同的 Port 區分。

### lwIP 內建的 NAPT 支援

lwIP 原生支援 NAPT，但需要在編譯時透過 sdkconfig 啟用：

```
CONFIG_LWIP_IP_FORWARD=y     # 啟用 IP 轉發，讓封包能在不同 netif 間路由
CONFIG_LWIP_IPV4_NAPT=y      # 啟用 NAPT，自動改寫來源 IP 和 Port
CONFIG_LWIP_L2_TO_L3_COPY=y  # 允許 L2 WiFi 封包複製到 L3 IP 層處理
```

啟用後，lwIP 會在內部維護一張 NAT 連線狀態表，自動追蹤每條連線的 IP/Port 對應關係。

---

## 封包交換機制

本專案的封包轉發完全依賴 lwIP 內建的 IP Forwarding + NAPT，專案本身不解析或操作任何封包內容。

### 雙模 WiFi 架構

ESP32 以 `WIFI_MODE_APSTA` 模式運作，同時扮演兩個角色：

| 介面 | 角色 | IP 範圍 | 功能 |
|------|------|---------|------|
| AP（Access Point） | 提供 WiFi 給其他裝置連線 | 192.168.4.0/24 | 內網，運行 DHCP Server |
| STA（Station） | 連接到上游路由器 | 由上游 DHCP 分配 | 外網，取得網際網路存取 |

兩個介面共用同一個實體無線電晶片，透過時分方式切換。

### 封包上行流程（AP 裝置 → 網際網路）

```
手機 (192.168.4.10) 發送 HTTP 請求到 example.com
        │
        ▼
┌──────────────────────────────────────────────┐
│ 1. ESP32 AP 介面接收 WiFi 封包（L2）          │
│    WiFi Driver 將 frame 交給 ESP-NETIF       │
├──────────────────────────────────────────────┤
│ 2. L2-to-L3 Copy                             │
│    ESP-NETIF 將封包傳入 lwIP 協議棧           │
├──────────────────────────────────────────────┤
│ 3. lwIP IP Forwarding（L3）                   │
│    查路由表 → 目的地不在 AP 子網              │
│    → 決定從 STA 介面轉發出去                  │
├──────────────────────────────────────────────┤
│ 4. NAPT 改寫（L3/L4）                         │
│    來源 IP:  192.168.4.10 → STA_IP           │
│    來源 Port: 12345 → 隨機高位 Port           │
│    記錄對應關係到 NAT 狀態表                   │
├──────────────────────────────────────────────┤
│ 5. ESP32 STA 介面發送封包到上游路由器（L2）    │
└──────────────────────────────────────────────┘
        │
        ▼
上游路由器 → 網際網路
```

### 封包下行流程（網際網路 → AP 裝置）

```
example.com 回覆封包到 ESP32 STA IP
        │
        ▼
┌──────────────────────────────────────────────┐
│ 1. ESP32 STA 介面接收回覆封包（L2）           │
├──────────────────────────────────────────────┤
│ 2. lwIP 收到封包（L3）                        │
│    NAPT 反向查表                              │
│    目的 IP:  STA_IP → 192.168.4.10           │
│    目的 Port: 隨機高位 → 12345               │
├──────────────────────────────────────────────┤
│ 3. 轉發到 AP 介面（L3 → L2）                  │
│    ESP32 AP 介面透過 WiFi 發送給手機           │
└──────────────────────────────────────────────┘
        │
        ▼
手機收到 HTTP 回覆
```

### 在哪一層交換？

| 層級 | 功能 | 實現方式 |
|------|------|----------|
| L2（資料鏈結層） | WiFi 封包收發 | ESP32 WiFi Driver，APSTA 模式 |
| L2 → L3 | 封包傳遞 | `L2_TO_L3_COPY` 將 WiFi frame 交給 lwIP |
| **L3（網路層）** | **IP 轉發** | **lwIP `IP_FORWARD` 在兩個 netif 間路由** |
| L3/L4 | NAT 改寫 | lwIP `IPV4_NAPT` 改寫 IP + Port |

**核心交換發生在 L3（網路層）**，由 lwIP 的 IP Forwarding 功能完成。

---

## 關鍵程式碼

### 初始化雙模 WiFi

```c
// esp32_nat_router.c
esp_netif_create_default_wifi_ap();   // 建立 AP 網路介面
esp_netif_create_default_wifi_sta();  // 建立 STA 網路介面
esp_wifi_set_mode(WIFI_MODE_APSTA);   // 設定雙模模式
```

### 啟用 NAT

```c
// esp32_nat_router.c - STA 取得 IP 後觸發
ip_napt_enable(my_ap_ip, 1);  // 以 AP 的 IP 啟用 NAPT
```

這一行呼叫 lwIP 的 API，告訴 lwIP：「對 AP 介面上的所有封包啟用 NAT 轉換」。之後所有從 AP 進來、要轉發到 STA 的封包，都會自動被改寫來源位址。

### Port Mapping（可選的端口映射）

```c
// 結構定義
struct portmap_table_entry {
    u32_t daddr;   // 內網目標 IP
    u16_t mport;   // 外部映射 Port
    u16_t dport;   // 內網目標 Port
    u8_t proto;    // 協議（TCP=6, UDP=17）
    u8_t valid;    // 是否有效
};

// 新增映射
ip_portmap_add(proto, my_ap_ip, mport, daddr, dport);

// 刪除映射
ip_portmap_remove(proto, mport);
```

Port Mapping 讓外部網路能主動存取 AP 子網內的裝置。例如將 ESP32 STA 的 8080 Port 映射到內網 192.168.4.3 的 3000 Port。

### DNS 透傳

```c
// STA 連上上游路由器取得 DNS 後
esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns);
// 將同一個 DNS 設定到 AP 的 DHCP Server
dhcps_dns_setserver(&dns.ip.u_addr.ip4);
esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
```

這確保連接到 AP 的裝置能自動獲得正確的 DNS 伺服器位址。

---

## ESP32 與其他設備的互動

### 整體網路拓撲

```
┌──────────┐     WiFi      ┌─────────────────┐     WiFi      ┌──────────────┐
│ 手機/筆電 │ ◄──────────► │   ESP32 Router   │ ◄──────────► │  上游路由器   │
│ (Client) │   AP 介面     │  AP    ←→    STA │   STA 介面   │ (Internet)   │
│ 192.168  │   192.168.4.x │ .4.1        DHCP │              │              │
│ .4.10    │               │    lwIP NAT      │              │              │
└──────────┘               └─────────────────┘              └──────────────┘
```

### 設備互動方式

1. **Client 裝置 ↔ ESP32 AP**
   - Client 透過 WiFi 連接到 ESP32 的 AP
   - ESP32 的 DHCP Server 分配 192.168.4.x IP
   - ESP32 的 DNS 設定透過 DHCP 傳給 Client

2. **ESP32 STA ↔ 上游路由器**
   - ESP32 以 STA 模式連接上游 WiFi（支援 WPA2/WPA2-Enterprise）
   - 從上游 DHCP 取得 IP、DNS 等設定
   - 也支援手動設定靜態 IP

3. **ESP32 內部轉發**
   - lwIP 的 IP Forwarding 自動在 AP 和 STA 兩個 netif 間路由
   - NAPT 改寫位址讓內網裝置對外部網路不可見
   - 整個過程對 Client 完全透明，Client 不需要任何特殊設定

4. **管理介面**
   - HTTP Web Server（Port 80）提供網頁設定介面
   - CLI Console 透過 UART/USB 提供指令列設定
   - 設定儲存在 NVS（Non-Volatile Storage）中，重啟後保留
