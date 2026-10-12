# Minimal HTTP Server

這是一個使用 C11 撰寫的教學用途 HTTP/1.1 server，對應 `spec.md` 的 V1 規格。

## 在 Linux / Container 中編譯

```bash
make
```

## 啟動 server

```bash
./mini_http_server
```

server 會監聽：

```text
0.0.0.0:8080
```

另一個 terminal 執行：

```bash
curl -v http://127.0.0.1:8080/
```

預期 response body 是：

```text
Hello
```

## 使用 Docker

```bash
docker build -t mini-http-server .
docker run --rm -p 127.0.0.1:8080:8080 mini-http-server
```

## V1 支援範圍

- 單執行緒
- blocking TCP socket
- IPv4
- 一條 connection 只處理一個 request
- `GET /` 回傳 `200 OK` 與 `Hello`
- request header 上限 8192 bytes
- request timeout 5 秒
- partial read / partial write
- `400`、`404`、`405`、`408`、`431`、`505` 錯誤 response
