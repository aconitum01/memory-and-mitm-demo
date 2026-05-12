# ゲームチート デモ

テックトーク用のデモプロジェクト。「ゲームのチートはどう成立するのか / どう守るのか」を、最小構成のオフライン版＆オンライン版で実演します。

- **`offline/`** ── C 製の ncurses ゲームと、`/proc/<pid>/mem` 越しにメモリを書き換えるチートツール。**「同一プロセスでは何も守れない」** を見せる。
- **`online/`** ── ガチャゲームのサーバー（C）／ブラウザ UI（JS）／プレイヤー PC で動く HTTP MITM プロキシ型チート（C）。**「サーバーで抽選していてもクライアントの言うことを信じたら守れない」** を見せる。引いた直後のアイテムだけが SSR にすり替わるので、アニメで N が出てもインベントリには SSR が積まれる。プロキシは何を書き換えたかを `stderr` に before/after で吐くので、`docker compose up` を foreground で動かしておけば横で「今これを書き換えました」が見える。

## クイックスタート (Docker)

Docker と Docker Compose v2 が入っていれば、OS を問わず動きます。

```bash
git clone <this-repo>
cd <this-repo>

# --- オンライン版（サーバーとチートプロキシを同時起動） ---
docker compose --profile online up        # foreground 推奨。プロキシのログが横で見える
# ブラウザで URL を切り替えてモードを選ぶ:
#   http://localhost:8080   健全モード（サーバー直）
#   http://localhost:8081   チートモード（プレイヤーの PC で動く MITM プロキシ越し）
#
# 両方とも実体は同じ 1 台のサーバー。
# 8081 でガチャを引くたびに、ターミナル側の cheat-proxy ログに
#   [cheat] ───── /api/sync intercepted ─────
#           ← from client: {"items":[1,5,2,8]}
#           → to   server: {"items":[1,5,2,11]}
# のような書換ログが流れる。
# 8081 で書き換えられたアイテムは 8080 からも見える（= 本物の保管庫が汚染されている）。
```

```bash
# --- オフライン版（対話デモ。2 ターミナル使う） ---
docker compose --profile offline up -d

# ターミナル A：ゲーム起動
docker compose exec offline ./game

# ターミナル B：チート起動（ゲームの PID を取って渡す）
docker compose exec offline bash -c './cheat $(pgrep game)'
# プロンプトで:
#   cheat> scan 100        ← 勇者の HP を検索
#   cheat> next 90         ← ダメージを受けたら絞り込み
#   cheat> setall 9999     ← HP を書き換える
#   cheat> scan 9999       ← 魔王の HP を検索
#   cheat> setall 1        ← 一撃で倒せるようにする

# 終了
docker compose --profile offline down
```

## プロジェクト構成

```
.
├── docker-compose.yml          # 2 つの profile: offline / online
├── offline/
│   ├── Dockerfile
│   ├── Makefile
│   ├── game.c                  # ncurses「勇者 vs 魔王」
│   └── cheat.c                 # /proc/<pid>/mem スキャナ・エディタ
└── online/
    ├── Dockerfile
    ├── Makefile
    ├── server.c                # HTTP ガチャサーバー（脆弱な /api/sync 持ち）
    ├── cheat.c                 # HTTP プロキシ。/api/sync の body を書換
    └── public/
        ├── index.html          # ガチャ UI
        ├── style.css           # レアリティ別演出
        └── app.js              # fetch + アニメーション
```

## オフライン版の中身

- `game.c`：`Character { name, hp, atk_min, atk_max }` を BSS グローバルに置いた素朴な ncurses ゲーム
- `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` を呼んでおり、同 UID の別プロセスから `/proc/<pid>/mem` を開けるようにしている
- `cheat.c`：`scan / next / list / set / setall / regions` を持つ古典的なメモリエディタ

教訓：プロセスメモリは他プロセスから書き換えられる。**「クライアントしか動いていない」状況では、状態を守るすべがない**。

## オンライン版の中身

### プロトコル（脆弱）

| メソッド・パス | 動作 |
|---|---|
| `GET /` ほか | `public/` から静的配信 |
| `POST /api/roll` | サーバー側 `rand()` で抽選。結果を返すだけで**保管はしない** |
| `POST /api/sync` | クライアントから `{"items":[id,...]}` を受け、保管庫を**丸ごと上書き** ← 脆弱 |
| `GET /api/inventory` | 現在の保管庫を返す |
| `POST /api/reset` | 保管庫クリア |

### チート（HTTP プロキシ）

`cheat.c` は単なる TCP プロキシ。`POST /api/sync` のリクエストの body `{"items":[...]}` の **末尾 1 個** だけを `11`（SSR エクスカリバー）に書き換え、Content-Length を再計算してサーバーへ転送する。既存のアイテムには触らない。

→ 「今引いたアイテム（= 配列末尾）が必ず SSR にすり替わる」挙動になる。アニメで「こんぼう (N)」が出てもインベントリには「エクスカリバー (SSR)」が増える、というそのギャップが画面で見える。

書換のたびにプロキシは `stderr` に before/after を吐く：

```
[cheat] ───── /api/sync intercepted ─────
        ← from client: {"items":[1,5,2,8]}
        → to   server: {"items":[1,5,2,11]}
        書換: 末尾 id=8 → id=11 (SSR エクスカリバー)
```

`docker compose --profile online up` を foreground で動かしておけば、横のターミナルに上記のような行が流れて「今この瞬間に書き換えた」がリアルタイムに見える。

このプロキシは Docker 構成上、`player-pc` という **サーバーとは別のネットワーク** に隔離されており、サーバーには公開ポート (`host.docker.internal:8080`) 経由でしか到達できない。プレイヤーの PC で動いている MITM ツールという立ち位置を意図している。

教訓：

- **抽選をサーバー側でやれば安全**、ではない。
- **クライアントが何を持っているかをクライアントに教えてもらう設計** にした時点で、信頼境界が崩れる。
- HTTPS は本ケースでは無力（ユーザー本人が CA を信頼ストアに入れれば容易に MITM される）。
- 真の対策はアーキテクチャ：**サーバーオーソリタティブ**（`/api/sync` を消し、`/api/roll` 内で抽選＋保管を 1 トランザクションで完結させる）。

## Docker を使わずに動かす（Linux のみ）

```bash
# offline
cd offline
make
./game            # ターミナル A
./cheat <pid>     # ターミナル B（pgrep game で PID 取得）

# online
cd online
make
./server                       # ターミナル A: 8080 で待受（健全モード）
./cheat 8081 8080              # ターミナル B: 8081 で待受、8080 へ転送（チートモード）
# → ブラウザで http://localhost:8080 = 健全 / http://localhost:8081 = チート
```

主な依存：

- `gcc` / `make`
- `libncurses-dev`（offline のみ）
- ブラウザ（online クライアントの動作確認用）

### 各バイナリのフラグ

```
./server [--port N] [--bind ADDR]
   --port      待受ポート（デフォルト 8080）
   --bind      待受アドレス（デフォルト 127.0.0.1、Docker は 0.0.0.0）

./cheat <listen-port> <upstream-port> [<upstream-host>] [--bind ADDR]
   upstream-host  デフォルト 127.0.0.1。Docker では "online-server" などサービス名
```

## 注意点

- `/proc/<pid>/mem` を別プロセスから開く動作はホストカーネルの `yama/ptrace_scope` 設定に依存します。`cat /proc/sys/kernel/yama/ptrace_scope` が `2` 以上だとオフラインデモが動かないことがあります（Docker でも同様）。
- macOS / Windows ユーザーは Docker Desktop でオンライン版は問題なく動作。オフライン版は Linux カーネル固有の機能（`/proc/<pid>/mem`、`prctl(PR_SET_PTRACER)`）を使うため、Docker Desktop の Linux VM 経由で動きます。
- このプロジェクトは**意図的に脆弱**に作ってあります。本番システムの参考にはしないでください。
