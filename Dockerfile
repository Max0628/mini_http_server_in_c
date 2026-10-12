FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install --yes --no-install-recommends gcc libc6-dev make curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

# 先清掉可能存在的編譯產物，再使用 container 裡的 Linux GCC 編譯。
RUN make clean && make

EXPOSE 8080

CMD ["./mini_http_server"]
