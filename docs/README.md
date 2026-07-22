# SteamShine 設計文書

SteamOS向けSteamShineの設計文書一覧。

## 優先順位と性能方針

- [`STREAMING_PRIORITY_AND_PERFORMANCE_POLICY.md`](./STREAMING_PRIORITY_AND_PERFORMANCE_POLICY.md)
  - Artemis／Moonlightと仮想Gamescope配信を最優先にする拘束仕様
  - Web管理からアクセスできる全情報
  - SSD書き込み、CPU／GPU負荷、監視間隔、性能予算
  - Webブラウザゲーム配信を任意・既定無効とする方針

## 仮想デスクトップ配信

- [`STEAMOS_VIRTUAL_SESSION_PLAN.md`](./STEAMOS_VIRTUAL_SESSION_PLAN.md)
  - headless Gamescope、PipeWire、DMA-BUFを使った仮想ストリーミングセッション
  - 解像度／FPS自動一致、Steam統合、音声、入力、復旧

## GPU・監視・遠隔管理

- [`STEAMOS_CONTROL_PLANE_DESIGN.md`](./STEAMOS_CONTROL_PLANE_DESIGN.md)
  - AMD GPU設定変更
  - リソースモニター
  - SSH／永続Webターミナル
  - RDP／VNC／SSHリモートデスクトップ
  - ControlDeckからの移植設計

## 実装時の判断順

1. `STREAMING_PRIORITY_AND_PERFORMANCE_POLICY.md`のP0性能要件を守る。
2. `STEAMOS_VIRTUAL_SESSION_PLAN.md`のArtemis／Moonlight経路を先に完成させる。
3. `STEAMOS_CONTROL_PLANE_DESIGN.md`の管理機能を、配信データプレーンから分離して追加する。
4. Webブラウザからのゲームプレイは、P0／P1完成後の任意評価とする。
