# SteamShine: SteamOS管理・遠隔操作・監視機能 詳細設計

- 状態: 実装前設計
- 対象: SteamOS / Arch系Linux、AMD GPUを主対象
- 作成日: 2026-07-23
- 関連文書: [`STEAMOS_VIRTUAL_SESSION_PLAN.md`](./STEAMOS_VIRTUAL_SESSION_PLAN.md)
- 移植元: [`souten-yd/ControlDeck`](https://github.com/souten-yd/ControlDeck)

## 1. 目的

SteamShineへ、ゲームストリーミングとは独立して利用できるSteamOS管理機能を追加する。

対象機能は以下の3系統とする。

1. AMD GPUの設定変更
   - GPUごとの電力上限
   - コア／VRAMクロック方針
   - 対応GPUに限るファン制御
   - 静音、標準、性能優先、カスタムのプロファイル
   - ストリーミング開始前の自動適用と終了後の復元
2. SSH／Webターミナル／リモートデスクトップ
   - ブラウザから操作できる永続Webターミナル
   - RDP、VNC、SSH接続の登録とブラウザ操作
   - SteamOS自身を保守するためのヘッドレス管理デスクトップ
   - iPhone／タブレット向けのタッチパッド操作
3. リソースモニター
   - CPU、メモリ、GPU、VRAM、温度、ファン、電力、クロック
   - ディスク、ネットワーク、プロセス、SteamShine配信状態
   - リアルタイム表示、履歴、警告、障害診断

これらはControlDeckの実装を挙動上の基準として移植する。ただしSteamShineはゲームストリーミングサーバーであるため、管理機能の障害や高負荷が映像配信を停止させない構造へ改める。

## 2. 非目標

初期実装では以下を対象外とする。

- AMD GPUの電圧変更、VBIOS変更、ドライバ上限を超えるオーバークロック
- 対応可否を確認せずに任意のsysfsファイルへ書き込む機能
- ブラウザからrootシェルへ自動ログインする機能
- インターネットへ直接公開することを前提としたリモート管理
- RDPを低遅延ゲーム配信の代替として使うこと
- 1秒ごとの全メトリクスを無期限にSSDへ保存すること
- GUIから任意のsystemdユニットや任意のrootコマンドを実行すること
- GPUドライバやカーネルの熱保護を無効化すること

## 3. ControlDeckからの移植基準

2026-07-23時点のControlDeck `main`を挙動基準とする。直接コードを移す場合は、両リポジトリのライセンス表記と第三者由来コードの通知を事前に確認する。

主な参照実装は以下とする。

| 機能 | ControlDeck参照元 | SteamShineでの扱い |
|---|---|---|
| AMD GPU能力検出・設定適用 | `backend/app/models_mgmt/amd_gpu.py` | C++の能力検出層と最小権限helperへ分割 |
| GPUメトリクス | `backend/app/monitoring/gpu.py` | sysfs優先の複数GPU対応providerへ拡張 |
| CPU／メモリ／I/O収集 | `backend/app/monitoring/collector.py` | 管理sidecarのcollectorへ移植 |
| 永続ターミナル | `backend/app/terminals/manager.py` | tmux session managerへ移植 |
| ターミナル再接続 | `backend/app/terminals/router.py`, `stream.py` | sequence journalと差分再送を維持 |
| Guacamoleトンネル | `backend/app/remote_desktop/guacd.py` | 命令境界を維持するBoost.Asio実装へ移植 |
| リモート接続管理 | `backend/app/remote_desktop/router.py`, `service.py` | 暗号化SecretStoreと接続先制限を追加 |
| ターミナルUI | `frontend/src/pages/Terminal.tsx`, `features/terminal/*` | Vueへ移植しSteamShine UIへ統合 |
| リモートUI | `frontend/src/pages/Remote.tsx`, `features/remote/RemoteViewer.tsx` | Vueへ移植しモバイル操作を維持 |

関連コミット:

- AMD GPU静音プロファイルと起動前制御: `8f2921e1197e64f02a29fae1107ba4a3945e3a47`
- Webターミナル初期実装: `506cdc8a26db6b63c74e66028ff0d57132aa0880`
- Guacamoleリモートデスクトップ初期実装: `2ef679ebc44586081ad204afd7c9abce74837a93`

## 4. 最重要設計判断

### 4.1 配信データプレーンと管理コントロールプレーンを分離する

SteamShine本体のゲーム配信経路へ、PTY、guacd、履歴DB、重いセンサー取得を直接混在させない。

```text
Moonlight / Artemis
       │
       ▼
SteamShine Stream Daemon
  - GameStream protocol
  - capture / encode
  - input
  - virtual Gamescope session
       │
       │ authenticated internal proxy
       ▼
steamshine-control（非root管理sidecar）
  - metrics collector
  - GPU policy controller
  - terminal broker
  - guacd tunnel broker
  - history / alerts / audit
       │
       ├─ Unix socket ─► steamshine-hw-helper（root、固定操作のみ）
       ├─ loopback TCP ─► guacd
       ├─ PTY / tmux
       └─ SQLite WAL
```

必須条件:

- `steamshine-control`が停止、再起動、クラッシュしても既存のゲーム配信を継続する。
- 管理WebSocketの輻輳が映像／音声ストリームのスレッドをブロックしない。
- GPU制御helperが停止しても監視とゲーム配信は継続する。
- 管理機能は機能フラグで完全無効化できる。

### 4.2 管理sidecarは同一リポジトリの独立バイナリとする

推奨バイナリ名:

```text
steamshine
steamshine-control
steamshine-hw-helper
```

`steamshine-control`は通常ユーザーで実行し、同一リポジトリのCMakeからビルドする。初期PoCでControlDeckのPython実装を参照用sidecarとして使うことは許容するが、配布版は追加Python環境に依存しない構成を目標とする。

### 4.3 ブラウザ認証はSteamShine本体へ集約する

外部へ待受するHTTPS／WSSはSteamShine本体だけとする。

- SteamShine本体がログイン、セッション、CSRF、Origin、RBACを判定する。
- `steamshine-control`はUnix domain socketだけで待受する。
- Unix socketはSteamShineユーザーのみ読み書き可能な`0600`とする。
- sidecarは`SO_PEERCRED`で接続元UIDを検証する。
- HTTPリクエストには、ユーザーID、権限、request ID、時刻、nonceを含む短寿命の署名済みinternal envelopeを付与する。
- WebSocketは30秒有効、1回限りの接続ticketを発行して引き渡す。
- sidecarへブラウザCookieやパスワードを転送しない。

### 4.4 root権限は小さなhelperへ閉じ込める

Webサーバー、配信サーバー、管理sidecarをrootで実行しない。

root helperが許可する操作は、コード内allowlistにあるGPU設定だけとする。任意コマンド、任意パス、任意systemctlを受け付けない。

## 5. 機能フラグ

初回導入時の既定値は安全側とする。

```toml
[control]
enabled = true

[monitoring]
enabled = true
history_enabled = true
alerts_enabled = false

[gpu_control]
enabled = false
apply_on_stream_start = false
restore_on_stream_end = true

[terminal]
enabled = false

[remote_desktop]
enabled = false
allow_external_hosts = false
```

機能が無効な場合は、API、WebSocket、メニュー、バックグラウンドタスクを登録しない。

## 6. 権限モデル

### 6.1 権限

| 権限 | 内容 |
|---|---|
| `metrics.view` | 現在値、履歴、配信統計の閲覧 |
| `alerts.manage` | 警告ルールの作成・更新・削除 |
| `gpu.view` | GPU能力、現在設定、温度、クロックの閲覧 |
| `gpu.manage` | GPUプロファイルの保存・適用・復元 |
| `terminal.use` | 自分のWebターミナル作成・接続 |
| `terminal.kill` | 任意の管理対象ターミナル終了 |
| `remote.view` | 保存済み接続一覧の閲覧 |
| `remote.connect` | 保存済み接続への接続 |
| `remote.manage` | 接続先と認証情報の登録・更新・削除 |
| `audit.view` | 監査ログ閲覧 |

### 6.2 標準ロール

| ロール | 既定権限 |
|---|---|
| Viewer | `metrics.view`, `gpu.view` |
| Operator | Viewer + `terminal.use`, `remote.view`, `remote.connect` |
| Administrator | 全権限 |

GPU設定変更、Secret更新、公開IP接続許可などの危険操作は、Administratorか明示的な同等権限を持つ利用者に限定する。

### 6.3 監査

記録対象:

- GPUプロファイル作成、更新、削除、適用、復元、失敗、緊急制御
- ターミナル作成、接続、切断、終了
- リモート接続設定の作成、更新、削除、接続開始、接続終了、失敗
- 警告ルール変更
- Secret key生成、ローテーション、復旧

記録しない情報:

- ターミナルへ入力したコマンド本文
- キーストローク
- パスワード、秘密鍵、passphrase
- クリップボード本文
- GPU helperの環境変数

## 7. AMD GPU設定変更機能

## 7.1 目的

AMD GPUごとに、ドライバが公開している安全な範囲内で動作プロファイルを変更する。

主な利用例:

- ゲーム配信時だけGPU電力上限を引き上げる。
- 軽いゲームでは静音プロファイルを適用する。
- ヘッドレス待機中は既定値へ戻す。
- クライアント、ゲーム、エンコーダーに応じてプロファイルを切り替える。

## 7.2 対応機能

初期対応:

- 電力上限
- `power_dpm_force_performance_level`
- 利用可能なSCLK level mask
- 利用可能なMCLK level mask
- driver defaultへの復元
- 現在値のread-back確認
- 複数AMD GPU

条件付き対応:

- fan RPM／PWM読取
- `pwm1_enable`と`pwm1`が安全に利用可能なGPUの手動ファン
- 温度点とPWM点からなるファンカーブ

対象外:

- 電圧オフセット
- VBIOS編集
- ドライバ公開範囲外のクロック
- GPU resetの自動実行

## 7.3 GPU識別

`card0`、`card1`は起動ごとに変わり得るため永続キーにしない。

永続識別子:

```text
PCI BDF: 0000:0a:00.0
vendor_id: 0x1002
device_id
subsystem_vendor_id
subsystem_device_id
DRM render node
VRAM total
```

設定の主キーはPCI BDFとする。起動時にBDFから現在のDRM card、render node、hwmonを再解決する。

仮想セッションで使用するGPUは、SteamShineのencoder adapter／render nodeからBDFへ逆引きする。自動選択に失敗した場合だけ、管理者がBDFを明示する。

## 7.4 能力検出

優先順位:

1. `/sys/class/drm/card*/device`と`hwmon`の読み取り
2. `amd-smi static --json`または相当の静的情報
3. `amd-smi metric --json`
4. `rocm-smi --json`
5. 読み取り専用として最低限のDRM情報

代表的なsysfs項目:

```text
vendor
device
mem_info_vram_total
mem_info_vram_used
gpu_busy_percent
power_dpm_force_performance_level
pp_dpm_sclk
pp_dpm_mclk
hwmon/*/power1_cap
hwmon/*/power1_cap_min
hwmon/*/power1_cap_max
hwmon/*/power1_cap_default
hwmon/*/power1_average
hwmon/*/temp*_input
hwmon/*/temp*_crit
hwmon/*/fan1_input
hwmon/*/pwm1
hwmon/*/pwm1_enable
```

項目の存在だけで書込み可能と判断しない。実際の権限、値形式、driver generation、現在のperformance levelを検査する。

## 7.5 データモデル

```cpp
struct gpu_device_id_t {
  std::string pci_bdf;
  uint16_t vendor_id;
  uint16_t device_id;
  std::string render_node;
  uint64_t vram_total_bytes;
};

struct clock_level_t {
  uint32_t level;
  uint32_t mhz;
  bool current;
};

struct gpu_capabilities_t {
  gpu_device_id_t id;
  std::optional<uint32_t> power_min_w;
  std::optional<uint32_t> power_max_w;
  std::optional<uint32_t> power_default_w;
  std::vector<clock_level_t> sclk_levels;
  std::vector<clock_level_t> mclk_levels;
  bool performance_level_writable;
  bool fan_readable;
  bool fan_manual_writable;
  std::string control_backend;
  uint64_t capability_revision;
};

enum class clock_policy_t {
  automatic,
  minimum,
  maximum,
  level_mask
};

enum class fan_policy_t {
  automatic,
  fixed_pwm,
  curve
};

struct gpu_profile_t {
  std::string id;
  std::string name;
  std::string pci_bdf;
  std::optional<uint32_t> power_limit_w;
  clock_policy_t core_policy;
  std::vector<uint32_t> allowed_sclk_levels;
  clock_policy_t memory_policy;
  std::vector<uint32_t> allowed_mclk_levels;
  fan_policy_t fan_policy;
  std::optional<uint32_t> fixed_pwm_percent;
  std::vector<std::pair<float, uint32_t>> fan_curve;
  bool restore_driver_default_on_stop;
  uint64_t expected_capability_revision;
};
```

## 7.6 プロファイル

UI上は以下を用意する。

- Driver Default
- Quiet
- Balanced
- Performance
- Custom

プリセット値を全GPU共通の固定W数にしない。初回作成時に検出したmin／default／max、clock levelsから明示値を生成し、その値をプロファイルへ保存する。

安全な既定:

- Quiet: driver default以下の電力、clockはauto、fanはauto
- Balanced: driver default、clockはauto、fanはauto
- Performance: driver default、利用可能な高performance policy。ただし電力上限を自動でdriver maxへ上げない
- Custom: driver min〜maxの範囲で管理者が指定

driver defaultを超える電力値は、driverが公開するmax以内でも危険操作として再確認を要求する。

## 7.7 適用優先順位

同じGPUへ複数ポリシーが競合した場合は以下を使用する。

```text
Thermal Emergency
  > Administrator Temporary Override
  > Active Stream Session Profile
  > Per-client Profile
  > Per-app Profile
  > Global Profile
  > Driver Default
```

複数配信セッションを将来対応した場合、同一GPUでは最も保守的な共通設定か、管理者が選んだ競合ポリシーを適用する。後着セッションが無条件に設定を上書きしてはならない。

## 7.8 適用トランザクション

```text
Preview
  └─ capability revision確認
      └─ per-GPU mutex取得
          └─ 現在状態snapshot
              └─ 全値validation
                  └─ helperへ固定operation送信
                      └─ read-back verification
                          ├─ success: last-known-good保存、監査
                          └─ failure: snapshotへrollback、監査、UIへ理由
```

適用順序:

1. fanをautoへ戻す必要がある場合は先に戻す。
2. performance levelをautoへ戻す。
3. power capを設定する。
4. manual performance levelへ切り替える。
5. SCLK／MCLK maskを設定する。
6. fan policyを最後に設定する。
7. 全項目を再読取し、要求値と一致するか確認する。

rollbackも同じ検証済みoperationだけで行う。

## 7.9 root helper

推奨配置:

```text
/usr/local/libexec/steamshine-hw-helper
/run/steamshine/hw-helper.sock
```

推奨方式はsystemd socket activationとする。

helperが受け付けるoperation例:

```json
{
  "version": 1,
  "request_id": "uuid",
  "operation": "apply_gpu_profile",
  "pci_bdf": "0000:0a:00.0",
  "power_limit_w": 210,
  "performance_level": "manual",
  "sclk_levels": [0, 1, 2],
  "mclk_levels": [0, 1],
  "fan": {"mode": "auto"}
}
```

禁止事項:

- shell文字列を受け取らない。
- 任意ファイルパスを受け取らない。
- BDFから解決したAMD GPU配下以外へ書き込まない。
- `system()`、`popen()`、`sh -c`を使わない。
- client指定の実行ファイルを起動しない。
- helperへ環境変数を継承しない。

検証:

- peer UID
- request schema version
- nonce／request IDの再利用
- BDFが現存するAMD GPUであること
- power min／max
- clock levelが現在のcapabilityに含まれること
- fan capabilityと安全下限
- operation rate limit

systemd hardening候補:

```ini
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_UNIX
RestrictNamespaces=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
SystemCallArchitectures=native
```

必要なsysfs書込みだけ`ReadWritePaths=`へ列挙する。GPUごとに動的パスが必要な場合は、広い`/sys`書込みを許可せず、起動時に検証済みbind mountか専用udev権限方式を評価する。

## 7.10 ファン制御安全策

ファン制御はcapabilityが確認できたGPUだけ表示する。

- `fan1_input`しかない場合はread-only。
- `pwm1`と`pwm1_enable`があってもmanual mode遷移を検証できない場合は無効。
- 0%固定を既定で許可しない。
- 最低PWM、最高温度点、ヒステリシスを必須とする。
- helper切断、SteamShine終了、suspend前、GPUエラー時はfan autoへ戻す。
- driverの`temp*_crit`／`temp*_emergency`を超える設定を許可しない。
- カーブ計算が停止した場合は一定時間内にautoへ戻すwatchdogをhelper側へ置く。

## 7.11 温度緊急制御

固定温度だけに依存せず、hwmonが公開するcritical値を優先する。

状態:

```text
NORMAL
WARNING
THROTTLING
EMERGENCY
SENSOR_STALE
```

例:

- WARNING: 設定しきい値を継続時間超過
- THROTTLING: hotspotがcritical近傍、quiet emergency profile適用
- EMERGENCY: critical到達、fan auto、clock auto、電力を安全値へ復元、新規配信開始を拒否
- SENSOR_STALE: 温度が取得不能。手動fanを解除し、危険なcustom profileの新規適用を拒否

カーネル／GPU firmwareの熱保護は常に最優先とし、SteamShineは補助制御だけを行う。

## 7.12 ライフサイクル

- 起動時: capability再検出、保存済みプロファイルmigration、前回異常終了の復旧
- 配信準備前: 対象GPUへsession profileを適用しread-back完了後にencoderを開始
- 配信中: 温度、電力、clock、throttleを監視
- 正常終了: 残るsession policyを再計算し、不要ならglobalまたはdefaultへ復元
- 異常終了: state fileとlast-known-goodから復元
- suspend前: fan auto、必要に応じdriver default
- resume後: capability revisionを再取得してから再適用
- GPU reset／driver reload後: 古いlevel番号を信用せず再検出

## 7.13 API

```text
GET    /api/control/v1/gpus
GET    /api/control/v1/gpus/{bdf}
GET    /api/control/v1/gpus/{bdf}/state
GET    /api/control/v1/gpu-profiles
POST   /api/control/v1/gpu-profiles
PATCH  /api/control/v1/gpu-profiles/{id}
DELETE /api/control/v1/gpu-profiles/{id}
POST   /api/control/v1/gpu-profiles/{id}/preview
POST   /api/control/v1/gpu-profiles/{id}/apply
POST   /api/control/v1/gpus/{bdf}/restore-default
GET    /api/control/v1/gpu-events
```

変更APIは`If-Match`またはrevisionを要求し、古い画面からの上書きを409で拒否する。

`preview`は書込みを行わず、以下を返す。

```json
{
  "valid": true,
  "changes": [
    {"field": "power_limit_w", "from": 300, "to": 210},
    {"field": "memory_policy", "from": "automatic", "to": "minimum"}
  ],
  "warnings": [],
  "requires_confirmation": false,
  "capability_revision": 42
}
```

## 7.14 Web UI

GPU画面:

- GPUカードをBDF／製品名／VRAMで表示
- 温度、hotspot、使用率、VRAM、電力、fan、SCLK、MCLKをライブ表示
- 現在プロファイル、要求値、read-back値を分けて表示
- 対応しない設定項目は非表示ではなく「このGPUでは利用不可」と理由表示
- 変更は即時適用せず、preview画面で差分を確認
- driver default超過、manual fan、performance固定は警告と再確認
- 「既定値に戻す」を常に表示
- 配信中の設定変更は、現在のsession profileとの競合を表示

## 8. リソースモニター

## 8.1 収集方針

通常収集は外部CLIを周期起動せず、`/proc`、`/sys`、hwmon、SteamShine内部統計を優先する。`amd-smi`／`rocm-smi`は能力補完と診断要求時だけ使用する。

失敗したセンサーは`N/A`にし、他のメトリクス収集を継続する。

## 8.2 共通メトリクス形式

```cpp
template<typename T>
struct metric_value_t {
  std::optional<T> value;
  std::string unit;
  std::string source;
  std::chrono::system_clock::time_point sampled_at;
  enum class quality_t { good, estimated, stale, unavailable, error } quality;
};
```

APIでは値だけでなく、単位、取得元、品質、最終更新時刻を返す。

## 8.3 収集項目

### CPU

- total utilization
- per-core utilization
- load average
- current／min／max frequency
- package temperature
- core temperatures
- CPU fan RPM
- process count
- uptime

### メモリ

- total／used／available／cached
- swap total／used
- memory pressureが取得可能な場合はPSI

### AMD GPU（全GPU）

- PCI BDF、製品名、render node
- GPU utilization
- VRAM used／total
- GTT used／total（取得可能な場合）
- edge temperature
- junction／hotspot temperature
- memory temperature
- socket／board power
- power cap
- SCLK／MCLK
- performance level
- fan RPM
- fan PWM percent
- throttle／busy reason
- encoder utilization、encoder sessions（取得可能な場合）
- 現在のSteamShine GPU profile

### ディスク

- filesystem total／used／free
- read／write bytes per second
- read／write IOPS
- queue／latencyが取得可能な場合
- NVMe／SATA temperatureがhwmonで取得可能な場合
- read-only、I/O error、空き容量警告

### ネットワーク

- interface state
- link speed
- RX／TX bytes per second
- packet rate
- drops／errors
- SteamShineが使用しているinterface
- Tailscale interfaceの有無

### SteamShine配信

- active sessions
- client ID／app／codec／resolution／FPS
- capture FPS
- encode FPS
- encode latency
- packet loss／retransmit／network latencyで取得可能な値
- dropped／duplicated frames
- bitrate
- Gamescope virtual session state
- PipeWire video／audio node state
- encoder adapter BDF

### プロセス

- CPU上位
- RSS上位
- GPU／VRAM使用量が取得可能な場合
- SteamShine、Gamescope、Steam、game、guacd、xrdp、tmuxの状態

## 8.4 サンプリング周期

| 種類 | 周期 | 備考 |
|---|---:|---|
| CPU／memory／GPU busy／network | 1秒 | live表示用 |
| 温度／fan／power／clock | 2秒 | sysfs中心 |
| filesystem容量／service health | 10秒 | 重い走査を避ける |
| process上位一覧 | 5秒 | UI表示中のみ1秒可 |
| CLI診断 | 手動 | 通常周期では実行しない |
| DB aggregate | 60秒 | 1分平均／min／max／last |

ブラウザtabが非表示の場合、UIへの送信頻度だけを下げる。collector自体の安全監視周期は維持する。

## 8.5 バックプレッシャー

- subscriberごとに有界queueを持つ。
- live metricsは古い未送信snapshotを捨て、常に最新値を優先する。
- 履歴取得とlive streamを別APIにする。
- 低速クライアントのためにcollectorを停止しない。
- 1接続あたりの送信上限と最大購読数を設定する。

## 8.6 履歴

SSD書込みを抑えるため、生1秒データはメモリring bufferだけに保持する。

推奨保持:

```text
raw in-memory: 15分
10秒aggregate in-memory: 6時間
1分aggregate SQLite: 30日
1時間aggregate SQLite: 1年
```

SQLite:

- WAL
- busy timeout
- schema version
- batch transaction
- UTC timestamp
- retention cleanup
- 低頻度checkpoint
- 起動時に不完全bucketを安全に破棄または再計算

1分行には平均だけでなくmin、max、last、sample count、missing countを保存する。

## 8.7 API／WebSocket

```text
GET /api/control/v1/metrics/snapshot
GET /api/control/v1/metrics/capabilities
GET /api/control/v1/metrics/history?metric=...&from=...&to=...&resolution=...
GET /api/control/v1/processes
GET /api/control/v1/services/health
WS  /api/control/v1/metrics/stream
```

WebSocket初期メッセージ:

```json
{
  "type": "subscribe",
  "version": 1,
  "topics": ["system", "gpu", "stream"],
  "interval_ms": 1000
}
```

server response:

```json
{
  "type": "snapshot",
  "sequence": 1842,
  "sampled_at": "2026-07-23T12:00:00Z",
  "data": {}
}
```

sequence欠落時はクライアントが最新snapshotを再取得する。live metricsは全量snapshotを基本とし、差分形式は必要性を計測してから追加する。

## 8.8 警告

ルール:

```text
metric
operator
target
warning threshold
critical threshold
duration
clear hysteresis
cooldown
actions
```

例:

- GPU hotspot > thresholdが30秒継続
- GPU fan RPM = 0かつ温度上昇中
- VRAM > 95%が60秒継続
- filesystem free < 10%
- encoder FPS < requested FPSが10秒継続
- virtual sessionがPreparingのままtimeout

一時スパイクで通知しないため、継続時間、clear hysteresis、cooldownを必須とする。

初期action:

- Web UI banner
- audit／event log
- GPU thermal emergency policy
- 新規配信開始の抑止

Discord／Slack／Webhookは後続実装とし、URLとtokenはSecretStoreへ保存する。

## 8.9 モニターUI

Dashboard:

- CPU、RAM、GPU、VRAM、hotspot、GPU power、network、active streamの主要カード
- 直近60秒の軽量sparkline
- warning／critical banner
- sensor stale表示

System:

- CPU core別
- GPU別
- fan／temperature一覧
- disk／network
- service health
- process上位
- 1分／1時間履歴
- event correlation

GPUプロファイル変更、配信開始、Gamescope再起動、thermal eventをグラフ上のevent markerとして表示する。

## 8.10 性能目標

- collector平均CPU使用率: 1%未満を目標
- live metricsによる配信FPS低下: 測定誤差範囲
- sidecar RSS: 100 MiB未満を目標
- 1クライアントへのsnapshot生成: 20 ms以内
- 1分aggregate DB transaction: 100 ms以内
- sidecar再起動後5秒以内にlive monitoring復帰
- sensor failureでcollector全体が停止しない

## 9. Webターミナル／SSH

## 9.1 機能の区分

SteamShineでは以下を別機能として扱う。

1. Local Web Terminal
   - SteamOS上の現在ユーザーのshellをブラウザで操作
   - tmuxで永続化
2. Saved SSH Connection
   - guacdのSSH protocolを使って外部ホストへ接続
3. SSH CLI in Local Terminal
   - 通常の`ssh`コマンドをLocal Web Terminalから実行

Local Web Terminalをroot shellとして起動しない。root操作が必要な場合は通常の`sudo`認証を使う。

## 9.2 ターミナルセッションモデル

```cpp
struct terminal_session_t {
  std::string id;
  std::string owner_user_id;
  std::string tmux_name;
  std::string shell;
  std::string cwd;
  pid_t pane_pid;
  bool persistent;
  std::string state;
  std::chrono::system_clock::time_point created_at;
  std::chrono::system_clock::time_point activity_at;
  uint64_t latest_output_sequence;
};
```

セッションIDはランダム128bitをbase32またはhex表現する。IDをtmux targetへ渡す前に厳格な形式検証を行う。

## 9.3 tmux管理

- prefix: `steamshine-term-`
- 専用tmux socketを使用する。
- history limitは100,000行を上限とする。
- Web初期復元は最大10,000行かつ512 KiBの小さい方とする。
- tmux serverは`systemd-run --user --scope`でSteamShine service cgroup外へ起動する。
- SteamShine／sidecar再起動後もtmux sessionを再列挙して復元する。
- tmux未導入時のPTY fallbackは開発用とし、配布版では永続性がないことを明示する。
- session数、idle時間、最大継続時間を設定可能にする。

## 9.4 PTY接続

各ブラウザ接続ごとにPTYを開き、`tmux attach-session`を実行する。

- `TIOCSWINSZ`でresize
- nonblocking read／write
- UTF-8を破壊しないbinary WebSocket frame
- child exit監視
- attach重複ポリシー
- disconnect時はattach processだけ終了し、tmux sessionは維持

## 9.5 再接続プロトコル

ControlDeckの以下の挙動を維持する。

- client instance ID
- connection generation
- output sequence
- initial snapshot
- resume delta
- replay不能時のsnapshot reset
- bounded output journal
- input chunk ACK
- resize message

接続例:

```text
WS /api/control/v1/terminals/{id}/connect
  ?client_instance_id=...
  &connection_generation=3
  &attach_mode=resume
  &last_sequence=1842
```

server control message:

```json
{
  "type": "resume_ready",
  "from_sequence": 1842,
  "through_sequence": 1904,
  "connection_generation": 3
}
```

outputはcontrol JSONの直後にbinary frameを送る。

## 9.6 長文入力

モバイルの長文pasteで末尾欠落や二重入力を起こさないことを必須とする。

- 1 chunk最大16 KiB
- paste ID
- input sequence
- chunk index
- byte length
- SHA-256 optional verification
- server ACK後に次chunkを送信
- 再送時は同一sequenceを冪等処理
- 最大paste容量とcancel
- stale connection generationを拒否
- IME composition中の未確定文字を送信しない

通常キー入力とpaste batchを同時処理しない。

## 9.7 API

```text
GET    /api/control/v1/terminals
POST   /api/control/v1/terminals
GET    /api/control/v1/terminals/{id}
DELETE /api/control/v1/terminals/{id}
POST   /api/control/v1/terminals/{id}/rename
WS     /api/control/v1/terminals/{id}/connect
```

作成request:

```json
{
  "cwd": "/home/deck",
  "shell": "/bin/bash",
  "name": "maintenance"
}
```

shellは管理者設定のallowlistから選ぶ。`command`自由入力で任意コマンドを自動起動する機能は初期UIへ出さない。

## 9.8 セキュリティ

- `terminal.enabled=false`を既定とする。
- `terminal.use`権限を要求する。
- 原則として所有者だけが接続、終了できる。
- Administratorだけが他ユーザーのsessionを終了できる。
- WebSocket認証、Origin、ticket、接続rate limitを必須とする。
- PTY processへブラウザ由来の環境変数を渡さない。
- `PATH`、`HOME`、`SHELL`、localeはserver側で生成する。
- shellをSteamShine serviceと同一ユーザーで実行する。
- `sudo`のNOPASSWDをWeb terminal全体へ与えない。
- GPU操作はterminalのsudoではなく専用helper APIを使う。
- session outputをserver logへ複製しない。
- session内容を監査ログへ保存しない。

## 9.9 ターミナルUI

- xterm.js相当を遅延ロード
- 全画面表示
- session切替
- reconnect状態
- recent history復元中のhidden render
- mobile補助キー: Esc、Tab、Ctrl、Alt、矢印、Enter、Ctrl+C、Ctrl+D
- copy／paste sheet
- paste進捗とcancel
- software keyboard開閉
- iOS safe area／visual viewport対応
- 320px幅対応
- 右端scrollback navigationはIMEを開かない

## 10. リモートデスクトップ

## 10.1 構成

```text
Browser / PWA
  └─ guacamole-common-js
       └─ authenticated WSS
            └─ SteamShine reverse proxy
                 └─ steamshine-control
                      └─ loopback TCP 127.0.0.1:4822
                           └─ guacd
                                ├─ RDP
                                ├─ VNC
                                └─ SSH
```

`guacd`はloopbackだけで待受し、LANやTailscaleへ直接公開しない。

## 10.2 接続データモデル

```cpp
struct remote_connection_t {
  std::string id;
  std::string owner_user_id;
  std::string name;
  enum class protocol_t { rdp, vnc, ssh } protocol;
  std::string host;
  uint16_t port;
  std::string username;
  std::string encrypted_secret_blob;
  nlohmann::json non_secret_params;
  bool is_self;
  bool enabled;
  uint64_t revision;
};
```

secret:

- password
- SSH private key
- private key passphrase
- optional domain credential

API responseへsecret本文を返さず、`has_password`、`has_private_key`だけを返す。

## 10.3 SecretStore

- 32byte master keyを初回起動時に生成
- `~/.config/steamshine/control-secrets.key`
- mode `0600`
- AES-256-GCMまたは既存依存で同等のAEAD
- record ID、protocol、schema versionをAADへ含める
- recordごとにrandom nonce
- key rotation時はtransaction内で再暗号化
- backupへkeyを含める場合は明示的に暗号化export
- decryptはguacd接続直前だけ
- plaintextをDB、API、audit、exceptionへ残さない

## 10.4 接続先制限

リモート接続登録はSSRF／内部サービス到達機能になり得るため制限する。

既定許可:

- loopback
- RFC1918 private network
- Tailscale CGNAT range
- 管理者allowlist CIDR

既定拒否:

- cloud metadata addresses
- link-local
- multicast
- broadcast
- unspecified address
- public Internet address
- Unix socket風文字列

DNS名は接続前に解決し、全解決結果を検査して許可IPへpinする。検証後に別IPへ再解決させるDNS rebindingを防ぐ。

public hostを許可する場合は、Administratorが接続先単位で明示的に例外登録する。

## 10.5 guacdプロトコル

ControlDeckの以下を維持する。

1. `select`
2. `args`
3. `size`
4. `audio`
5. `video`
6. `image`
7. `connect`
8. 以後双方向pipe

Guacamole instructionは以下の形式である。

```text
LENGTH.VALUE,LENGTH.VALUE;
```

TCP read境界とinstruction境界は一致しない。guacamole-common-jsへ不完全な命令を送ると初期描画が失われ得るため、incremental UTF-8 decoderとinstruction splitterで完全な命令だけをWebSocket messageへ載せる。

制限:

- 単一instruction最大8 MiB
- parser buffer最大16 MiB
- handshake timeout 15秒
- idle timeout設定
- WebSocket frame rate limit
- 1ユーザーあたり同時接続上限
- 不正length、負数、overflow、終端欠落を即時拒否

## 10.6 RDP既定値

xrdp向け既定:

```text
security=any
ignore-cert=true
resize-method=display-update
disable-bitmap-caching=true
disable-offscreen-caching=true
disable-glyph-caching=true
```

Windowsへ接続する場合はNLAを選択可能にする。UIで`any`、`nla`、`tls`、`rdp`を選べるようにする。

## 10.7 SteamOS自身への管理デスクトップ

「このSteamOS PC」を最上段の固定接続として扱う。

用途:

- SteamOS Desktop Modeの保守
- ファイル操作
- ブラウザ設定
- SteamShineの復旧

ゲーム配信とは分離し、以下のどちらかを実機評価する。

1. xrdp + 独立Xorg session
2. xrdp + Xvnc/headless session

要件:

- 物理Gaming Modeの画面を乗っ取らない。
- headlessで接続できる。
- GameStreamのGamescope sessionとは別sessionとする。
- Exclusive Streaming Mode中でも管理デスクトップへ接続可能にする。
- GPUを使う場合もゲームencoderとの競合を監視する。
- xrdp停止とguacd停止を区別して診断する。

RDPはゲームプレイ用に最適化せず、管理画面に「ゲームはMoonlight／Artemisを使用」と明記する。

## 10.8 SSH接続

- protocol `ssh`
- passwordまたはprivate key
- host key policyを設定可能
- 初回host key fingerprintを表示し、管理者承認後にpin
- fingerprint変更時は自動接続せず警告
- terminal type、font、color scheme、keepaliveを設定可能
- agent forwarding、port forwarding、X11 forwardingは初期実装で無効
- private keyはSecretStoreへ暗号化保存

## 10.9 モバイル操作

ControlDeckの操作体系を維持する。

- 1本指移動: 相対カーソル
- 1本指tap: 左クリック
- 長押し後移動: drag
- 2本指tap: 右クリック
- 2本指上下: scroll
- 3本指tap: software keyboard
- touch端末は高解像度接続し縮小表示
- display scaleを考慮して座標変換
- orientation／resize時に`sendSize`

## 10.10 クリップボード

- Remote → client: Guacamole clipboard stream受信
- Client → remote: clipboard stream送信後にCtrl+V
- text/plainのみを初期対応
- 最大1 MiB
- binary clipboardを拒否
- クリップボード本文をログ、DB、auditへ保存しない
- browser permission拒否をUIへ表示
- copy待機にtimeoutを設定

## 10.11 API

```text
GET    /api/control/v1/remote/status
GET    /api/control/v1/remote/connections
POST   /api/control/v1/remote/connections
GET    /api/control/v1/remote/connections/{id}
PATCH  /api/control/v1/remote/connections/{id}
DELETE /api/control/v1/remote/connections/{id}
POST   /api/control/v1/remote/connections/{id}/test
POST   /api/control/v1/remote/connections/{id}/ticket
WS     /api/control/v1/remote/connections/{id}/tunnel
```

`test`はTCP到達性、DNS／CIDR policy、guacd capabilityだけを確認し、保存済みcredentialでログインしない。

## 10.12 リモートUI

一覧:

- 「このSteamOS PC」を最上段固定
- RDP／VNC／SSH badge
- host／port／username
- health
- last connected
- 接続ボタン
- 編集／削除は権限に応じて表示

viewer:

- 全画面
- 接続状態
- reconnect
- copy／paste
- Ctrl+Alt+Del（RDPのみ）
- software keyboard
- disconnect
- touch gesture help
- network quality表示

## 11. 管理API共通仕様

### 11.1 エラー形式

```json
{
  "error": {
    "code": "gpu_capability_changed",
    "message": "GPU能力が変更されたため再読み込みしてください",
    "request_id": "uuid",
    "retryable": true,
    "details": {}
  }
}
```

Secret、command output、内部file path、helper stderr全文を返さない。

### 11.2 制限

- JSON body size
- WebSocket handshake rate
- per-user connection count
- terminal session count
- remote concurrent tunnel count
- history query time range
- metrics topic count
- GPU apply rate

429には`Retry-After`を付ける。

### 11.3 ヘルス

```text
GET /api/control/v1/health
```

返却項目:

- control sidecar
- internal authentication
- metrics collector
- history DB
- hw helper
- guacd
- xrdp／VNC backend
- tmux
- GPU provider
- stale session count

## 12. 永続データ

推奨DB:

```text
~/.local/state/steamshine/control.db
```

主要table:

```text
schema_meta
gpu_profiles
gpu_profile_bindings
gpu_apply_events
remote_connections
alert_rules
alert_events
metric_minute
metric_hour
audit_events
```

Terminal session本体はtmuxをsource of truthとし、DBには表示名、owner、policyだけを保存する。起動時にtmux一覧と照合する。

## 13. 設定ファイル

```text
~/.config/steamshine/control.toml
```

例:

```toml
schema_version = 1

[control]
enabled = true
socket = "%t/steamshine/control.sock"

[monitoring]
enabled = true
live_interval_ms = 1000
sensor_interval_ms = 2000
history_enabled = true
minute_retention_days = 30
hour_retention_days = 365

[gpu_control]
enabled = false
helper_socket = "/run/steamshine/hw-helper.sock"
apply_on_stream_start = false
restore_on_stream_end = true
restore_on_shutdown = true

[terminal]
enabled = false
max_sessions_per_user = 4
history_lines = 100000
replay_lines = 10000
replay_bytes = 524288
idle_timeout_minutes = 0

[remote_desktop]
enabled = false
guacd_host = "127.0.0.1"
guacd_port = 4822
max_connections_per_user = 2
allow_external_hosts = false
allowed_cidrs = ["127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10"]
```

不正値は安全な既定へ黙って置換せず、起動診断で明示する。旧schemaはmigration後にbackupを残す。

## 14. systemd構成

### 14.1 user services

```text
steamshine.service
steamshine-control.service
guacd.service または system service参照
tmux scopes
```

`steamshine-control.service`:

- SteamShineと同一通常ユーザー
- Restart=on-failure
- 独立CPU／memory quotaを設定可能
- main daemonのPartOfにしない
- control停止でstream daemonを停止しない
- private runtime directory

### 14.2 root helper

```text
steamshine-hw-helper.socket
steamshine-hw-helper.service
```

socket activationにより常駐時間を短くする。ただしmanual fan curveをhelper watchdogで管理する場合は常駐serviceとする。

## 15. SteamOSインストール

既存の`install-steamos.sh`へ以下を追加する。

- CPU／GPU／hwmon診断
- control sidecar配置
- user service配置
- tmux有無
- guacd有無
- xrdp／VNC backend有無
- helper capability診断
- rootfs／read-only状態検出
- SecretStore key生成
- DB migration
- Tailscale／LAN bind診断
- 既存設定backup

root変更が必要な機能は明示的なsubcommandへ分ける。

```bash
./scripts/install-steamos.sh core
./scripts/install-steamos.sh gpu-control
./scripts/install-steamos.sh terminal
./scripts/install-steamos.sh remote-desktop
```

`core`だけではroot helper、guacd、xrdpを導入しない。

SteamOS更新後に必要なsystem fileが消えた場合、診断画面から再適用コマンドを表示する。rootfs変更を前提にせず、利用可能ならsystemd-sysext等の更新耐性が高い方式を別途評価する。

## 16. 障害処理

## 16.1 control sidecar停止

- game stream継続
- 管理UIへdegraded表示
- metrics／terminal／remoteだけ利用不能
- main daemonが指数backoffでsidecar再接続
- active tmux sessionは維持
- active guacd tunnelは切断され得るがstreamへ影響させない

## 16.2 helper停止

- GPU read-only monitoring継続
- 新規設定変更を503で拒否
- 現在のdriver設定を勝手に推測しない
- manual fan利用中ならwatchdogがautoへ復元

## 16.3 sensor異常

- 該当項目をstale／unavailable
- 他項目を継続
- custom fan制御解除
- thermal safetyに必要な温度が失われた場合、危険profileの新規適用を拒否

## 16.4 guacd停止

- Remote一覧／設定は利用可能
- 新規tunnelを503
- healthに復旧コマンド
- Local Web Terminalは継続

## 16.5 history DB破損

- live metrics継続
- DBをread-only隔離
- integrity check結果をUIへ表示
- backupからrestoreまたは新規DB生成
- 配信を停止しない

## 16.6 suspend／resume

suspend前:

- GPU manual fan解除
- DB flush
- remote tunnelへ終了通知
- terminalはtmuxのまま保持

resume後:

- device／BDF／hwmon再検出
- GPU capability revision更新
- collectorのcounter baseline再初期化
- stale network／disk差分を破棄
- policyを再評価してから再適用

## 17. ロールバック

- `control.enabled=false`で管理sidecarを完全無効化
- `gpu_control.enabled=false`でhelper呼出しを停止
- `terminal.enabled=false`でAPI／メニューを停止して既存tmuxは明示的に残すか終了を選択
- `remote_desktop.enabled=false`でguacd tunnelを停止
- uninstall時にGPUをdriver default／fan autoへ戻す
- DBとSecret keyをbackup
- upstream Sunshine互換の既存Web UIと配信機能を維持

緊急復旧:

```bash
steamshine-recover --restore-gpu-defaults --fan-auto
steamshine-recover --stop-control-plane
steamshine-recover --kill-remote-tunnels
steamshine-recover --list-terminal-sessions
```

## 18. 推奨ソース構成

```text
src/
  control/
    control_client.cpp
    control_client.h
    internal_auth.cpp
    internal_auth.h

  platform/linux/hardware/
    gpu_inventory.cpp
    gpu_inventory.h
    amd_gpu_capabilities.cpp
    amd_gpu_capabilities.h
    amd_gpu_state.cpp
    amd_gpu_state.h
    gpu_profile.cpp
    gpu_profile.h

control/
  main.cpp
  api/
    router.cpp
    errors.cpp
  monitoring/
    metrics_collector.cpp
    metrics_store.cpp
    linux_system_metrics.cpp
    amd_gpu_metrics.cpp
    stream_metrics_bridge.cpp
  gpu/
    gpu_policy_controller.cpp
    gpu_profile_store.cpp
    thermal_guard.cpp
  terminal/
    terminal_manager.cpp
    tmux_manager.cpp
    pty_connection.cpp
    terminal_journal.cpp
    terminal_ws.cpp
  remote/
    remote_connection_store.cpp
    secret_store.cpp
    destination_policy.cpp
    guacd_client.cpp
    guacamole_parser.cpp
    remote_ws.cpp
  persistence/
    sqlite_store.cpp
    migrations/
  security/
    internal_envelope.cpp
    rate_limiter.cpp
  audit/
    audit_store.cpp

helper/
  steamshine_hw_helper.cpp
  gpu_sysfs_writer.cpp
  helper_protocol.cpp

src_assets/common/assets/web/
  control/
    pages/SystemMonitor.vue
    pages/GpuControl.vue
    pages/Terminal.vue
    pages/RemoteDesktop.vue
    components/metrics/
    components/gpu/
    components/terminal/
    components/remote/

scripts/
  install-steamos.sh
  uninstall-steamos.sh
  diagnose-control-plane.sh
  recover-steamos-session.sh
  recover-hardware-defaults.sh

docs/
  STEAMOS_VIRTUAL_SESSION_PLAN.md
  STEAMOS_CONTROL_PLANE_DESIGN.md
  STEAMOS_CONTROL_SECURITY.md
  STEAMOS_CONTROL_TEST_PLAN.md
```

既存Sunshineのソース構造に合わせて配置名は実装時に調整する。

## 19. ControlDeck移植マップ

| ControlDeck | SteamShine | 移植内容 |
|---|---|---|
| `backend/app/models_mgmt/amd_gpu.py` | `platform/linux/hardware/*`, `control/gpu/*` | BDF選択、power／clock検証、read-back、profile |
| `/usr/local/libexec/control-deck-hw-helper`相当 | `steamshine-hw-helper` | 固定operation、最小権限、sudo非依存socket |
| `backend/app/monitoring/gpu.py` | `control/monitoring/amd_gpu_metrics.*` | sysfs fast path、amd-smi／rocm-smi fallback |
| `backend/app/monitoring/collector.py` | `control/monitoring/metrics_collector.*` | periodic sample、bounded subscribers、aggregate |
| `backend/app/terminals/manager.py` | `control/terminal/tmux_manager.*` | tmux永続session、PTY、bounded replay |
| `backend/app/terminals/router.py` | `control/terminal/terminal_ws.*` | auth済みWS、resume、sequence、input ACK |
| `backend/app/terminals/stream.py` | `control/terminal/terminal_journal.*` | bounded journal、subscriber管理 |
| `backend/app/remote_desktop/guacd.py` | `control/remote/guacd_client.*`, `guacamole_parser.*` | handshake、命令境界、TCP↔WS pipe |
| `backend/app/remote_desktop/service.py` | `remote_connection_store.*`, `secret_store.*` | 暗号化secret、RDP既定値、health |
| `backend/app/remote_desktop/router.py` | `control/remote/remote_ws.*` | CRUD、RBAC、ticket、activity |
| React Terminal UI | Vue Terminal UI | xterm、mobile key、resume UX |
| React Remote UI | Vue Remote UI | Guacamole viewer、touch、clipboard |

移植時に改善する点:

- 単一最大VRAM GPU選択から、全GPU＋BDF指定へ拡張
- CLI常時収集を廃止しsysfs fast pathを徹底
- sudo fallbackではなくUnix socket helperを標準化
- remote接続先CIDR policyとDNS rebinding対策を追加
- stream daemonからPTY／guacd／SQLiteを分離
- GPU applyをread-back付きtransactionへ変更
- fan watchdogとthermal guardを追加

## 20. 実装フェーズ

既存の仮想セッションPhase 0〜6と並行する管理系workstreamとして、`C0`〜`C7`を使用する。

## C0: 境界・セキュリティ基盤

作業:

- `steamshine-control` skeleton
- Unix socket
- internal auth envelope
- feature flag
- RBAC permission
- structured error
- audit event
- sidecar crash isolation test

完了条件:

- sidecarをkillしてもactive streamが継続
- 未認証browserからcontrol APIへ到達不能
- sidecar socketへ別UIDから接続不能

## C1: Read-only resource monitor

作業:

- CPU／memory／disk／network
- AMD GPU inventory／sysfs metrics
- metrics WS
- dashboard／system UI
- sensor N/A handling

完了条件:

- 24時間収集でcollector停止なし
- GPU／CPU温度、fan、utilizationの取得可否を個別表示
- 監視ON/OFFで配信性能差が測定誤差範囲

## C2: History／health／alerts基盤

作業:

- in-memory ring
- SQLite minute／hour aggregate
- service health
- warning duration／hysteresis／cooldown
- event markers

完了条件:

- 再起動後に履歴継続
- DB unavailableでもlive metrics継続
- 短いspikeでalertを発報しない

## C3: AMD GPU profile read-only／preview

作業:

- capability detection
- BDF mapping
- state read-back
- profile schema
- preview／validation
- UI差分表示

完了条件:

- 書込みなしで全対応範囲を表示
- GPU／driver変更でcapability revision更新
- 不正level／範囲外Wを拒否

## C4: AMD GPU privileged apply

作業:

- root helper
- socket activation
- transaction／rollback
- per-stream binding
- thermal guard
- fan auto復元

完了条件:

- 100回のapply／restoreでdriver defaultへ復元可能
- helperへ任意command／pathを注入不能
- apply失敗時にlast-known-goodへrollback
- helper crash時にmanual fanをautoへ戻す

## C5: Local Web Terminal

作業:

- tmux manager
- PTY
- WS sequence／resume
- chunk ACK paste
- Vue/xterm UI
- mobile key／IME

完了条件:

- browser／sidecar再起動後にsession復元
- 100回再接続で重複出力／入力なし
- 長文pasteで末尾欠落なし
- 他ユーザーsessionへ接続不能

## C6: RDP／VNC／SSH

作業:

- guacd client
- parser／splitter
- SecretStore
- destination policy
- CRUD／tunnel
- Remote Viewer
- touch／clipboard

完了条件:

- RDP、VNC、SSHを各30分操作
- TCP chunk分割で初期全画面描画を失わない
- password／private keyがAPI／logへ漏れない
- public／metadata addressを既定拒否

## C7: SteamOS self desktop／packaging

作業:

- xrdpまたはVNCのheadless session
- health／recovery hint
- installer subcommands
- SteamOS update後診断
- backup／restore
- documentation

完了条件:

- 物理モニターなしで管理デスクトップ接続
- Gaming Modeと管理sessionを分離
- update後に単一コマンドで再適用可能
- uninstallでGPU、fan、serviceを安全に復元

## 21. PR分割案

1. control sidecar skeletonとinternal socket
2. RBAC、audit、feature flag
3. Linux system metrics read-only
4. AMD GPU multi-device metrics
5. metrics WebSocketとUI
6. history DBとalerts
7. AMD capability／profile preview
8. root helperとGPU apply
9. thermal guard／stream profile binding
10. tmux manager／PTY
11. terminal WS resume protocol
12. Terminal Vue UI／mobile UX
13. SecretStore／remote connection CRUD
14. guacd parser／tunnel
15. Remote Viewer／touch／clipboard
16. SteamOS self desktop installer
17. endurance／security testsと文書

GPU制御、terminal、remoteを1つの巨大PRで実装しない。

## 22. テスト計画

## 22.1 単体テスト

AMD GPU:

- sysfs parser
- BDF resolution
- power min／max
- DPM level parser
- profile normalization
- capability revision
- operation allowlist
- rollback plan
- fan curve interpolation／hysteresis
- thermal state transition

Monitoring:

- counter wrap／reset
- network／disk delta
- missing sensor
- stale value
- aggregate min／max／avg／last
- retention
- subscriber backpressure

Terminal:

- session ID validation
- tmux target escaping
- replay truncation
- journal sequence
- resume delta
- reconnect generation
- duplicate input ACK
- resize bounds
- UTF-8 split

Remote:

- Guacamole encoder／parser
- instruction split across arbitrary chunks
- oversized instruction
- handshake timeout
- SecretStore AEAD
- CIDR policy
- DNS rebinding
- RDP params
- clipboard bounds

## 22.2 統合テスト

- fake sysfs tree
- fake hw helper
- helper timeout／partial apply／rollback
- fake amd-smi JSON
- fake tmux／PTY
- sidecar restart with live tmux
- fake guacd
- fragmented TCP input
- DB locked／corrupt／read-only
- main daemon ↔ sidecar ticket
- browser disconnect during GPU apply
- suspend／resume simulation

## 22.3 セキュリティテスト

- unauthenticated REST／WS
- Origin mismatch
- CSRF
- stale／replayed internal envelope
- sidecar socket別UID
- shell metacharacters in BDF／profile／host
- path traversal
- SSRF metadata address
- DNS rebinding
- secret in API／audit／exception
- terminal cross-user access
- WebSocket flood
- large paste／clipboard／Guacamole instruction

## 22.4 実機テスト

最低構成:

- SteamOS
- AMD GPU 1枚
- AMD GPU 2枚構成
- Moonlight PC
- Artemis Android
- iPhone Safari
- 有線LAN
- Tailscale経由

ケース:

- idle／game load／encode load
- physical monitor ON／OFF
- GPU profile apply中のstream start
- stream中のthermal event
- helper kill
- sidecar kill
- SteamShine restart
- suspend／resume
- driver reload後
- tmux 100 reconnect
- RDP／VNC／SSH
- iPhone keyboard open／close
- network interruption
- SteamOS update後診断

## 22.5 長時間試験

- monitoring 72時間
- game stream + monitoring 8時間
- GPU profile切替500回の自動試験
- terminal output 100,000行
- remote desktop 4時間
- 50回suspend／resume
- 100回sidecar restart

## 23. 受入条件

### AMD GPU

- 全AMD GPUをBDF単位で識別できる。
- 電力、SCLK、MCLKの対応範囲を実機から取得する。
- 対応しない項目を安全に無効化する。
- 設定適用後にread-back検証する。
- 失敗時にrollbackする。
- fan manual使用後に必ずautoへ戻せる。
- root helperが任意commandを実行できない。

### Resource Monitor

- CPU、memory、GPU、VRAM、温度、fan、power、disk、networkを可能な範囲で表示する。
- 取得不能項目だけをN/Aにする。
- 監視障害で配信を停止しない。
- live、1分、1時間の履歴を表示する。
- low-speed clientでcollectorを詰まらせない。
- 監視によるframe pacing悪化がない。

### Terminal／SSH

- tmux sessionがbrowser／sidecar再起動後も残る。
- resumeで重複と欠落を起こさない。
- 長文pasteをACK付きで最後まで送る。
- mobile keyboard／IMEで二重入力を起こさない。
- root自動ログインを提供しない。
- cross-user session accessを拒否する。

### Remote Desktop

- RDP、VNC、SSHをブラウザから操作できる。
- SteamOS自身へヘッドレス管理接続できる。
- Guacamole命令境界を維持する。
- touch、scroll、drag、right click、keyboard、clipboardが動作する。
- credentialを暗号化保存しAPIへ返さない。
- public／metadata addressを既定拒否する。
- Remote障害がGameStreamへ影響しない。

## 24. 完成定義

以下を満たした時点で管理・遠隔操作・監視機能の初期完成とする。

- SteamShineの配信機能と管理sidecarがプロセス分離されている。
- AMD GPU設定を能力検出、preview、適用、read-back、rollbackの順で安全に変更できる。
- 温度／fan／電力を含む監視が常時動作し、欠測へ耐える。
- ブラウザを閉じても継続するtmuxターミナルを利用できる。
- RDP／VNC／SSH接続を暗号化保存しブラウザから操作できる。
- 物理モニターなしでSteamOSの管理デスクトップへ到達できる。
- SteamShine、control sidecar、helper、guacdの障害が適切に分離される。
- すべての危険操作にRBAC、監査、rate limit、入力検証がある。
- 機能を無効化すれば既存Sunshine互換の配信だけで動作する。
- SteamOS更新後に診断と再適用が可能である。

---

本設計は、ControlDeckの実績あるGPU制御、sysfs優先監視、tmux永続ターミナル、guacdリモート操作を、SteamShineの低遅延配信を妨げない独立コントロールプレーンへ再構成する。最初はread-only監視から導入し、能力検出、GPU書込み、ターミナル、リモートデスクトップの順に段階実装する。