# PopNES — SHARP Brain (Windows CE) 移植版

## 概要

- ファミコン / NES のエミュレータ [QuickNES](https://github.com/libretro/QuickNES_Core) の libretro 版を、SHARP の電子辞書 **Brain PW-G5200**(Windows CE / ARMv5TE)向けに移植したものです
- `libretro/libretro.cpp` とエミュレータ本体(`nes_emu/`)をそのままコンパイルし、Win32 のフロントエンド(`CE/` 以下)を新しく書いて繋いでいます。**上流のファイルは変えていません**
- QuickNES は C++ で書かれていますが、この端末には `libstdc++-6.dll` が無いので、C++ の実行時ライブラリを `AppMain.exe` に静的に組み込んでいます。依存する DLL は `COREDLL.dll` だけです
- ゲームの ROM は同梱していません。利用者が合法的に用意したものを使ってください
- SHARP・任天堂とは関係のない、非公式のファンプロジェクトです
- QuickNES の公式版ではありません。QuickNES の作者や libretro のメンテナーはこの移植に関わっていないので、不具合はこちらに報告してください

### 主な機能

- 4 つの表示倍率(x1 / x1.5 / Wide / Full)、GDI で直接描画
- 日本語 / 英語の UI(Galmuri14 と東雲 16 ドットのビットマップフォントを `AppMain.exe` に内蔵)
- キーの割り当て(斜め入力、連射 A / 連射 B を含む)
- ステートセーブ / ロード(セーブ前に確認)、SRAM の自動セーブ
- 画面の保存(スクリーンショット。`AppMain.exe` と同じフォルダの `Screenshots` に BMP で保存)
- 日本語のファイル名・フォルダ名に対応した ROM 選択画面(前回開いたフォルダから始まります)
- Video Config: スプライト数の制限をなくす / フレームスキップ(0〜30、自動判定あり)/ UI の言語 / デバッグログ
- Sound Config: 音量 / レート / ビット数 / 音質 / バッファの大きさ

`.nes` の ROM に対応しています。`.zip` に圧縮した ROM には対応していません。

## ダウンロード

ビルド済みの `AppMain.exe` は [Releases](../../../releases) に置きます。

## ビルド方法

必要なもの:

- WSL(Windows 上の Linux)などに入れた cegcc のクロスコンパイラ(`/opt/cegcc/bin/arm-mingw32ce-*`。C と C++ の両方を使います)

```sh
git clone https://github.com/<このリポジトリ>.git
cd <リポジトリ>/CE
make clean && make && make strip
```

- できあがるのは `CE/AppMain.exe` です(依存する DLL は `COREDLL.dll` だけ)
- サブモジュールは使っていないので、`--recursive` は要りません
- 既定のフォントは Galmuri14 です。`make CE_FONT=shinonome` で東雲 16 ドット版(`AppMain_shinonome.exe`)、`make CE_FONT=galmuri11` で GalmuriMono11 版(`AppMain_galmuri11.exe`)も作れます
- 設計の理由やつまずいた点は、`CE/` の各ソース(特に `Makefile`、`ce_main.c`、`ce_display.c`、`ce_audio.c`)の冒頭のコメントにあります

## 使用方法

SHARP Brain(PW-G5200 など)を PC にリムーバブルディスクとしてつなぎ、ドライブの直下に次のように置きます(メニューの名前は機種によって違うことがあります)。

```
<ドライブ直下>/
  アプリ/
    <好きなアプリ名>/
      AppMain.exe    ← ビルドしたもの
      index.din      ← 中身は空でよいファイル
```

- `index.din` を置くと、そのフォルダが [追加アプリ・動画] の一覧に出ます
- ROM(`.nes`)は SD カードに置き、アプリのメニューの「ROMを開く」から選びます
- セーブ(`.srm`)とステート(`.state`)は、ROM と同じフォルダに作られます
- 設定ファイル `popnes.cfg` は、`AppMain.exe` と同じフォルダに作られます
- `popnes_debug.log` は、Video Config で「デバッグログを有効にする」をオンにしたときだけ作られます(既定はオフ)

## 動作確認環境

- SHARP Brain PW-G5200

PopGBA を CeOpener や CERestorer から起動すると、PW-G5300 で、終了後に画面が真っ暗になる、または操作を受け付けなくなることがありました。USB ケーブルと電池を抜いてから入れ直すと戻りました。PopNES での動作は確かめていません。

## クレジット

- **QuickNES** — エミュレーションコア。元になった **Nes_Emu** / **Nes_Snd_Emu** / **Blip_Buffer** / **nes_ntsc** は **Shay Green**(blargg)氏の作。LGPL-2.1 以降
- **libretro / QuickNES_Core** — QuickNES の libretro 版(`libretro/`)。libretro のメンテナーの皆さん。GPL-2.0
  <https://github.com/libretro/QuickNES_Core>
- **emu2413**(YM2413 / VRC7 の FM 音源)— **Mitsutaka Okazaki** 氏。VRC7 向けの改変は **xodnizel** 氏
- **MMC5(マッパー5)** の拡張属性の読み出しは、上流のコメントによると **FCEUmm**(GPL-2.0)の実装にならったものです
- **libretro API** のヘッダ — **The RetroArch team**。MIT
- **Galmuri** ビットマップフォント(メニューの既定の文字)— **Lee Minseo**(quiple)氏。SIL Open Font License 1.1
  <https://github.com/quiple/galmuri>
- **東雲(しののめ)16 ドットビットマップフォント** — メインデザイン **古川 泰之** 氏ほか、**The Electronic Font Open Laboratory(/efont/)**。実質パブリックドメイン
  <https://github.com/code4fukui/shinonome-font>
- **CeGCC** — Windows CE / ARM 向けクロスコンパイラ。**Danny Backx** 氏ほか、モダン版の **Max Kellermann** 氏
- **SHARP Brain homebrew コミュニティ** — 端末の情報を残してくださった皆さん
- Windows CE フロントエンド(`CE/`)は本プロジェクトで作成

コンポーネントごとの出所とライセンスの詳細は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) と [`LICENSING.md`](LICENSING.md) をご覧ください。

## 制作について

コードとマスコットの絵はAI(Claude)で作りました。製作者はプログラムを読めません。

マスコットの絵とアイコンは、Pop シリーズのマスコットをもとに作ったもので、CC0 1.0(パブリックドメイン)です。各ライセンスについては、[`LICENSING.md`](LICENSING.md) をご覧ください。

## ライセンス

PopNES 全体(`AppMain.exe`)は、上流と同じ条件、つまり **GNU General Public License, version 2(GPL-2.0)** で配布します(リポジトリ直下の [`LICENSE`](../LICENSE))。

- 改変したものを配るときは、GPL-2.0 で、ソースを付けてください
- 著作権表示とライセンスの文を残してください
- 保証はありません

PopNES の自作部分(`SPDX-License-Identifier: MIT` と書いてあるファイル)は MIT です([`LICENSE`](LICENSE))。バイナリを配るときは、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) も一緒に配ってください。

## 商標・免責

PopNES は非公式のファンプロジェクトです。シャープ株式会社、任天堂株式会社とは関係がなく、許諾・後援も受けていません。

- 「SHARP」「Brain」はシャープ株式会社の商標です
- 「任天堂」「ファミリーコンピュータ」「ファミコン」「Nintendo Entertainment System」「NES」は任天堂株式会社の商標です

ゲームの ROM は含みません。利用者が合法的に入手したものを用意してください。
