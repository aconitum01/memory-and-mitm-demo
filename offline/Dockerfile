# ===== build stage =====
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        libncursesw5-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY game.c cheat.c Makefile ./
RUN make

# ===== runtime stage =====
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libncurses6 \
        libncursesw6 \
        procps \
        locales \
    && sed -i '/ja_JP.UTF-8/s/^# //' /etc/locale.gen \
    && locale-gen ja_JP.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=ja_JP.UTF-8 \
    LC_ALL=ja_JP.UTF-8 \
    TERM=xterm-256color

WORKDIR /app
COPY --from=builder /build/game  ./game
COPY --from=builder /build/cheat ./cheat

# offline は exec で対話的に使う想定なので、コンテナを生かしておくだけ
CMD ["sleep", "infinity"]
