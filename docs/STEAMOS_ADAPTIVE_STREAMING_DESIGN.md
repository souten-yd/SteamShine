# SteamShine: クライアント追従表示・自動bitrate・低負荷設計

- 状態: 実装前詳細設計
- 対象: SteamOS / Arch系Linux、AMD GPU、Moonlight / Artemis
- 作成日: 2026-07-23
- 関連文書:
  - [`STEAMOS_VIRTUAL_SESSION_PLAN.md`](./STEAMOS_VIRTUAL_SESSION_PLAN.md)
  - [`STEAMOS_CONTROL_PLANE_DESIGN.md`](./STEAMOS_CONTROL_PLANE_DESIGN.md)

## 1. この文書で固定する優先順位

SteamShineの実装判断は、以下の順序を必ず守る。

| 優先度 | 対象 | 方針 |
|---:|---|---|
| P0 | Moonlight / Artemisへのゲーム配信 | 最優先。仮想Gamescopeデスクトップ、低遅延、高画質、安定性を守る |
| P0 | キャプチャ、AMDエンコード、入力、音声 | 管理機能より常に高いCPU・I/O・スレッド優先度を持つ |
| P1 | クライアント画面への自動追従 | ホストの物理モニターサイズに依存せず、クライアントの表示領域を基準にする |
| P1 | 回線品質への自動bitrate追従 | 画質を可能な限り高く保ちつつ、輻輳・損失・遅延増加を避ける |
| P2 | Web管理画面からの完全な観測・設定 | 配信状態、選択理由、GPU、温度、回線、履歴、障害をWebから確認可能にする |
| P3 | SSH、Webターミナル、リモートデスクトップ | ゲーム配信を妨げない範囲で提供する |
| P4 | Webブラウザへのゲーム映像配信 | 実現可能性を評価する任意機能。初期実装・完成条件には含めない |

Webブラウザゲーム配信を理由に、Moonlight / Artemis互換性、仮想デスクトップ、配信遅延、画質、AMDエンコード性能を悪化させてはならない。

## 2. 必須要件

### 2.1 表示

1. Virtual Session Modeでは、ホストの物理ディスプレイ解像度、接続状態、電源状態を表示サイズ決定に使用しない。
2. クライアントが要求した解像度、FPS、向き、アスペクト比を第一の入力とする。
3. Artemis拡張が利用可能な場合は、実画面サイズ、表示可能領域、ノッチ・角丸・システムバーを除いたsafe areaも使用する。
4. ゲームがクライアント固有アスペクト比へ対応しない場合も、表示欠落や意図しない引き伸ばしを避ける。
5. 既定は全内容を表示する`fit`とし、切り取りを伴う`fill`は明示設定時だけ使用する。
6. 物理画面と同じ映像を配信するExisting Gaming Session Modeは互換モードとして残すが、自動選択の第一候補にしない。

### 2.2 bitrate

1. クライアントが指定したbitrateは上限として尊重する。
2. 解像度、FPS、codec、HDR、色形式から高品質な開始bitrateを算出する。
3. 回線の損失、RTT、jitter、送信queue、フレーム到着状況に応じて、配信中もtarget bitrateを自動調整する。
4. 輻輳時は速やかに下げ、回復時は慎重に上げる。
5. 短い無線packet lossだけで過剰に画質を落とさず、RTT増加や送信queue増加を併用して輻輳を判定する。
6. bitrate変更は解像度変更より先に行う。配信中の解像度変更は画面再構成を伴うため、既定では行わない。
7. 自動調整が利用できないencoder backendでは、配信を再起動してまでbitrateを頻繁に変更しない。

### 2.3 性能・SSD

1. ゲーム配信中に、監視・Web UI・履歴保存・SSH・RDPがcapture / encode / inputをblockしない。
2. packet単位、frame単位、1秒単位の値をSSDへ直接記録しない。
3. 収集のfast pathはsysfs、hwmon、`/proc`、既存プロセス内counterとする。
4. `amd-smi`、`rocm-smi`等の外部CLIを毎秒起動しない。
5. runtime state、ring buffer、短期履歴は`/run/user/$UID/steamshine`またはメモリ上に置く。
6. 永続履歴は集約・batch化し、ゲーム配信中はcheckpoint、圧縮、vacuum等を行わない。
7. Webクライアントが遅い場合は古いmonitor snapshotを破棄し、最新値を優先する。

## 3. 現行プロトコルを利用する方針

現行Moonlight系クライアントは接続開始時に以下を指定する。

```text
width
height
fps
bitrate
client refresh rate
supported codecs
HDR / color capabilities
```

Moonlightのbitrate値は、映像payloadだけではなくFECを含むstream bitrateとして扱われる。標準的な20% FECでは、encoder本体のtargetは指定値より低くなるため、SteamShine内部では次を分離して扱う。

```text
client_stream_ceiling_bps
wire_budget_bps
video_encoder_target_bps
fec_overhead_bps
audio_overhead_bps
protocol_overhead_bps
```

Sunshineには既にクライアントからのloss reportが届く。

```text
loss count since last report
milliseconds since last report
last good frame
```

現状のupstream Sunshineはこれを主にログ出力へ使用している。SteamShineでは、この既存feedbackをstock Moonlight互換の自動bitrate制御へ利用する。

基本自動制御はstock Moonlightを変更せず成立させる。Artemis拡張は、safe area、decoder queue、jitter、表示遅延、network type等の追加情報を与える高度化として扱う。

## 4. 全体アーキテクチャ

```text
Moonlight / Artemis
  ├─ requested width / height / fps / bitrate
  ├─ codec / HDR / decoder capabilities
  ├─ standard loss reports
  └─ optional Artemis telemetry extension
             │
             ▼
SteamShine Session Negotiator
  ├─ Client Capability Normalizer
  ├─ Display Policy Engine
  ├─ Network Bootstrap Estimator
  └─ Resource Priority Governor
             │
             ├───────────────┐
             ▼               ▼
Virtual Gamescope        Adaptive Rate Controller
  ├─ virtual canvas       ├─ loss / RTT / jitter
  ├─ content viewport     ├─ send queue / pacing
  ├─ safe area            ├─ client ceiling
  └─ UI scale             └─ historical good rate
             │               │
             ▼               ▼
PipeWire / DMA-BUF ──► AMD Encoder Rate Adapter
             │               │
             └──────► GameStream UDP + FEC
                              │
                              ▼
                     In-memory Telemetry Bus
                              │
                              ├─ Web UI / API
                              ├─ alerts
                              └─ batched optional history
```

## 5. コンポーネント

### 5.1 Client Capability Normalizer

stock MoonlightとArtemis拡張の入力差を吸収し、共通descriptorを生成する。

```cpp
struct client_display_descriptor_t {
  std::string client_id;

  // 標準Moonlight要求
  uint32_t requested_stream_width;
  uint32_t requested_stream_height;
  uint32_t requested_fps;
  uint32_t client_refresh_millihz;
  uint64_t requested_bitrate_bps;

  // Artemis拡張。未提供時は0またはnull
  std::optional<uint32_t> native_width;
  std::optional<uint32_t> native_height;
  std::optional<uint32_t> viewport_width;
  std::optional<uint32_t> viewport_height;
  std::optional<uint32_t> safe_inset_left;
  std::optional<uint32_t> safe_inset_top;
  std::optional<uint32_t> safe_inset_right;
  std::optional<uint32_t> safe_inset_bottom;
  std::optional<float> dpi;
  std::optional<float> device_scale_factor;
  std::optional<std::string> orientation;

  // decode / display capability
  uint32_t max_decode_width;
  uint32_t max_decode_height;
  uint64_t max_decode_pixels_per_second;
  std::vector<codec_profile_t> codecs;
  bool hdr_supported;
  bool yuv444_supported;
  bool dynamic_rate_feedback_supported;
  bool dynamic_geometry_supported;
};
```

stock Moonlightでは、`requested_stream_width`と`requested_stream_height`をクライアントが選んだ有効表示領域とみなす。

Artemis拡張では以下の優先順位でviewportを決める。

```text
explicit viewport
  > native resolution - safe insets
  > requested stream resolution
  > client profile default
```

### 5.2 Display Policy Engine

クライアントdescriptorとhost capabilityから、Gamescope仮想canvasと映像内content rectangleを決定する。

```cpp
struct selected_stream_geometry_t {
  uint32_t canvas_width;
  uint32_t canvas_height;
  uint32_t stream_width;
  uint32_t stream_height;
  uint32_t refresh_millihz;

  uint32_t content_x;
  uint32_t content_y;
  uint32_t content_width;
  uint32_t content_height;

  uint32_t safe_x;
  uint32_t safe_y;
  uint32_t safe_width;
  uint32_t safe_height;

  float ui_scale;
  enum class fit_mode_t { exact, fit, fill_safe, integer, custom } fit_mode;
  bool letterbox;
  bool pillarbox;
  bool cropped;
  std::string selection_reason;
};
```

### 5.3 Network Bootstrap Estimator

接続前または開始直後のtarget bitrateを算出する。

入力:

- クライアントbitrate上限
- 管理者上限
- 解像度・FPS・codec
- HDR / 4:4:4
- LAN / Tailscale / WAN分類
- 同一client・同一network fingerprintの直近成功bitrate
- 過去のloss、RTT、切断理由
- 任意の短時間preflight probe

preflight probeは既定で回線を飽和させない。大容量speed testを接続前に毎回実行しない。開始後のpassive rampを主方式とする。

### 5.4 Adaptive Rate Controller

配信中にnetwork feedbackを集約し、encoder targetを更新する。

```cpp
struct adaptive_rate_sample_t {
  std::chrono::steady_clock::time_point at;
  uint64_t packets_sent;
  uint64_t bytes_sent;
  uint64_t packets_lost_reported;
  uint64_t report_interval_ms;
  uint32_t last_good_frame;
  std::optional<double> rtt_ms;
  std::optional<double> rtt_variance_ms;
  std::optional<double> client_jitter_ms;
  std::optional<double> client_decode_queue_ms;
  std::optional<double> client_render_queue_ms;
  uint64_t socket_queue_bytes;
  uint64_t pacing_late_packets;
  uint64_t encoder_target_bps;
  uint64_t measured_wire_bps;
};
```

### 5.5 Encoder Rate Adapter

encoder backendごとの差を隠す。

```cpp
class encoder_rate_adapter_t {
 public:
  virtual bool supports_runtime_bitrate_change() const = 0;
  virtual bool supports_runtime_vbv_change() const = 0;
  virtual rate_apply_result_t set_rate(
      uint64_t target_bps,
      uint64_t peak_bps,
      uint32_t vbv_ms) = 0;
  virtual encoder_rate_state_t read_back() const = 0;
  virtual ~encoder_rate_adapter_t() = default;
};
```

優先実装:

1. AMD VAAPI
2. AMD Vulkan Video
3. NVENC
4. Intel VAAPI / Quick Sync
5. software encoder

backendがruntime変更を安全に行えない場合は、開始時自動bitrateだけを使用する。画質改善のためにencoderを何度も作り直し、frame freezeや入力遅延を発生させない。

### 5.6 In-memory Telemetry Bus

packet / frame pathはatomic counterとlock-freeまたは短時間lockのsnapshot更新だけを行う。

Web JSON、SQLite、alert評価をstream thread内で実行しない。

## 6. クライアント表示範囲の決定

### 6.1 原則

Virtual Session Modeでは次の値を使用しない。

- 物理モニターの現在解像度
- 物理モニターのアスペクト比
- 物理モニターのprimary設定
- 物理モニターの電源状態
- HDMI / DisplayPort EDID

次を使用する。

- client requested stream geometry
- client native / viewport geometry
- safe area
- decoder limits
- encoder limits
- user policy
- current network capacity

### 6.2 検証

入力値は以下を検証する。

- 0、負数、overflowを拒否
- 管理者設定の最大width / height / FPSを超えない
- decoder最大pixel rateを超えない
- encoder capabilityを超えない
- 4:2:0では最低2pixel単位へ整列
- backendが4 / 8 / 16pixel alignmentを要求する場合は内側または外側へ安全に補正
- 補正量と理由をWeb UIへ表示

既定上限をコードへ固定せず、実機capabilityと安全な設定上限の小さい方を使う。

### 6.3 選択手順

```text
1. client viewportを決定
2. safe areaを決定
3. target aspect ratioを算出
4. decoder / encoder / policy上限を適用
5. network初期推定から許容pixel rateを確認
6. exact candidateを試す
7. game / Gamescopeが受理しない場合は同一aspectの下位candidateへ移る
8. content rectangleとbar / cropを決定
9. encoder alignmentを適用
10. selected reasonを記録
```

### 6.4 fit mode

#### exact

クライアントviewportと同じcanvasを作る。

```text
client 2560x1600
→ Gamescope 2560x1600
→ stream 2560x1600
→ content 2560x1600
```

ゲームが対応する場合の第一候補とする。

#### fit（既定）

ゲーム側のaspectを維持し、全内容を表示する。余白はletterbox / pillarboxとする。

```text
client 3440x1440
16:9-only game 2560x1440
→ content 2560x1440 centered
→ left/right bars 440px each
```

HUDや字幕を欠落させないため、既定ではcropしない。

#### fill_safe

safe areaを満たす範囲で拡大し、外側だけをcropする。ユーザーが明示的に選択した場合に限る。

#### integer

低解像度ゲーム、pixel art等で整数倍率を優先する。

#### stretch

意図しない縦横比変形を避けるため既定候補にしない。Advanced設定でのみ許可する。

### 6.5 safe area

Artemisがsafe insetを提供した場合、以下を分離する。

```text
full canvas
safe display rectangle
recommended game UI rectangle
```

ゲーム本体をfull canvasへ表示し、SteamShine overlay、通知、ソフトウェアキーボード、接続情報はsafe rectangle内へ配置する。

ゲームUI自体のsafe areaを外部から変更できない場合は、Steam overlayやSteam Big Pictureのscale調整へ使用する。

### 6.6 UI scale

UI scaleは単純な解像度だけではなく、DPI、device scale factor、短辺pixel数から決める。

```text
ui_scale = clamp(
  client_profile_override
    ?? dpi_based_scale
    ?? short_edge_based_scale,
  minimum,
  maximum)
```

stock MoonlightではDPIが不明なため、短辺と過去のclient profileから推定する。

### 6.7 orientation

- 接続開始時の向きをsession geometryへ反映する。
- stock Moonlightで配信中に端末回転した場合は、client側scaleで継続する。
- Artemis拡張でdynamic geometryを利用できる場合だけ、将来のhot reconfigure候補とする。
- 配信中の端末回転を理由に即座にGamescopeとencoderを再生成しない。

### 6.8 例

#### Steam Deck系 1280x800

```text
client aspect: 16:10
candidate: 1280x800@60/90
16:10対応ゲーム: exact
16:9固定ゲーム: 1280x720 centered + 40px top/bottom
```

#### 2560x1600 tablet

```text
client aspect: 16:10
network sufficient: 2560x1600
network insufficient: 1920x1200または1600x1000
aspectを維持し、物理host monitorは参照しない
```

#### 3440x1440 ultrawide

```text
ultrawide対応ゲーム: exact
非対応ゲーム: 2560x1440 fit
fill_safeはユーザー選択時だけ
```

#### mobile wide display

```text
Artemis viewport: 2400x1080
safe insets: left 80 / right 40
virtual canvas: 2400x1080
safe overlay rectangle: x=80, width=2280
```

## 7. 開始bitrateの決定

### 7.1 hard ceiling

```text
hard_ceiling = min(
  client_requested_bitrate,
  client_profile_max,
  administrator_max,
  encoder_backend_max,
  transport_policy_max)
```

stock Moonlightのbitrate指定を勝手に超えない。

### 7.2 quality bootstrap

開始値の算出には、1080p60 SDRを基準とした経験的modelを使用する。

```text
pixel_ratio = pixels / (1920 * 1080)
fps_ratio = fps / 60

bootstrap = reference_bitrate
          * pow(pixel_ratio, 0.85)
          * pow(fps_ratio, 0.75)
          * codec_factor
          * color_factor
```

初期default候補:

```text
reference_bitrate = 20 Mbps
codec_factor:
  H.264 = 1.00
  HEVC  = 0.75
  AV1   = 0.65
color_factor:
  SDR 4:2:0 = 1.00
  HDR 10-bit = 1.15
  YUV 4:4:4 = 1.35
```

これは固定規格値ではなく、実機試験で更新可能なbootstrap heuristicとする。画質評価、encoder世代、game contentに応じてprofile別に上書き可能にする。

目安として、bootstrapは概ね以下の範囲から開始する。

| Stream | H.264開始目安 | HEVC / AV1開始目安 |
|---|---:|---:|
| 1280x720@60 | 8–15 Mbps | 6–12 Mbps |
| 1920x1080@60 | 15–30 Mbps | 12–24 Mbps |
| 2560x1440@60 | 30–50 Mbps | 22–40 Mbps |
| 2560x1440@120 | 50–90 Mbps | 40–75 Mbps |
| 3840x2160@60 | 60–120 Mbps | 45–100 Mbps |
| 3840x2160@120 | 100–180 Mbps | 80–150 Mbps |

実際の開始値はnetwork estimateとhard ceilingで制限する。

### 7.3 historical good rate

client IDとnetwork fingerprintごとに、以下だけを保存する。

```text
last_successful_wire_bps
last_stable_encoder_bps
p95_rtt_ms
p95_loss_ratio
codec
geometry
network_class
updated_at
```

SSID、public IP、位置情報等をそのまま保存しない。network fingerprintはsalt付きhashとする。

開始値:

```text
if recent stable history exists:
  initial = min(bootstrap, historical_good * 0.90, hard_ceiling)
else if local wired/LAN:
  initial = min(bootstrap, hard_ceiling)
else:
  initial = min(bootstrap * 0.70, hard_ceiling)
```

### 7.4 optional preflight

preflightは既定で200–500ms程度の小さいpacket trainとRTT sampleだけを使う。回線最大速度を測るために数百MBを送らない。

preflightが不明確でも接続を拒否せず、低めに開始してpassive rampする。

## 8. 配信中の自動bitrate制御

### 8.1 状態

```text
STARTUP
  └─ RAMP_UP
       ├─ STABLE
       ├─ SUSPECTED_CONGESTION
       ├─ CONGESTED
       ├─ RECOVERY
       └─ QUALITY_FLOOR
```

### 8.2 sample interval

- packet counterは送信時にatomic加算
- controller評価は500ms〜1秒
- encoder reconfigureは最短1秒、既定2秒間隔
- Web UI更新は1秒
- 永続履歴は1分集約

制御loopをpacket送信threadへ入れない。

### 8.3 signal

stock Moonlight互換で使用可能:

- loss count / interval
- last good frame
- periodic ping / ENet RTTが取得できる場合のRTT
- host socket send queue
- packet pacing lateness
- encoded frame sizeとwire throughput
- IDR request頻度
- connection timeout / frame timeout

Artemis拡張で追加可能:

- client receive bitrate
- jitter buffer duration
- decode queue duration
- render queue duration
- decode drop
- display missed frame
- Wi-Fi / cellular / Ethernet classification
- client estimated available bandwidth

### 8.4 輻輳判定

単一signalで決定しない。

```text
strong congestion:
  loss high
  OR last good frame stalls
  OR send queue grows continuously
  OR RTT rises sharply with queue growth

wireless random loss candidate:
  loss exists
  AND RTT stable
  AND send queue stable
  AND frame delivery continues
```

random loss候補では、bitrateを大幅に下げる前にholdする。将来のdynamic FEC対応時はFEC調整を優先候補とする。

### 8.5 既定制御値

値は設定・実機試験で調整可能とする。

```text
clean condition:
  loss < 0.1%
  RTT inflation < 10%
  send queue stable
  last good frame advancing

warning condition:
  loss 0.1–0.5%
  OR RTT inflation 10–25%

congestion condition:
  loss > 0.5%
  OR RTT inflation > 25%
  OR send queue exceeds threshold

severe condition:
  loss > 2%
  OR frame progress stalls
  OR repeated IDR requests
```

制御action:

```text
clean for 2–4s:
  +3–5%

warning:
  hold current bitrate

congestion:
  -10–15%
  hold 5s

severe:
  -25–35%
  request IDR when appropriate
  hold 8–10s
```

上昇は緩やか、下降は速くする。1回のsampleで上下を繰り返さないようhysteresisとminimum hold timeを設ける。

### 8.6 wire budgetとencoder target

```text
wire_budget = min(
  hard_ceiling,
  estimated_available_bandwidth * safety_margin)

video_budget = wire_budget
             - audio_budget
             - protocol_budget

encoder_target = video_budget / (1 + fec_ratio)
```

既定safety margin:

```text
wired LAN: 0.90
stable Wi-Fi: 0.82
WAN / Tailscale: 0.75
cellular: 0.65
```

network classが不明な場合は0.75から開始し、stable historyで改善する。

### 8.7 low-latency rate control

bitrate変更時も以下を維持する。

- B-frameを新たに有効化しない
- lookaheadを新たに有効化しない
- encoder queueを深くしない
- VBVを過大にしない
- capture FPSを不用意に落とさない
- packet burstを作らずpacingする

`vbv_ms`はbackend capabilityに応じた低遅延範囲とし、既定33ms前後を候補に実機評価する。品質不足時も、数百msのbufferを追加して遅延を隠さない。

### 8.8 quality floor

各geometry / codecに最低品質bitrateを定義する。

current targetがquality floor未満へ15〜30秒留まる場合:

1. Web UIへ「現在の回線では選択解像度の品質を維持できない」と表示
2. stock Moonlightではbitrateをfloor以下へ無制限に落とさず、ユーザー設定に応じて継続または次回接続時の下位解像度を提案
3. Artemis拡張では、client同意済みの場合に限りseamless reconfigureを将来実装
4. 自動解像度変更が無効なら、解像度を勝手に変更しない

### 8.9 geometry downgrade ladder

同一aspect ratioを優先する。

```text
2560x1600
→ 1920x1200
→ 1600x1000
→ 1280x800
```

```text
3440x1440
→ 2560x1072
→ 1920x804
→ 1720x720
```

標準解像度へ無理に変換してaspectを壊さない。候補はencoder alignmentへ補正する。

mid-stream downgradeは初期実装では行わず、次回接続profileまたは明示reconnectに使う。

## 9. stock MoonlightとArtemisの互換戦略

### 9.1 stock Moonlight

追加client改修なしで次を実現する。

- client要求width / height / FPSを仮想Gamescopeへ反映
- client bitrateをhard ceilingとして使用
- standard loss reportでhost target bitrateを下方・上方調整
- Web UIで現在値と調整理由を表示
- sessionごとのstable history

stock Moonlightが知らないhost-side bitrate低下はdecoder互換性を壊さない。encoder stream format、resolution、codecは維持する。

### 9.2 Artemis拡張

optional control extensionを追加する。

```text
SS_DISPLAY_CAPS_V1
SS_NETWORK_FEEDBACK_V1
SS_RATE_STATUS_V1
SS_GEOMETRY_RECONFIGURE_V1  // 将来
```

`SS_DISPLAY_CAPS_V1`例:

```json
{
  "native_width": 2400,
  "native_height": 1080,
  "viewport_width": 2400,
  "viewport_height": 1080,
  "safe_insets": {"left": 80, "top": 0, "right": 40, "bottom": 0},
  "dpi": 420,
  "orientation": "landscape",
  "max_decode_pixels_per_second": 497664000,
  "dynamic_rate_feedback": true
}
```

`SS_NETWORK_FEEDBACK_V1`例:

```json
{
  "received_bps": 48600000,
  "loss_ratio": 0.0012,
  "jitter_ms": 1.8,
  "decode_queue_ms": 4.2,
  "render_queue_ms": 7.0,
  "dropped_frames": 0,
  "network_type": "wifi"
}
```

受信値は信頼境界外として検証し、host側counterと矛盾する場合はhost側を優先する。

## 10. AMD encoder実装

### 10.1 VAAPI

VAAPI backendは、実機とdriverがruntime rate-control parameter更新を受理するかprobeする。

要件:

- session開始時にdynamic bitrate capabilityを確認
- target / peak / VBVの更新
- update後のread-backまたは次frame sizeで反映確認
- 失敗時は直前値へrollback
- repeated failureでruntime adaptationをそのsessionだけ無効化
- encoder再生成は最後の手段とし、既定では行わない

### 10.2 Vulkan Video

- driver extensionとrate-control capabilityをprobe
- per-session mutable rate state
- command buffer / session parameter更新をencode hot pathから分離
- update completionをfenceで確認
- update中もcapture threadをblockしない

### 10.3 共通

```cpp
struct encoder_rate_limits_t {
  uint64_t minimum_bps;
  uint64_t maximum_bps;
  uint64_t minimum_step_bps;
  uint32_t minimum_update_interval_ms;
  bool runtime_target_supported;
  bool runtime_peak_supported;
  bool runtime_vbv_supported;
};
```

rate controllerはbackend limitに量子化して要求する。

## 11. Webサーバーからアクセス可能にする情報

ゲーム映像をWebブラウザへ配信しなくても、運用情報と設定は認証済みWeb UI / APIから取得可能にする。

### 11.1 session summary

```text
client ID / name
connection start time
app / game
virtual session mode
selected canvas / stream / content rectangle
fit mode / safe area / UI scale
codec / profile / HDR / chroma
requested bitrate
hard ceiling
current wire budget
current encoder target
measured send bitrate
FEC ratio
loss / RTT / jitter / queue
capture FPS / encode FPS / sent FPS
dropped / repeated / IDR frames
capture latency / encode latency / network estimate
GPU / encoder device
selection and adaptation reason
```

### 11.2 API

```text
GET /api/control/v1/stream/summary
GET /api/control/v1/stream/sessions
GET /api/control/v1/stream/sessions/{id}
GET /api/control/v1/stream/sessions/{id}/display
GET /api/control/v1/stream/sessions/{id}/network
GET /api/control/v1/stream/sessions/{id}/encoder
GET /api/control/v1/stream/profiles/clients
GET /api/control/v1/stream/profiles/clients/{client_id}
PATCH /api/control/v1/stream/profiles/clients/{client_id}
POST /api/control/v1/stream/profiles/clients/{client_id}/reset
WS  /api/control/v1/stream/live
```

### 11.3 mutation

Webから変更可能:

- auto / manual bitrate
- client ceiling
- administrator ceiling
- fit mode
- max resolution / FPS
- safe area override
- UI scale override
- auto downgrade consent
- client profile reset

即時変更が安全でないものは`applies_on_next_session`を返す。

### 11.4 secretとprivacy

「全ての情報へアクセス」は運用上必要な情報を意味し、secret本文を返すことではない。

返さないもの:

- pairing private key
- session encryption key
- password / SSH key
- full clipboard content
- terminal keystroke
- raw input payload

## 12. Webブラウザゲーム配信の位置付け

### 12.1 優先度

Webブラウザからのゲーム操作は必須ではない。以下を満たす場合だけ将来評価する。

- Moonlight / Artemis配信性能を低下させない
- 既存encoder sessionを不安定にしない
- 追加GPU encoder sessionが不足する場合に明示拒否できる
- WebRTC gatewayが別processとして障害分離される
- feature flag既定OFF

### 12.2 候補

低遅延が必要なため、実装する場合はWebRTCを候補とし、RDP、VNC、MJPEG、HLSをゲーム配信の主経路にしない。

ただしWebRTCはGameStream / Moonlightとは別のsignaling、congestion control、input、audio、codec negotiationが必要になる。初期完成条件から除外する。

### 12.3 active GameStream時

Web browser game streamは既定で同時起動を拒否するか、共有bitstreamが安全に成立する場合だけ許可する。

管理用Web UI、SSH、低FPSリモートデスクトップは利用可能だが、Resource Priority Governorが負荷を制限する。

## 13. Resource Priority Governor

### 13.1 優先度class

```text
Class 0: capture / encode / audio / input / packet pacing
Class 1: loss feedback / adaptive bitrate / session recovery
Class 2: lightweight metrics snapshot / Web status
Class 3: terminal / SSH / management RDP
Class 4: history persistence / compression / cleanup / update check
Class 5: optional WebRTC experiment
```

### 13.2 degradation order

encoder latency、GPU busy、CPU pressure、disk pressureが上昇した場合:

```text
1. Web UI updateを1Hz→0.5Hzへ低下
2. process detail収集を停止
3. amd-smi / rocm-smi fallbackを停止
4. history flushを延期
5. management RDPを15FPS以下へ制限
6. management RDPのimage qualityを低下
7. optional WebRTCを停止
8. terminal scrollback snapshot生成を延期
```

ゲームstream bitrateを管理機能のために下げない。

### 13.3 CPU scheduling

- stream critical threads: upstream Sunshine方針を維持
- adaptive controller: normal-high、短時間処理
- metrics collector: nice 10以上
- persistence / cleanup: nice 15以上、可能ならI/O idle class
- guacd / xrdp: systemd CPUWeightをstreamより低くする
- optional WebRTC:最も低いGPU / CPU priority

### 13.4 GPU

- metricsはread-only sysfsを優先
- GPU profile変更はstream開始前または明示操作時だけ
- active encode中のclock / fan polling頻度を上げすぎない
- management desktopはGPU accelerationを既定OFFまたは低優先度
- encoder session limitを超える処理を開始しない

## 14. SSD消耗の最小化

### 14.1 runtime path

```text
/run/user/$UID/steamshine/
  sessions/
  telemetry/
  sockets/
  locks/
```

runtime pathはtmpfsを使用する。

### 14.2 書き込み禁止事項

次をSSDへ逐次書き込まない。

- per-packet stats
- per-frame stats
- 1秒raw metrics
- WebSocket送信queue
- terminal live outputの複製
- remote desktop frame
- client jitter sample
- adaptive controller全decision

### 14.3 session state

crash recoveryに必要なstateだけを状態遷移時にatomic writeする。

```text
Preparing
Streaming
Stopping
Recovered
```

bitrate変更ごとにstate fileを書き換えない。

### 14.4 metrics history

既定:

```text
raw 1s: RAM 15分
10s aggregate: RAM 6時間
1m aggregate: persistent optional
1h aggregate: persistent optional
```

persistent historyを有効にした場合:

- 1分aggregateをRAMへ蓄積
- 5分または10分ごとに1 transactionでbatch insert
- active stream中は最大15分までflush延期可能
- database WAL checkpointはstream終了後または十分なidle時
- VACUUM / compressionは手動またはidle maintenance windowのみ
- retention削除もbatch処理

既定のpersistent historyは`off`または`minimal`を選べるようにする。

```text
off:
  設定、profile、audit、crash state以外の時系列値を保存しない

minimal:
  1分aggregateを7日、1時間aggregateを90日

standard:
  1分aggregateを30日、1時間aggregateを1年
```

SteamOS向け初期defaultは`minimal`とする。

### 14.5 log

- in-memory ring logを基本とする
- persistent logはwarning以上を既定
- repeated messageをrate limit / coalesce
- per-frame debug logは明示debug sessionだけ
- log rotationを小さく保つ
- stream中の圧縮を行わない
- session終了後にsummary 1件だけ保存

### 14.6 書き込み目標

目標値:

```text
history=off:
  通常監視による周期SSD書き込み 0

history=minimal:
  典型運用で10 MiB/day未満
  1分ごとのfsyncは禁止

active game stream:
  配信処理自体による継続的SSD書き込み 0
```

ゲーム、Steam、shader cache等のSteamOS自身の書き込みは本目標の対象外とする。

## 15. 処理負荷目標

管理機能を有効にした場合の追加負荷目標:

| 項目 | idle | active game stream |
|---|---:|---:|
| adaptive controller CPU | 0 | 0.1%未満を目標 |
| resource collector CPU | 0.5%未満 | 0.5%未満 |
| Web status配信 CPU | clientなしで0に近い | 1 clientで0.5%未満 |
| GPU monitoring | sysfs readのみ | sysfs readのみ |
| persistent history | batch時のみ | 原則延期 |
| RAM | 64 MiB未満を目標 | 128 MiB未満を上限目標 |

これは受入試験で確認するtargetであり、ハードウェア差を記録する。

## 16. Web UI

### 16.1 Stream Overview

最上段:

```text
Streaming: 2560x1600 @ 120 FPS
Codec: AV1 8-bit 4:2:0
Bitrate: 61.2 Mbps / ceiling 80 Mbps
Network: Stable
Loss: 0.03%
RTT: 4.8 ms
Host processing: 3.2 ms
```

### 16.2 Display card

- Client viewport
- safe area
- selected virtual canvas
- content rectangle
- fit mode
- bar / crop
- UI scale
- alignment correction
- selection reason

例:

```text
Selected 1920x1200 instead of 2560x1600
Reason: current network quality estimate cannot sustain the 2560x1600 quality floor.
Host physical monitor: ignored in Virtual Session Mode.
```

### 16.3 Network card

- requested ceiling
- policy ceiling
- current target
- measured wire rate
- estimated available bandwidth
- loss
- RTT / variation
- queue
- controller state
- last adjustment
- adjustment reason

### 16.4 Performance card

- capture / encode / send FPS
- capture / encode latency
- encoder queue
- GPU utilization / encoder utilization
- VRAM
- temperature / hotspot / fan / power
- CPU / memory
- control-plane CPU
- persistent write rate

### 16.5 settings

```text
Display mode:
  Auto exact / Fit / Fill safe / Integer / Custom

Resolution policy:
  Match client / Match aspect / Fixed maximum / Manual

Bitrate:
  Auto quality-first / Auto balanced / Auto data-saving / Manual

Automatic resolution downgrade:
  Off / Suggest / Reconnect with consent / Artemis seamless experimental

Persistent history:
  Off / Minimal / Standard
```

SteamOS初期default:

```text
Display: Auto exact with Fit fallback
Bitrate: Auto quality-first
Resolution downgrade: Suggest
History: Minimal
Web game streaming: Off
```

## 17. エラー処理

### 17.1 不正display情報

- invalid fieldだけを無視
- stock requestへfallback
- safe default 1920x1080@60等は最後のfallback
- Web UIへvalidation理由を表示
- crashさせない

### 17.2 loss feedback欠落

- host send queueとRTTだけで保守的に継続
- bitrateを無制限に上げない
- controller stateを`feedback_limited`と表示

### 17.3 encoder reconfigure失敗

1. 直前targetを維持
2. 1回だけ再試行
3. backend runtime adaptationをsession中無効化
4. streamは継続
5. Web UIへwarning
6. encoderを自動再生成しない

### 17.4 control sidecar停止

- current streamは継続
- last applied bitrateを維持
- host内最小controllerがstream daemon側にある場合は継続
- Web UIだけdegraded表示
- sidecar復旧後にsnapshot再同期

### 17.5 database failure

- telemetryはRAMで継続
- persistent historyだけ停止
- stream継続
- repeated error logを抑制

## 18. API data contract

### 18.1 live summary

```json
{
  "session_id": "uuid",
  "priority": "game_stream",
  "client": {
    "id": "client-hash",
    "name": "Artemis Phone",
    "protocol": "artemis-extension-v1"
  },
  "display": {
    "requested": {"width": 2400, "height": 1080, "fps": 120},
    "selected": {"width": 2400, "height": 1080, "fps": 120},
    "content": {"x": 0, "y": 0, "width": 2400, "height": 1080},
    "safe": {"x": 80, "y": 0, "width": 2280, "height": 1080},
    "fit_mode": "exact",
    "host_display_ignored": true,
    "reason": "exact client viewport supported"
  },
  "rate": {
    "mode": "auto_quality_first",
    "requested_ceiling_bps": 80000000,
    "hard_ceiling_bps": 80000000,
    "wire_budget_bps": 66200000,
    "encoder_target_bps": 54800000,
    "measured_wire_bps": 64100000,
    "state": "stable",
    "last_action": "increase_3_percent",
    "reason": "clean loss and stable RTT"
  },
  "network": {
    "loss_ratio": 0.0003,
    "rtt_ms": 4.8,
    "rtt_variance_ms": 0.7,
    "send_queue_bytes": 0
  }
}
```

### 18.2 WebSocket

- 1Hz default
- userがtab非表示の場合0.2Hzまたは停止
- latest-only queue
- 1 clientあたりqueue depth 1
- slow consumerはsnapshot drop
- reconnect時にfull snapshot 1件
- raw packet listを送らない

## 19. 実装フェーズ

### A0: 優先度・観測基盤

- stream / control / maintenance priority class
- in-memory telemetry bus
- Web summary read-only API
- no per-frame disk writes test

完了条件:

- Web sidecarを停止してもstream継続
- live metrics subscriberが遅くてもencode latency不変

### A1: Client Display Normalizer

- stock Moonlight fields
- client profile
- capability validation
- exact / fit geometry
- Gamescope起動値へ反映

完了条件:

- host物理解像度を変更してもvirtual stream geometry不変
- 1280x800、1920x1080、2560x1600、3440x1440を正しく選択

### A2: Artemis Display Extension

- viewport / safe area / DPI
- schema version
- fallback
- Web UI表示

### A3: Initial Auto Bitrate

- bootstrap formula
- hard ceiling
- historical good rate
- passive ramp
- manual override

### A4: stock Moonlight Loss Adaptation

- loss report parser validation
- controller state machine
- host queue / RTT signal
- current bitrate Web表示
- simulation test

### A5: AMD Runtime Rate Adapter

- VAAPI capability probe
- Vulkan Video capability probe
- safe runtime update
- read-back
- rollback
- failure isolation

### A6: Quality Floorと次回解像度提案

- aspect-preserving ladder
- client consent
- no automatic stock Moonlight reconnect default
- Artemis experimental reconfigure設計

### A7: SSD / Load Hardening

- tmpfs runtime
- RAM ring buffers
- 5–10分batch persistence
- stream-active maintenance deferral
- log rate limit
- CPU / I/O priority

### A8: Optional Web Game Feasibility

A0〜A7完了後だけ開始する。

- WebRTC sidecar PoC
- separate process / feature flag
- encoder resource detection
- simultaneous stream impact test
- Moonlight / Artemis latency regression test

regressionがあれば実装を採用しない。

## 20. テスト計画

### 20.1 display matrix

| Client | Requested | Expected |
|---|---|---|
| Steam Deck | 1280x800@60 | exact 16:10 |
| 1080p TV | 1920x1080@60 | exact 16:9 |
| 1600p tablet | 2560x1600@120 | exact or same-aspect downgrade |
| ultrawide | 3440x1440@120 | exact when game supports, fit otherwise |
| mobile | custom wide + safe inset | exact canvas + safe overlay |
| portrait client | portrait request | explicit supported mode or safe fallback |

各caseで物理host monitorを以下へ変更しても結果が変わらないことを確認する。

```text
monitor disconnected
1920x1080
2560x1440
3440x1440
4K
monitor powered off
```

### 20.2 network emulation

`tc netem`等で以下を試験する。

```text
bandwidth: 10 / 20 / 40 / 80 / 150 Mbps
RTT: 2 / 10 / 30 / 80 ms
loss: 0 / 0.1 / 0.5 / 1 / 3%
jitter: 0 / 2 / 10 / 30 ms
reordering
short burst loss
bandwidth step-down / step-up
Wi-Fi-like random loss
queue buildup
```

確認:

- congestion時に5秒以内を目標に下降開始
- clean回復時は急上昇しない
- oscillationしない
- RTT増加を抑える
- frame deliveryが停止しない
- user ceilingを超えない

### 20.3 encoder

- VAAPI H.264 / HEVC / AV1対応範囲
- Vulkan Video対応範囲
- bitrate update中のframe gap
- target反映時間
- IDRへの影響
- encoder crash / driver reset
- unsupported runtime update fallback

### 20.4 performance

active stream中に以下を比較する。

```text
baseline SteamShine
+ adaptive controller
+ Web monitor 1 client
+ terminal idle
+ RDP idle
+ history minimal
```

測定:

- capture latency
- encode latency
- frame time p50 / p95 / p99
- sent FPS
- dropped frame
- CPU per thread
- GPU encode utilization
- VRAM
- context switch
- disk write bytes
- fsync count

### 20.5 SSD

- 8時間stream
- history off / minimal / standard
- Web UI開閉
- repeated connect / disconnect
- alert発生
- crash recovery

`pidstat -d`、`iotop`、`iostat`等でSteamShine由来のwriteを分離する。

### 20.6 long-run

- 8時間1440p120
- 50回connect / disconnect
- network切替
- client suspend / resume
- SteamShine control sidecar restart
- Web UI slow client
- database read-only / full disk

## 21. 受入条件

### 21.1 表示

- Virtual Session Modeでhost物理画面サイズに依存しない。
- client requested aspectを維持する。
- exact未対応時にfitへ安全にfallbackする。
- 既定でcropやstretchを行わない。
- safe areaが提供された場合、SteamShine overlayがsafe areaを外れない。
- selected geometryと理由をWebから確認できる。

### 21.2 bitrate

- client ceilingを超えない。
- 回線帯域低下時にloss / queue / RTTを使ってtargetを下げる。
- 回線回復時にhysteresis付きでtargetを上げる。
- bitrate変更がencoder crashや長いfreezeを起こさない。
- runtime update非対応backendでもstreamを維持する。
- current target、wire rate、loss、RTT、controller stateをWebから確認できる。

### 21.3 latency / quality

- adaptive controller追加によるhost processing latency増加は測定誤差範囲を目標とする。
- monitoringのためにcapture FPSを落とさない。
- management RDPやhistory処理でencode frame dropを起こさない。
- 1440p120 SDRを主要完成目標とする。

### 21.4 SSD / load

- history offでは周期monitor writeが0。
- active stream中に1秒ごとのDB write / fsyncがない。
- persistent historyはbatch transaction。
- collectorはsysfs fast pathを使用する。
- management feature停止時は追加負荷がほぼ0。

### 21.5 compatibility

- stock Moonlightで基本自動表示と自動bitrateが動く。
- Artemis拡張がなくても接続できる。
- extension version不一致時は標準protocolへfallbackする。
- adaptive機能を無効化すれば従来Sunshine相当の固定bitrate動作へ戻せる。

## 22. ロールバック

設定:

```text
adaptive_display = disabled
adaptive_bitrate = disabled
artemis_extended_feedback = disabled
persistent_history = off
web_game_streaming = disabled
```

fallback:

- virtual geometry失敗 → profile default → 1920x1080@60 → Existing Gaming Session
- adaptive controller失敗 → last stable target固定
- runtime encoder update失敗 → session開始値固定
- control sidecar失敗 → stream daemon単独継続
- history失敗 → RAM-only

## 23. 推奨ソース構成

```text
src/
  streaming/adaptive/
    client_capability_normalizer.cpp
    client_capability_normalizer.h
    display_policy_engine.cpp
    display_policy_engine.h
    network_bootstrap_estimator.cpp
    network_bootstrap_estimator.h
    adaptive_rate_controller.cpp
    adaptive_rate_controller.h
    encoder_rate_adapter.cpp
    encoder_rate_adapter.h
    telemetry_bus.cpp
    telemetry_bus.h
    resource_priority_governor.cpp
    resource_priority_governor.h

  platform/linux/encode/
    vaapi_rate_adapter.cpp
    vaapi_rate_adapter.h
    vulkan_rate_adapter.cpp
    vulkan_rate_adapter.h

  protocol/extensions/
    artemis_display_caps.cpp
    artemis_display_caps.h
    artemis_network_feedback.cpp
    artemis_network_feedback.h

  control/
    stream_status_api.cpp
    stream_status_api.h
    stream_profile_store.cpp
    stream_profile_store.h

src_assets/common/assets/web/
  stream/
    StreamOverview.vue
    DisplayGeometryCard.vue
    AdaptiveRateCard.vue
    StreamPerformanceCard.vue
    ClientProfileDialog.vue

tests/
  adaptive/
  network_emulation/
  performance/
```

既存SteamShine / Sunshineのsource layoutへ合わせ、最終配置は実装時に調整する。

## 24. 最初の縦切り

最初に以下だけを実装する。

```text
stock Moonlight
→ requested 1920x1080@60 / bitrate ceiling取得
→ host physical monitorを無視
→ headless Gamescope 1920x1080@60
→ VAAPI H.264
→ loss reportをRAM counterへ反映
→ Web UIへrequested / selected / current bitrate / loss表示
→ runtime bitrateはまだ変更しない
→ disconnect後に状態破棄
```

次にVAAPI runtime bitrate updateを追加する。

この順序により、表示選択、観測、encoder制御の不具合を分離して検証する。

---

この設計では、SteamShineの中心をWeb管理機能ではなく、Moonlight / Artemisへ向けた仮想デスクトップゲーム配信に置く。ホストの物理画面から独立したclient-native geometryを生成し、既存loss feedbackと追加telemetryから回線に合うbitrateを選び、監視・履歴・リモート操作はゲーム配信へ影響しない低優先度control planeとして動作させる。