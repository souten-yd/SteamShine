# SteamShine: 配信最優先・Web管理・低負荷ポリシー

- 状態: 実装前の拘束仕様
- 対象: SteamOS / AMD GPU / Moonlight・Artemis
- 作成日: 2026-07-23
- 関連文書:
  - [`STEAMOS_VIRTUAL_SESSION_PLAN.md`](./STEAMOS_VIRTUAL_SESSION_PLAN.md)
  - [`STEAMOS_CONTROL_PLANE_DESIGN.md`](./STEAMOS_CONTROL_PLANE_DESIGN.md)

## 1. 決定事項

SteamShineの優先順位を以下で固定する。

| 優先度 | 機能 | 方針 |
|---:|---|---|
| P0 | Artemis／Moonlightへのゲーム配信 | 最優先。低遅延、高画質、安定性を他機能より優先する |
| P0 | 仮想Gamescopeデスクトップ | クライアント解像度・FPSへ自動一致し、物理画面やダミープラグに依存しない |
| P1 | Web管理画面 | 全状態・設定・診断へアクセスできる単一管理画面とする。ただし映像データプレーンへ介入しない |
| P1 | 障害復旧 | 管理機能が停止しても配信を継続し、配信側が停止してもWebから原因を確認できる |
| P2 | SSH／Webターミナル／管理デスクトップ | 保守用。ゲーム配信のCPU、GPU、ネットワークを奪わない |
| P3 | Webブラウザからのゲームプレイ | 任意の将来機能。既定無効。P0/P1完成後に限り評価する |

**ブラウザゲーム配信のために、Artemis／Moonlight配信の品質、遅延、安定性を下げてはならない。**

## 2. 非交渉要件

1. ゲーム映像の標準経路は以下とする。

   ```text
   Game / Steam
      → Headless Gamescope virtual session
      → PipeWire / DMA-BUF capture
      → AMD hardware encoder
      → GameStream compatible transport
      → Artemis / Moonlight
   ```

2. Webサーバーは管理コントロールプレーンであり、通常の映像フレームを中継しない。
3. Webサーバー、SQLite、guacd、tmux、履歴処理が停止・遅延しても進行中のゲーム配信を止めない。
4. Web管理画面から、秘密値そのものを除く全運用情報へアクセスできる。
5. 高頻度データは原則メモリ上に保持し、SSDへの書き込みを集約する。
6. 監視のために毎秒`amd-smi`、`rocm-smi`、`sensors`等の外部プロセスを起動しない。
7. Web管理だけを開いている状態では、追加の映像キャプチャ、GPUコピー、ハードウェアエンコーダーセッションを作らない。
8. 配信中は自動的に`stream_priority_mode`へ入り、非重要処理を抑制または延期する。

## 3. 全体アーキテクチャ

```text
                           ┌────────────────────────────┐
                           │ Browser / PWA              │
                           │ Settings / Metrics / Logs  │
                           │ Terminal / Remote Desktop  │
                           └──────────────┬─────────────┘
                                          │ HTTPS / WSS
                                          ▼
┌────────────────────────────────────────────────────────────────┐
│ SteamShine Web / Auth Gateway                                  │
│ - login / RBAC / CSRF / Origin / rate limit                    │
│ - REST / WebSocket proxy                                       │
│ - static Web UI                                                │
└──────────────────┬───────────────────────────────┬──────────────┘
                   │ internal authenticated IPC    │
                   ▼                               ▼
┌──────────────────────────────┐       ┌───────────────────────────┐
│ steamshine-control           │       │ steamshine stream daemon │
│ - metrics / history          │       │ - GameStream              │
│ - GPU policy                 │       │ - Gamescope session       │
│ - terminal / guacd broker    │       │ - capture / encode        │
│ - health / alerts / audit    │       │ - audio / input           │
└───────────┬──────────────────┘       └─────────────┬─────────────┘
            │                                        │
            │ Unix socket                            │ UDP / TCP
            ▼                                        ▼
┌──────────────────────────────┐          Artemis / Moonlight
│ steamshine-hw-helper         │
│ fixed AMD GPU operations     │
└──────────────────────────────┘
```

### 3.1 データプレーン分離

配信スレッドは次の処理を直接行わない。

- SQLite query／commit
- 履歴集計
- ログ圧縮
- guacd通信
- PTY／tmux通信
- DNS解決
- WebSocket fan-out
- GPU設定用root helper待ち
- 外部CLIによるセンサー取得

配信デーモンから管理面へ渡す情報は、非同期・有界・drop可能なテレメトリ経路を使用する。

### 3.2 内部テレメトリ経路

推奨順:

1. Unix domain socket + length-prefixed message
2. shared-memory snapshot + eventfd
3. lock-free single-producer ring buffer

初期実装はUnix socketでよい。ただし以下を必須とする。

- stream daemon側の送信はnon-blocking
- control側が遅い場合は古いtelemetryを破棄し、最新snapshotを優先
- queue上限を固定
- 送信失敗をゲーム配信のfatal errorにしない
- config変更要求とtelemetryを別channelにする
- request timeout後もstream threadを待たせない

## 4. Webサーバーからアクセスできる情報

「全ての情報へアクセスできる」とは、秘密値、暗号鍵、raw credential、root helper handleを除き、運用・診断・設定に必要な状態をWeb UI／APIで確認できることを意味する。

### 4.1 システム

- OS名、SteamOS build、kernel、Mesa、ROCm、AMD driver
- SteamShine version、commit、build options
- uptime、boot ID、timezone
- CPU model、core/thread数、frequency、load、usage、temperature
- memory、cache、swap、PSI
- mount、filesystem、容量、空き、read/write throughput
- network interface、link speed、RX/TX、drop、error
- relevant systemd user/system unit health
- suspend／resume履歴

### 4.2 GPU

GPUごとにPCI BDFをキーとして表示する。

- vendor、device、marketing name
- DRM card、render node
- driver、firmware、VBIOS hash（取得可能な場合）
- VRAM total／used
- GFX／memory／media utilization
- edge／hotspot／memory temperature
- fan RPM／PWM／control mode
- socket／board power
- current／minimum／maximum／default power cap
- SCLK／MCLK current and available levels
- DPM performance level
- encoder capability: H.264／HEVC／AV1、10-bit
- active encoder session count（取得可能な場合）
- applied profile、profile revision、last apply result
- thermal guard state
- read-only／write-capable capability

取得不能な値は`N/A`とし、GPU全体をエラーにしない。

### 4.3 SteamShine配信

- listener／discovery／pairing health
- paired client一覧
- client permission
- active GameStream session
- preparing／streaming／stopping／recovering state
- client ID、app、resolution、FPS、HDR、audio layout
- selected codec、profile、bit depth
- selected GPU／encoder backend
- capture backend、capture resolution、capture FPS
- encoded FPS、bitrate、keyframe、frame queue depth
- dropped／duplicated／late frames
- network packet statistics
- stream start time、reconnect count
- virtual Gamescope PID、Wayland display、Xwayland display
- PipeWire video node／audio sink health
- input device health
- local display policy
- cleanup status、stale resource count
- last failure stage and bounded error detail

秘密のpairing materialやcertificate private keyは表示しない。

### 4.4 仮想デスクトップ

- provider capability
- requested／effective resolution and refresh rate
- fallback reason
- Gamescope executable／version／supported options
- session runtime directory status
- child process tree
- PipeWire node mapping
- audio route
- input route
- client profile
- Gaming Mode切替状態
- restore checkpoint
- orphan recovery result

### 4.5 管理機能

- `steamshine-control` health
- metrics collector health and lag
- SQLite queue depth／last flush／last checkpoint
- current write rate
- terminal session一覧
- remote desktop connection一覧
- guacd／xrdp／VNC／SSH health
- alerts and acknowledgement
- audit event metadata
- backup／restore status
- feature flags

### 4.6 情報取得API

推奨API:

```text
GET /api/control/v1/overview
GET /api/control/v1/system/snapshot
GET /api/control/v1/system/history
GET /api/control/v1/gpus
GET /api/control/v1/gpus/{bdf}
GET /api/control/v1/stream/status
GET /api/control/v1/stream/sessions
GET /api/control/v1/virtual-sessions
GET /api/control/v1/services
GET /api/control/v1/storage/wear
GET /api/control/v1/logs
GET /api/control/v1/alerts
GET /api/control/v1/settings
GET /api/control/v1/capabilities
WS  /api/control/v1/events
WS  /api/control/v1/metrics
WS  /api/control/v1/stream/telemetry
```

APIはUIと同じRBACを使用する。API responseには以下を付与する。

```json
{
  "schema_version": 1,
  "generated_at": "ISO-8601",
  "source_timestamp": "ISO-8601",
  "stale": false,
  "partial": false,
  "data": {}
}
```

### 4.7 設定変更

Webから以下を変更可能にする。

- SteamShine一般設定
- client permission／profile
- virtual session mode
- encoder／codec preference
- resolution／FPS upper bound
- AMD GPU profile
- monitoring interval／retention
- alerts
- terminal／remote feature flag
- remote connection configuration
- log level

ただし以下を必須とする。

- validation
- capability check
- preview
- confirmation for disruptive changes
- revision／optimistic lock
- audit
- read-back verification
- rollback
- stream中に危険な変更を行う場合の明示警告

## 5. Artemis／Moonlight最優先経路

### 5.1 標準動作

1. Artemis／Moonlightから接続要求を受ける。
2. client width／height／FPS／HDRを検証する。
3. `stream_priority_mode`を有効にする。
4. 必要なAMD profileを適用する。
5. headless Gamescope virtual sessionを生成する。
6. PipeWire video／audio nodeを確定する。
7. DMA-BUF優先でcaptureする。
8. 単一のAMD hardware encoderでGameStream向けbitstreamを生成する。
9. Artemis／Moonlightへ配信する。
10. 切断時にsessionを破棄し、GPU／audio／Gaming Modeを復元する。

### 5.2 実装上の優先順位

次を完成させるまで、ブラウザゲーム配信を実装フェーズへ入れない。

- 1080p60 SDRの安定配信
- 1440p120 SDRの安定配信
- client resolution／FPS自動一致
- physical monitor OFF
- 50回接続／切断でorphan 0
- Steam Big Picture／Proton統合
- audio／input安定化
- GPU profile／monitoringの低負荷化
- Web管理から全状態を確認可能
- 1時間以上のstream soak test

### 5.3 Encoder ownership

標準では、active game sessionごとに必要最小数のencoder sessionだけを使用する。

- Web dashboard: encoderを作らない
- Remote Desktop管理画面: game encoderを共有・再設定しない
- Thumbnail: active stream frameを低頻度で再利用できる場合のみ。専用encoderを作らない
- Preview動画: 既定無効
- Browser game streaming: 既定無効

## 6. Webブラウザからのゲームアクセス

### 6.1 位置付け

ブラウザゲームアクセスは**可能であれば実装する将来機能**とし、初期完成条件へ含めない。

```text
web_game_streaming.enabled = false
```

既定では関連バイナリ、signaling、STUN／TURN、WebRTC encoder、追加captureを起動しない。

### 6.2 実装候補

将来評価する場合は、GameStreamをブラウザへ無理に解釈させるのではなく、独立したWebRTC出力を候補とする。

```text
Gamescope capture
   ├─ primary: GameStream → Artemis / Moonlight
   └─ optional: WebRTC RTP → Browser
```

ただし独立encoderを常時作る方式は禁止する。

### 6.3 性能保護ルール

ブラウザ配信を実装する場合も以下を守る。

1. feature flag既定無効。
2. P0配信の準備中／配信中は、既定ではブラウザ配信開始を拒否する。
3. 同時配信を許可するのは、GPU capabilityと実測余裕を確認した管理者設定時だけ。
4. browser signaling serverは未使用時にsleepし、定期pollをしない。
5. 同一codec／resolution／FPSを使える場合のみ、encoded bitstreamのmulti-sinkを検討する。
6. bitstream共有が不可能ならsecondary encoderを使うが、primary streamのframe queue、clock、power profileを変更しない。
7. browser session開始によってMoonlightのdrop frame、encode latency、game FPSが閾値を超えて悪化した場合はbrowser sessionを自動停止する。
8. TURN relayを使用する場合もgame network queueを優先する。
9. browser機能を無効化したビルド／配布を維持する。

### 6.4 Browser UIのゲーム映像を必須にしない

Web UIは、ゲーム映像がなくても以下を完全に実行できる。

- session開始／停止
- app選択
- client profile管理
- resolution／FPS設定
- GPU profile設定
- live metrics
- stream state／error確認
- log／diagnostic bundle
- virtual session recovery
- terminal／remote desktop

必要なら静止画thumbnailを表示できるが、既定では無効とし、active streamの既存frameから低頻度に生成する。高頻度JPEG生成や動画previewは行わない。

## 7. SSD消耗最小化

### 7.1 原則

- raw video、encoded frame、audio packetをSSDへ書かない。
- stream replay bufferはRAMのみ。
- per-frame telemetryを永続化しない。
- 1秒metricをそのままSQLiteへ書かない。
- terminal出力をserver logやSQLiteへ複製しない。
- debug logを既定無効にする。
- 一時session stateは`$XDG_RUNTIME_DIR`または`/run/user/<uid>`へ置く。

### 7.2 データ分類

| データ | 保存先 | 既定保持 |
|---|---|---:|
| 最新metrics | RAM snapshot | 1件 |
| 1〜2秒raw metrics | RAM ring | 15分 |
| 10秒集計 | RAM ring | 6時間 |
| 1分集計 | SQLite metrics DB | 30日 |
| 1時間集計 | SQLite metrics DB | 1年 |
| stream per-frame stats | RAM ring | 15分 |
| stream session summary | SQLite state DB | 90日 |
| config／client profile | SQLite config DB | 永続 |
| audit metadata | SQLite audit DB | 設定値に従う |
| terminal scrollback | tmux memory／tmux server | session lifetime |
| transient Gamescope state | runtime tmpfs | session lifetime |
| warning／error log | bounded file or journal | rotate |

### 7.3 DB分離

頻繁に変わるmetricsと重要設定を別DBへ分離する。

```text
config.db   : users, permissions, settings, client profiles, remote definitions
audit.db    : security and change audit
metrics.db  : minute/hour aggregates, stream summaries
```

目的:

- metrics WAL churnがconfig durabilityへ影響しない
- metrics DBを削除／再生成可能にする
- metricsだけ低durability設定を選べる
- backup頻度を分ける

### 7.4 SQLite書き込み方針

#### config／audit

- transaction単位で即時commit
- WAL
- durability優先
- schema migration前にbackup

#### metrics

- 1分値をメモリで集約
- 5分分を1 transactionでbatch insertしてよい
- active stream中は最大5分までflushを延期可能
- clean shutdownでflush
- `synchronous=NORMAL`を候補とする
- checkpoint、vacuum、retention deleteはstream終了後またはidle時
- `VACUUM`を定期自動実行しない
- incremental cleanupを使用
- page count／WAL sizeに上限を設ける

クラッシュ時に数分のmetricsが失われても、ゲーム配信や設定は失われない設計とする。

### 7.5 書き込み予算

既定設定のSteamShine自身による書き込み量は、ゲーム／Steam／OS更新を除き、以下を目標とする。

```text
通常運用: 50 MiB/day未満
配信中:   10 MiB/hour未満
```

この値は受入試験で`/proc/<pid>/io`、cgroup I/O、filesystem countersを使って実測する。

上限を超えた場合は、次を順に削減する。

1. debug／info log
2. metrics flush頻度
3. raw history保持
4. session event詳細
5. diagnostic sampling

config、security audit、error recordは削減対象の最後にする。

### 7.6 ログ

- default level: warning
- normal session lifecycleはstructured eventとしてメモリringへ保持
- warning／errorだけ永続化
- 同一エラーのrate limit／deduplicate
- 1 message最大長
- 1 file最大サイズ
- 世代数上限
- 圧縮はstream中に行わない
- crash dumpはopt-in
- credential、clipboard、terminal本文、pairing secretを記録しない

### 7.7 zero-write gameplay mode

設定:

```text
storage.zero_write_during_stream = true
```

有効時、active stream中は以下をRAMへ保留する。

- minute metrics
- session noncritical events
- UI preference updates
- log rotation
- history cleanup
- update checks result

以下は例外として即時保存できる。

- security audit
- administratorによる設定変更
- GPU apply／rollback failure
- fatal stream failure summary
- explicit bookmark／diagnostic request

保留データは終了後に1 transactionでflushする。上限を超えた場合は古いnoncritical metricsを捨てる。

## 8. CPU・GPU・メモリ負荷最小化

### 8.1 監視取得

優先順:

1. sysfs／hwmon／procfs
2. stream daemon内部counter
3. PipeWire／DRM API
4. amd-smi library APIが利用可能な場合
5. `amd-smi` CLI fallback
6. `rocm-smi` CLI fallback

CLI fallbackはcapability再検出、手動診断、低頻度health checkに限定する。

### 8.2 Adaptive sampling

推奨既定値:

```text
no browser viewing, idle host:       5 s
Web dashboard visible:               1 s
Moonlight/Artemis streaming:         2 s
thermal guard:                       1 s
stream internal counters:            producer-native, RAM only
historical persistence:              60 s aggregate
```

WebSocket subscriberがいないときはUI向け1秒samplingを行わない。

配信中も温度保護は維持するが、process list、filesystem scan、service discovery等の重い項目は10〜30秒へ落とす。

### 8.3 Process priority

`steamshine-control`:

- `Nice=10`
- `IOSchedulingClass=idle`またはbest-effort low priority
- bounded CPUQuotaを検討
- memory high／maxを設定
- OOM時はstream daemonより先に終了させる

`steamshine` stream daemon:

- encoder／capture threadを優先
- control IPC threadを低優先度
- DB／Web処理をリンクしない

`guacd`／xrdp／tmux:

- stream daemonと別cgroup
- active game stream時はCPU／I/O上限を厳しくする
- remote desktopは管理用途のFPS／quality上限を設ける

### 8.4 GPU負荷

- sensor readにGPU computeを使わない
- dashboard thumbnail用encoderを作らない
- GPU profile read-backを毎秒行わない
- profile変更後、driver reset後、resume後だけ詳細capability再取得
- hardware encoder能力確認をstream開始ごとに全codec総当たりしない
- capability cacheにdriver／kernel／GPU identity fingerprintを付ける
- active game stream中のGPU profile変更は、safe preset以外を拒否または明示確認する

### 8.5 ネットワーク

- GameStream socketをWebSocket、download、remote desktopより優先する
- diagnostic bundle downloadはactive stream中に警告または帯域制限
- remote desktop image quality／FPSを管理用途向けに制限
- Web metricsは差分／compact encodingを使い、巨大snapshotを毎秒送らない
- history graphはrange queryで取得し、live WSへ混在させない
- slow Web client用queueを有界化し、古いmetricsをdropする

## 9. Stream Priority Mode

### 9.1 自動遷移

```text
Idle
  └─ Moonlight/Artemis prepare
       └─ StreamPriorityEntering
            └─ Preparing / Streaming
                 └─ Stopping
                      └─ StreamPriorityLeaving
                           └─ Idle
```

`Preparing`に入る前にpriority modeを有効化し、cleanup完了後に解除する。

### 9.2 配信中に延期する処理

- update check
- package discovery
- full process inventory
- filesystem recursive scan
- DB checkpoint
- DB vacuum
- log compression
- old history deletion
- backup
- diagnostic archive generation
- WebRTC browser service startup
- thumbnail generation
- remote connection health full scan

### 9.3 配信中も維持する処理

- thermal guard
- latest CPU／GPU／memory／network metrics
- stream telemetry
- health heartbeat
- GPU power read
- fatal error logging
- Web UI status
- stop／recover command
- SSH／terminal最小操作
- security audit

### 9.4 Web UI表示

active時に`ゲーム配信優先モード`を明示する。

表示例:

```text
Streaming Priority: ACTIVE
Primary client: Artemis / 2560x1440@120
Deferred tasks: history cleanup, update check, log compression
Control overhead: CPU 0.7%, disk write 0 B/s
```

ユーザーが重い処理を開始しようとした場合は、次を選べるようにする。

- 配信終了後に実行
- 今回だけ実行
- キャンセル

既定は「配信終了後に実行」とする。

## 10. Web管理の情報完全性と安全性

### 10.1 Single pane of glass

Web UIから以下の画面へ到達できる。

- Overview
- Streaming
- Virtual Displays
- Clients
- Applications
- GPU
- System
- Storage／SSD Write
- Network
- Audio／Input
- Logs／Diagnostics
- Alerts
- Terminal
- Remote Desktop
- Services
- Settings
- Audit

### 10.2 Secretの扱い

「全情報」と「秘密値表示」は同義ではない。

APIは以下を返さない。

- password
- private key
- passphrase
- session cookie
- TOTP secret
- pairing private material
- encryption master key
- raw sudo token

代わりに返す。

```json
{
  "has_password": true,
  "has_private_key": false,
  "credential_updated_at": "ISO-8601"
}
```

### 10.3 RBAC

推奨権限:

```text
system.view
stream.view
stream.control
virtual_session.view
virtual_session.control
gpu.view
gpu.manage
monitoring.view
monitoring.manage
terminal.use
remote_desktop.use
settings.manage
logs.view
diagnostics.run
audit.view
```

viewerでも秘密値以外の状態は確認できるが、GPU変更、stream停止、terminal、remote desktopは別権限とする。

## 11. 性能予算

対象実機でbaseline比較を行う。baselineは管理sidecar無効のSteamShineとする。

### 11.1 Idle

| 項目 | 目標 |
|---|---:|
| `steamshine-control` CPU平均 | 1%未満 |
| dashboard未表示時の外部CLI起動 | 0回/分 |
| 追加GPU encoder session | 0 |
| 追加GPU utilization | 測定誤差範囲 |
| resident memory | 200 MiB未満 |
| disk write | 50 MiB/day未満 |

### 11.2 Dashboard表示中

| 項目 | 目標 |
|---|---:|
| metrics更新 | 1秒以内 |
| control CPU平均 | 3%未満 |
| GPU encoder session | 0 |
| WebSocket backlog | 有界、slow clientでcollector停止なし |

CPU%は対象機の全論理CPUに対する値として計測方法を文書化する。

### 11.3 Artemis／Moonlight配信中

| 項目 | 目標 |
|---|---:|
| 追加hardware encoder | 0（browser streaming無効時） |
| control CPU平均 | 2%未満 |
| SteamShine自身のdisk write | 10 MiB/hour未満 |
| capture／encode frame drop増加 | baseline比 +0.1 percentage point未満 |
| encode latency悪化 | baseline比 +3%または+0.3msの大きい方未満を目標 |
| game FPS／frametime | 統計的に有意な悪化なし |
| control crash時 | stream継続 |
| WebSocket slow client | streamへの影響なし |

ハードウェア、ゲーム、driverにより変動するため、閾値は「目標」と「release gate」を分けて実測で確定する。

## 12. A/B性能試験

同一ゲーム、同一scene、同一解像度、同一codec、同一bitrateで以下を比較する。

```text
A: stream daemon only
B: + control sidecar idle
C: + dashboard open
D: + terminal idle
E: + remote desktop idle
F: + metrics persistence
```

計測:

- game average／1% low／0.1% low FPS
- CPU frametime／GPU frametime
- capture latency
- encode latency
- network send latency
- dropped frames
- GPU utilization／power／temperature
- CPU utilization
- process context switches
- disk read／write bytes
- network bytes per service
- WebSocket queue depth

各ケースを複数回実行し、warm-upを除外する。

release gate:

- B〜FのいずれかでP0指標が閾値を超えて悪化したら、管理機能側を最適化または無効化する。
- P0を犠牲にして機能を残さない。

## 13. 設定スキーマ

```toml
[priority]
primary_stream = "gamestream"
stream_priority_mode = true
pause_noncritical_tasks_during_stream = true
reject_optional_encoder_during_stream = true

[web]
enabled = true
expose_all_operational_information = true
live_metrics_interval_ms = 1000
slow_client_policy = "drop_old_keep_latest"

[web_game_streaming]
enabled = false
allow_during_gamestream = false
implementation = "none"

[monitoring]
idle_interval_ms = 5000
dashboard_interval_ms = 1000
streaming_interval_ms = 2000
thermal_interval_ms = 1000
raw_memory_retention_minutes = 15
minute_retention_days = 30
hour_retention_days = 365

[storage]
zero_write_during_stream = true
metrics_batch_minutes = 5
max_runtime_ring_bytes = 67108864
max_log_bytes = 33554432
log_level = "warning"

[remote_desktop]
enabled = false
max_fps_during_stream = 15
max_bandwidth_mbps_during_stream = 8

[terminal]
enabled = false
max_sessions = 4
```

## 14. 実装フェーズへの反映

既存のVirtual Session PhaseとControl Plane C0〜C7へ以下を追加する。

### S0: Priority contract

- 本文書の設定schema
- stream priority state
- feature flags
- performance baseline harness
- disk write measurement

### S1: Telemetry bridge

- non-blocking internal IPC
- stream snapshot
- bounded queue
- stale／partial semantics
- control crash isolation test

### S2: Web information coverage

- Overview／Streaming／GPU／Virtual Session API
- full capability view
- service health
- source timestamp／stale display
- secrets redaction test

### S3: Storage minimization

- DB split
- RAM rings
- batch metrics persistence
- zero-write gameplay mode
- bounded logs
- write-budget test

### S4: Stream priority scheduler

- deferred task queue
- adaptive sampling
- cgroup／nice／I/O priority
- active stream throttling for guacd／remote desktop
- UI priority indicator

### S5: Optional browser spike

以下をすべて満たした後だけ着手可能。

- P0 complete
- P1 complete
- 1440p120 stable
- A/B performance gate pass
- GPU encoder headroom measured
- browser implementation can be fully disabled

Spikeの成果がP0を悪化させる場合は採用しない。

## 15. テスト追加

### 15.1 Web情報

- 全APIにschema version／timestamp
- GPU／stream／virtual sessionの欠測表示
- control sidecar restart後の再同期
- stream daemon停止時の診断
- secret non-disclosure
- RBAC
- slow WebSocket subscriber

### 15.2 SSD

- 24時間模擬metrics
- WAL size bound
- batch transaction count
- stream中zero-write
- crash後metrics欠損がconfigへ波及しない
- retention cleanupがstream中に走らない
- log storm rate limit

### 15.3 性能

- control disabled／enabled比較
- dashboard open／closed
- 1080p60／1440p120
- H.264／HEVC／AV1
- remote desktop接続中
- terminal大量出力中
- history query中
- SSD slow／full simulation
- control sidecar CPU saturation
- control sidecar kill -9

### 15.4 Browser optional guard

- feature disabled時にlistener／thread／encoderが存在しない
- active GameStream中にbrowser startを既定拒否
- secondary encoderのcapability不足時に拒否
- browser overload時にbrowserだけ停止
- browser crashがGameStreamへ影響しない

## 16. 完成定義

以下を満たした時点で本ポリシーの初期実装を完成とする。

- Artemis／Moonlightが標準かつ最優先のゲーム配信クライアントである。
- 仮想Gamescope sessionで物理モニターOFFから接続できる。
- Web UIから秘密値を除く全運用情報、設定、health、diagnosticへアクセスできる。
- Web UI／control sidecarを停止しても進行中のstreamが継続する。
- Web管理だけでは追加encoder sessionを作らない。
- high-rate metricsはRAM中心であり、1秒ごとのDB commitを行わない。
- 既定のSteamShine自身のSSD write budgetを満たす。
- 配信中にcheckpoint、vacuum、log compression、update checkを行わない。
- monitoring／terminal／remote desktopを有効化してもP0性能gateを満たす。
- Web browser game streamingは既定無効で、初期完成条件に含まれない。
- optional browser機能を追加しても無効時のoverheadがゼロに近い。

---

SteamShineは「多機能な管理サーバー」より先に「低遅延・高品質なゲーム配信ホスト」である。Web管理、GPU制御、監視、SSH、リモートデスクトップは、その配信を支え、診断し、復旧するための機能として設計する。機能追加と配信品質が競合した場合は、常にArtemis／Moonlightへの仮想デスクトップ配信を優先する。
