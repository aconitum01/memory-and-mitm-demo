# ===== build stage =====
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY server.c cheat.c Makefile ./
RUN make

# ===== runtime stage =====
FROM debian:bookworm-slim

WORKDIR /app
COPY --from=builder /build/server ./server
COPY --from=builder /build/cheat  ./cheat
COPY public/ ./public/

EXPOSE 8080 8081

# デフォルトは「健全な server を 8080 で待受」
CMD ["./server", "--port", "8080"]
