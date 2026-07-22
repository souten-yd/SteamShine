# SteamShine: SteamOS仮想ストリーミングセッション改修計画

## 1. 目的

SteamShineをSteamOS向けに最適化し、ApolloのWindows版に近い操作感で、Moonlight／Artemis接続時にクライアント専用の仮想デスクトップを自動生成してゲームストリーミングできるようにする。

目標とする利用体験は以下とする。

1. SteamOSを起動したまま、物理モニターの電源状態に依存せず接続できる。
2. Moonlight／Artemisから接続すると、クライアントの解像度・FPSに合った仮想表示環境を自動生成する。
3. ゲームまたはSteam Big Pictureを仮想表示環境内に確実に起動する。
4. 映像はGamescopeからPipeWire経由でSunshineへ渡し、AMD GPUではVAAPIまたはVulkan Videoでハードウェアエンコードする。
5. 切断・アプリ終了・クラッシュ時に仮想セッション、音声、入力、子プロセスを確実に破棄する。
6. SteamOS更新で壊れにくいユーザー領域中心の導入方式とする。

## 2. 非目標

初期実装では以下を対象外とする。

- Linuxカーネルへ恒久的な仮想ディスプレイドライバを追加すること
- KDE／GNOMEの通常のディスプレイ設定にPnPモニターとして常駐表示すること
- 複数クライアントへの独立ゲームセッション同時配信
- HDR、VRR、10-bit色深度の完全対応
- 物理Gaming Modeと独立Steamセッションの完全同時利用
- upstream Sunshine／Gamescopeへの即時統合を前提とした設計

これらは基礎機能の安定後に段階的に扱う。

## 3. 基本方針

Windows版ApolloのSudoVDA相当をLinux向けに新規開発するのではなく、SteamOSに既に含まれるGamescopeをクライアント専用の仮想デスクトップとして利用する。

```text
Moonlight / Artemis
        │
        │ 接続要求: width / height / fps / hdr / audio
        ▼
SteamShine (Sunshine fork)
        │
        │ Virtual Session Manager
        ▼
Headless Gamescope Session
        │  専用Wayland / Xwayland / Steam環境
        │
        ├── PipeWire Video Source ──► SteamShine Capture ──► Encoder
        ├── PipeWire Audio Sink  ───► SteamShine Audio Capture
        └── Virtual Input Devices ◄── Moonlight Input
```

この方式ではOSレベルの仮想モニターではなく、Gamescope内に独立した仮想表示環境を生成する。ユーザーから見た接続・解像度追従・切断時破棄の使い勝手をApolloへ近づける。

## 4. 設計原則

### 4.1 SteamOS非破壊

- SteamOSのrootfs変更を最小化する。
- SteamOS付属Gamescopeを置換しない。
- 実行ファイル、設定、状態は原則としてユーザー領域に保存する。
- systemd user serviceを基本とする。
- OS更新後に再適用が必要な権限設定は、単一のセットアップスクリプトで復旧可能にする。

推奨配置:

```text
~/.local/bin/steamshine
~/.local/lib/steamshine/
~/.config/steamshine/
~/.config/systemd/user/steamshine.service
~/.local/state/steamshine/
~/.cache/steamshine/
```

### 4.2 既存Sunshine資産の再利用

- GameStream／Moonlightプロトコル
- 接続クライアント情報
- ハードウェアエンコーダー
- PipeWireキャプチャ
- 仮想入力
- Web UI
- アプリ定義
- prep／undo command機構

新規実装は仮想セッション管理とGamescope統合へ集中する。

### 4.3 状態機械による制御

単純なシェルスクリプト連結ではなく、接続単位の明示的な状態機械を導入する。

```text
Idle
  └─► Preparing
         ├─► StartingGamescope
         ├─► WaitingForPipeWire
         ├─► ConfiguringAudio
         ├─► ConfiguringInput
         └─► LaunchingApp
                └─► Streaming
                       ├─► Stopping
                       └─► Recovering
                              └─► Idle
```

各遷移にタイムアウト、エラー理由、ロールバック処理を持たせる。

## 5. 主要コンポーネント

## 5.1 Virtual Session Provider API

プラットフォーム依存実装を分離する共通APIを追加する。

```cpp
struct virtual_session_request_t {
  std::string client_id;
  std::string app_id;
  int width;
  int height;
  int refresh_rate;
  bool hdr;
  int audio_channels;
};

struct virtual_session_result_t {
  std::string session_id;
  std::string wayland_display;
  std::string x11_display;
  uint32_t pipewire_video_node_id;
  uint64_t pipewire_video_object_serial;
  std::string audio_sink_name;
};

class virtual_session_provider_t {
 public:
  virtual virtual_session_result_t start(
      const virtual_session_request_t& request) = 0;
  virtual void stop(const std::string& session_id) = 0;
  virtual void recover_stale_sessions() = 0;
  virtual ~virtual_session_provider_t() = default;
};
```

Linux／SteamOS実装名:

```text
GamescopeVirtualSessionProvider
```

## 5.2 Gamescope Session Manager

責務:

- セッションID生成
- 専用runtime directory作成
- Gamescope起動
- readiness待機
- PipeWireノード特定
- Wayland／Xwayland環境取得
- systemd user scopeまたはcgroupへのプロセス収容
- 子プロセス監視
- 終了時一括停止
- 起動時の孤立セッション回収

想定起動形式:

```bash
gamescope \
  --backend headless \
  --output-width "$WIDTH" \
  --output-height "$HEIGHT" \
  --nested-width "$WIDTH" \
  --nested-height "$HEIGHT" \
  --nested-refresh "$FPS" \
  --expose-wayland \
  --xwayland-count 1 \
  --ready-fd "$READY_FD" \
  -- steamshine-session-launcher
```

実際のオプションはSteamOS同梱Gamescopeのバージョンを実機検証して確定する。

## 5.3 Gamescope Capture Backend

新規設定値:

```text
capture = gamescope
```

内部では既存PipeWireキャプチャを再利用する。

要件:

- Virtual Session Managerが返すnode IDまたはobject serialを使用する。
- 毎回変化するPipeWire node IDを設定ファイルへ固定しない。
- ノード名、object serial、管理FDのいずれかでセッションと映像ソースを一意に関連付ける。
- DMA-BUFを優先し、失敗時のみ共有メモリへフォールバックする。
- 解像度変更時はPipeWire format renegotiationを行う。

ログ解析のみでnode IDを得る方式はPoC限定とし、正式実装には使用しない。

## 5.4 Session Launcher

ゲームとSteamを正しい仮想セッションへ起動するラッパーを追加する。

渡す環境変数例:

```text
WAYLAND_DISPLAY=<session-wayland>
DISPLAY=<session-xwayland>
XDG_RUNTIME_DIR=<session-runtime>
GAMESCOPE_WAYLAND_DISPLAY=<session-wayland>
PULSE_SERVER=<pipewire-pulse-server>
STEAMSHINE_SESSION_ID=<uuid>
```

役割:

- Steam Big Picture起動
- Steam URIまたはゲーム実行ファイル起動
- Proton環境の継承
- ゲーム終了検知
- Steam Overlay互換性維持
- 既存Gaming Mode側へ誤って起動されないことの保証

## 5.5 Audio Session Manager

接続ごとに専用PipeWire仮想sinkを生成する。

命名例:

```text
steamshine.<session-id>.output
steamshine.<session-id>.monitor
```

処理:

1. 仮想sink作成
2. セッション内ゲームの出力先を仮想sinkへ固定
3. monitor sourceをSteamShineへ渡す
4. 切断時に既定出力を復元
5. 仮想sinkを削除

2.0／5.1／7.1を段階対応する。MVPは2.0のみとする。

## 5.6 Input Session Manager

MVPでは既存SunshineのLinux入力実装を再利用する。

検証対象:

- 相対マウス
- 絶対座標タッチ
- キーボード
- Xbox系ゲームパッド
- DualSense
- Steam Input
- Steam Overlay
- ソフトウェアキーボード

複数同時セッションを実装するまでは、入力デバイスをセッション間で分離する必要はない。ただし将来の分離を妨げないインターフェースとする。

## 5.7 Client Profile Store

Apolloのクライアント別固定仮想ディスプレイ設定に相当する機能をSteamShine側で保持する。

保存項目:

```text
client_id
preferred_width
preferred_height
preferred_refresh_rate
hdr_enabled
ui_scale
audio_layout
preferred_codec
preferred_encoder
last_app_id
local_display_policy
```

クライアント証明書またはSunshine内の一意識別子をキーにする。

## 6. 動作モード

## 6.1 Virtual Session Mode

推奨モード。接続時に専用headless Gamescopeを生成する。

用途:

- 物理モニターOFF
- ダミープラグなし
- クライアント固有解像度
- スマートフォン、タブレット、ウルトラワイド

## 6.2 Existing Gaming Session Mode

既存SteamOS Gaming Modeを直接配信する互換モード。

用途:

- 最小遅延を優先
- ローカル画面と同じ内容を配信
- Virtual Session Modeが動作しないゲームへのフォールバック

## 6.3 Exclusive Streaming Mode

Gaming Modeを一時停止し、リモート専用Gamescopeへ切り替える安定優先モード。

初期Steam統合ではこの方式を採用する。

```text
Gaming Mode
  └─ remote connection
       ├─ current session state save
       ├─ local Gaming Mode suspend/stop
       ├─ virtual streaming session start
       └─ disconnect
            ├─ streaming session stop
            └─ Gaming Mode restore
```

## 7. Web UI改修

「Audio/Video」またはSteamOS専用タブへ以下を追加する。

### Virtual Display

- Virtual session: Off / Auto / Always
- Session mode: Virtual / Existing Gaming Session / Exclusive Streaming
- Match client resolution: On / Off
- Match client refresh rate: On / Off
- Default resolution
- Default refresh rate
- Maximum resolution
- Maximum refresh rate
- Unsupported mode fallback
- Local display policy: Keep / Blank / Disable / Restore after disconnect

### Gamescope

- Gamescope executable
- Backend
- Preferred Vulkan device
- Headless timeout
- PipeWire discovery timeout
- Additional arguments
- Enable HDR experimental

### Recovery

- Kill stale sessions on startup
- Session shutdown timeout
- Force kill after timeout
- Restore Gaming Mode after crash
- Restore audio after crash

危険な自由入力オプションはAdvancedへ隔離し、通常設定では安全なプリセットを使用する。

## 8. SteamOSインストール方式

## 8.1 推奨

- SteamShine独自ビルドをユーザー領域へ配置
- systemd user serviceとして起動
- 必要なuinput／KMS／PipeWire権限だけセットアップスクリプトで付与
- SteamOS同梱Gamescopeを利用

## 8.2 セットアップスクリプト

以下を一括実行する。

```text
scripts/install-steamos.sh
```

責務:

- 依存確認
- SteamOS／Arch系判定
- SteamShine配置
- systemd user unit配置・enable
- uinput権限確認
- PipeWire確認
- Gamescopeオプション対応確認
- VAAPI確認
- 設定バックアップ
- 動作診断

アンインストール:

```text
scripts/uninstall-steamos.sh
```

ユーザーデータを残すか削除するか選択可能とする。

## 9. 実装フェーズ

## Phase 0: 調査・基準固定

### 作業

- SteamOS対象バージョンを記録
- Gamescopeバージョンと利用可能オプションを取得
- Sunshine既存PipeWire経路を確認
- AMD VAAPI／Vulkan Video動作確認
- 現行Gaming Modeの起動構成と環境変数を記録
- 接続時処理順序をトレース

### 成果物

- `docs/STEAMOS_ENVIRONMENT.md`
- `docs/ARCHITECTURE.md`
- 実機診断スクリプト
- PoC実行ログ

### 完了条件

- headless Gamescopeがテスト画面を描画できる
- PipeWireにVideo/Sourceが現れる
- Sunshineまたはテストツールから取得できる

## Phase 1: SDR Virtual Desktop PoC

### 対象

- 1クライアント
- 1920x1080@60
- SDR
- Steam以外のテストアプリ
- AMD VAAPI
- ステレオ音声

### 作業

- Virtual Session Managerの最小実装
- Gamescope起動／終了
- PipeWireノード検出
- Sunshineキャプチャ接続
- キーボード／マウス入力
- systemd scopeでの一括終了

### 完了条件

- 物理モニターOFFで接続できる
- 10回連続接続／切断に成功する
- 切断後にGamescope、Xwayland、音声ノードが残らない
- 1080p60を30分配信して重大なフレーム破損がない

## Phase 2: 動的解像度・FPS

### 対象

- 1280x800
- 1920x1080
- 2560x1440
- 3440x1440
- 60／90／120Hz

### 作業

- クライアント要求値の検証・上限適用
- Gamescopeへ動的反映
- PipeWire renegotiation
- 解像度非対応時の安全なフォールバック
- クライアントプロファイル保存

### 完了条件

- 対象解像度・FPSで接続時に自動一致する
- 物理画面の解像度を変更しない
- 奇数解像度・4:2:0制約を安全に補正する
- 異常値でプロセスがクラッシュしない

## Phase 3: Steam Big Picture統合

### 方針

最初はExclusive Streaming Modeを採用する。

### 作業

- Steam／Big Pictureを専用Gamescopeへ起動
- 既存Gaming Modeとの切替
- Steam URI起動
- Protonゲーム起動
- Overlay／Steam Input確認
- 切断後のGaming Mode復帰

### 完了条件

- 主要なProtonゲーム3本以上で起動・入力・終了が成功
- ゲームが物理画面側へ誤起動しない
- 通信断後もGaming Modeへ復帰できる
- SteamShine再起動後に正常復旧する

## Phase 4: 音声・入力安定化

### 作業

- セッション専用PipeWire sink
- 2.0／5.1／7.1
- DualSense／Xbox／Steam Input
- タッチ／相対マウス
- カーソル表示とフォーカス

### 完了条件

- 一時停止／再接続で音声を失わない
- ゲーム終了後にホスト音声が正常復元される
- 主要入力デバイスで30分以上の連続操作が可能

## Phase 5: Apollo相当UX

### 作業

- クライアント別設定
- 接続時自動生成／切断時破棄
- UI上の状態表示
- セッションログと診断
- local display policy
- 自動フォールバック

### 完了条件

- 初回設定後はMoonlight側から通常接続するだけで利用できる
- ダミープラグが不要
- セッション失敗時に既存画面配信へ自動フォールバック可能
- エラー原因をWeb UIで確認できる

## Phase 6: 高度機能

基礎安定後に個別評価する。

- HDR／10-bit
- AV1最適化
- 物理画面との同時利用
- 複数仮想セッション
- セッション別入力分離
- セッション別Steamプロセス
- KWin virtual output／VKMS／AMDGPU virtual display比較
- Gamescope upstream拡張

## 10. エラー処理と復旧

各セッションで以下を永続記録する。

```json
{
  "session_id": "uuid",
  "state": "streaming",
  "gamescope_pid": 1234,
  "child_pids": [1235, 1236],
  "pipewire_video_serial": 987,
  "audio_sink": "steamshine.uuid.output",
  "started_at": "ISO-8601",
  "client_id": "client-key"
}
```

起動時処理:

1. state fileを列挙
2. PIDとcgroupを検証
3. 孤立プロセスを停止
4. 仮想音声ノードを削除
5. Gaming Mode状態を復元
6. state fileをarchive

終了処理は冪等にする。同じstop処理が複数回呼ばれても安全でなければならない。

## 11. セキュリティ

- クライアント入力の権限は既存Sunshineのペアリング認証へ従う。
- 任意シェル引数をクライアント要求から直接生成しない。
- width、height、fps、app IDは型検証とallowlistを通す。
- Gamescope追加引数は管理者設定だけから取得する。
- セッションruntime directoryはユーザーのみアクセス可能にする。
- uinput権限を全ユーザーへ無制限開放しない。
- ログへ証明書、PIN、認証トークンを出力しない。

## 12. 性能目標

MVP目標:

| 項目 | 目標 |
|---|---:|
| 接続要求から映像開始 | 8秒以内 |
| 1080p60追加GPU負荷 | 既存Sunshine比 +10%以内 |
| キャプチャコピー | DMA-BUF優先、CPU full-frame copy回避 |
| 切断後クリーンアップ | 5秒以内 |
| 連続接続／切断 | 50回で孤立プロセスなし |
| 1時間配信 | クラッシュ・映像停止なし |

最終目標:

- 1440p120 SDR安定配信
- AV1またはHEVCハードウェアエンコード
- 物理モニターOFFからの再起動後接続
- 追加フレーム遅延を可能な限り1フレーム未満へ抑制

## 13. テスト計画

## 13.1 単体テスト

- request validation
- 状態遷移
- timeout
- command argument escaping
- session ID
- state file read/write
- cleanup idempotency
- profile migration

## 13.2 統合テスト

- fake Gamescope process
- fake PipeWire node discovery
- Gamescope abnormal exit
- PipeWire node timeout
- encoder initialization failure
- app launch failure
- disconnect during preparation
- SteamShine restart during streaming

## 13.3 実機テスト

最低構成:

- SteamOS
- AMD GPU
- 有線LAN
- Moonlight PC
- Artemis Android

ケース:

- 物理モニターON／OFF
- SteamOS再起動直後
- 1080p60／1440p120
- H.264／HEVC／AV1
- 通常切断／Wi-Fi切断／クライアント強制終了
- ゲームクラッシュ
- Steamクラッシュ
- Gamescopeクラッシュ
- Sunshine再起動
- suspend／resume

## 14. ロールバック

常に既存Sunshine動作へ戻せること。

- `virtual_session = disabled`で新機能を完全無効化
- 既存のKMS／Wayland／X11キャプチャを維持
- 設定schema変更にはversionとmigrationを付与
- 新機能失敗時に既存Desktop appへフォールバック可能にする
- インストーラー実行前に設定をバックアップする
- SteamOS Gaming Modeへの復帰コマンドを独立して提供する

緊急復旧コマンド例:

```bash
steamshine-recover --stop-all --restore-audio --restore-gaming-mode
```

## 15. 推奨ディレクトリ構成

```text
src/
  platform/linux/virtual_session/
    virtual_session_provider.h
    gamescope_session_manager.cpp
    gamescope_session_manager.h
    pipewire_node_discovery.cpp
    pipewire_node_discovery.h
    audio_session_manager.cpp
    audio_session_manager.h
    session_state_store.cpp
    session_state_store.h

  capture/
    gamescope_capture.cpp
    gamescope_capture.h

  steam/
    steam_session_launcher.cpp
    steam_session_launcher.h

src_assets/common/assets/web/
  configs/tabs/SteamOSVirtualSession.vue

scripts/
  install-steamos.sh
  uninstall-steamos.sh
  diagnose-steamos.sh
  recover-steamos-session.sh

docs/
  STEAMOS_VIRTUAL_SESSION_PLAN.md
  STEAMOS_ENVIRONMENT.md
  ARCHITECTURE.md
  TEST_PLAN_STEAMOS.md
```

既存ソース構造へ合わせて最終配置を調整する。

## 16. 最初に実装する縦切り

初回の実装タスクは、UIやSteam統合より先に以下の最小経路を完成させる。

```text
Moonlight接続要求
  → client width/height/fps取得
  → headless Gamescope起動
  → PipeWire video node発見
  → Sunshineでキャプチャ
  → VAAPI H.264エンコード
  → test application表示
  → disconnect
  → 全プロセス／ノード破棄
```

固定条件:

- 1920x1080
- 60 FPS
- SDR
- stereo audioは後回しでも可
- 単一クライアント
- AMD GPU

この縦切りが安定するまで、HDR、複数セッション、UI大改修、通常モニター型の仮想DRM実装へ進まない。

## 17. 実装判断基準

### 継続条件

- Gamescope headless出力を安定してPipeWire取得できる。
- DMA-BUFまたは実用的な低コピー経路が成立する。
- SteamOS更新に依存しすぎない起動方式を確立できる。
- 切断後の完全クリーンアップを保証できる。

### 方式再検討条件

- SteamOS付属Gamescopeが必要なheadless／PipeWire機能を提供しない。
- GamescopeからのPipeWire出力が実用フレームレートに達しない。
- Steamを専用セッションへ分離できない。
- SteamOS更新ごとに大規模なバイナリ置換が必要になる。

再検討候補:

1. 既存Gaming Modeの動的解像度変更
2. KWin virtual output
3. AMDGPU virtual display
4. VKMS
5. 強制EDID／未使用コネクター
6. ダミープラグを自動設定する互換モード

## 18. 完成定義

SteamShineのSteamOS仮想セッション機能は、以下を満たした時点で初期完成とする。

- 物理モニターまたはダミープラグなしで接続できる。
- Moonlight／Artemisの解像度とFPSへ自動一致する。
- Steam Big PictureとProtonゲームを仮想セッションへ起動できる。
- AMDハードウェアエンコードが使用される。
- 入力とステレオ音声が正常に動く。
- 切断、通信断、ゲームクラッシュ後に自動復旧する。
- SteamOS再起動後に手動操作なしで再接続できる。
- 仮想機能を無効化すれば通常のSunshineとして動作する。
- 50回の接続／切断試験で孤立プロセスと残留音声ノードがない。

---

この計画では、SteamShineをSunshine互換のまま維持しつつ、SteamOSに特化したGamescope仮想セッション管理層を追加する。最初からOSレベルの仮想モニター完全互換を狙わず、既存のGamescope・PipeWire・Sunshine資産を組み合わせて、Apolloに近い実用的な接続体験を段階的に実現する。
