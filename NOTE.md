https://github.com/martin-ger/esp32_nat_router?tab=readme-ov-file

這個為 esp32 router 的部分

## 操作步驟:

1. 連接 esp32，開啟 vscode 並執行 build 和 run
2. wifi 中會出現'ESP32_NAT_Router'名稱，連線後，esp32 會給每一個設備一個 IP，就是設備在 ESP32 router 上的 IP。
3. 打開網頁"http://192.168.4.1"，會開啟設定，
   > 第一部分為 ESP32 網路設定，名稱和密碼
   > 第二部分為讓 ESP32 連接路由器，就是一般家用 WIFI 分享器
4. "connect"後，會重啟 ESP32，這時就會知道 ESP32 在家用路由器中的 IP 位置。
5. 這時將設備連上'ESP32_NAT_Router'，就可以藉由 ESP32 router 上網。

## 連接內網設備

- 假設有一個網站為 localhost:3000，代表進入這個 IP(localhost)後，會連接到 port = 3000

1. 當要使用其他設備(只連接家用路由器)，要連線 ESP32 router 內網的設備時，需要建立映射
2.

```
portmap add TCP 8080 192.168.4.3 3000
```

建立一個 TCP 映射，將 8080 的 port 映射到 192.168.4.3 的 3000 port，意思是如果連接 ESP32 router 的 8080 port，就會到 192.168.4.3 的 3000 port 這個地方。 3. 使用其他設備(只連接家用路由器)，可以在瀏覽器中打開

```
(ESP32 router在家用路由器的IP位置):8080
```

因為設備連上了家用路由器，所以和 ESP32 router 屬於内網，當連接這個 IP 後，在連到對應的 port 就能連接到有在 ESP32 router 上開啟映射的其他設備。
