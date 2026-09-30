# PopNES のライセンス

PopNES は、ファミコン / NES のエミュレータ [QuickNES](https://github.com/libretro/QuickNES_Core)(Shay Green 氏(blargg)作、libretro 版)を、SHARP Brain PW-G5200(Windows CE)向けに移植した**非公式**の改変版です。QuickNES の作者や libretro のメンテナーは、この移植には関わっていません。

## 1. アプリ全体: 上流と同じ条件(GPL-2.0)

アプリ全体(`AppMain.exe`、およびこのリポジトリで公開しているソース全体)は、上流の libretro/QuickNES_Core と同じ条件、つまり **GNU General Public License, version 2(GPL-2.0)** で配布します。本文はリポジトリ直下の [`LICENSE`](../LICENSE) にあります。

- 販売や商用の利用を禁じる条件はありません
- `AppMain.exe` を配布するときは、対応するソース(このリポジトリ)も入手できるようにしてください
- 改変したものを配るときも、GPL-2.0 で、ソースを付けて配ってください
- 著作権表示とライセンスの文を残してください。保証はありません

この条件は GPL-2.0 そのものが認めているので、作者への個別の確認はしていません。

`AppMain.exe` に入るファイルには、先頭に書かれている表記によって次のものがあります。

- 多くのファイル(`nes_emu/` の `.cpp` と一部のマッパー)は、Shay Green 氏の著作権表示と GNU LGPL バージョン 2.1 以降の表記があります。LGPL-2.1 の第3条により、GPL バージョン2 以降の条件で配布することができます
- マッパー 021、022、023、025 は、Shay Green 氏と CaH4e3 氏の著作権表示と、GPL バージョン2 以降の表記があります
- `nes_emu/abstract_file.cpp` と libretro API のヘッダは MIT、emu2413 は作者の許諾文(商用を含めて自由に使えるもの)、Galmuri フォントは SIL Open Font License 1.1、東雲フォントは実質パブリックドメインです
- 著作権表示に作者の名前が書かれていないファイル(マッパー 015、060、075 と、マッパー 030 などの 25 個)と、ライセンスの表記がないファイル(`libretro/libretro.cpp`、`nes_emu/` のヘッダの多く、マッパー 009、010、026、078、156、190 など)は、**上流リポジトリの `LICENSE`(GPL-2.0)に従います**。ファイルに書かれている表記は、変えずに残しています

ファイルごとの一覧と、それぞれの著作権表示・許諾文の本文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

## 2. QuickNES 本体(上流のファイル)

リポジトリ直下のファイル(`CE/` と `.github/README.md` を除く)は、上流の [libretro/QuickNES_Core](https://github.com/libretro/QuickNES_Core) のコミット `26bb785`(2026-07-06)を元にしています。

PopNES は、上流のファイルを変えていません。上流のファイルのうち手を加えたのは `.gitignore` で、末尾に PopNES 用の除外ルールを書き足しました。この端末に合わせるための設定(たとえば、4の倍数でない番地から4バイトを読まないようにする `-DNO_UNALIGNED_ACCESS`)は、すべて `CE/Makefile` の中で指定しています。

上流のソースの著作権表示は、変えずに残しています。

## 3. PopNES の自作部分: MIT

PopNES の自作部分は、MIT ライセンスです。本文は [`LICENSE`](LICENSE) にあります。対象は、先頭に `SPDX-License-Identifier: MIT` と書いてある次のファイルです。

- `CE/` のフロントエンド(`ce_*.c`、`ce_*.h`、`ce_res.rc`、`Makefile`、`compat/`)。ただし次のものは除きます
  - フォントのデータ `ce_shinonome16.h`、`ce_galmuri14.h`、`ce_galmuri11.h`(下の「第三者のもの」を参照)
  - `CE/icon/` の画像(下の「マスコットの絵とアイコン」を参照)

自作部分を取り出して、ほかのプロジェクトで MIT として使うことができます。QuickNES と組み合わせた `AppMain.exe` を配布する場合は、1 の GPL-2.0 の条件を守ってください。

## 4. マスコットの絵とアイコン: CC0 1.0

メニューのマスコットの絵(`CE/icon/popnes_mascot.bmp`)、アプリのアイコン(`CE/icon/popnes.ico`、`AppMain.exe` に入っているもの)、README の先頭の絵(`.github/images/popnes_mascot_A_ohirune_4x.png`)は、[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)(パブリックドメイン)です。Pop シリーズのマスコットをもとに、AI(Claude)で作りました。

## 5. 第三者のもの

`AppMain.exe` に入っている第三者のものは次のとおりです。著作権表示と許諾文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

| もの | 作者 | ライセンス |
|---|---|---|
| QuickNES 本体(`nes_emu/`: Nes_Emu、Nes_Snd_Emu、Blip_Buffer、nes_ntsc など) | Shay Green 氏(blargg) | LGPL-2.1 以降 |
| マッパー 021、022、023、025 | Shay Green 氏、CaH4e3 氏 | GPL-2.0 以降 |
| `nes_emu/abstract_file.cpp` | Shay Green 氏 | MIT |
| 作者の名前が書かれていないファイル、ライセンスの表記がないファイル(`libretro/libretro.cpp` などの libretro の接続部分を含みます) | 上流リポジトリ(libretro/QuickNES_Core) | 上流リポジトリの `LICENSE`(GPL-2.0)に従います |
| emu2413(YM2413 / VRC7 の FM 音源、`nes_emu/emu2413.cpp`) | Mitsutaka Okazaki 氏(VRC7 向けの改変は xodnizel 氏) | 作者の許諾文(商用を含めて自由に使え、出所を偽らなければ改変・再配布できるもの) |
| libretro API のヘッダ、libretro-common の一部 | The RetroArch team | MIT |
| Galmuri フォント(メニューの既定の文字) | Lee Minseo 氏 | SIL Open Font License 1.1 |
| 東雲 16 ドットフォント | 古川泰之 氏ほか、/efont/ | 実質パブリックドメイン |

どれも GPL-2.0 のプログラムの一部として配布できるライセンスです。

MMC5(マッパー5)の拡張属性(ExGrafix)の読み出しは、上流のコメントに、FCEUmm(FCEUX プロジェクトの一部、GPL-2.0)の実装にならったと書かれています。

## 6. 同梱していないもの

- ゲームの ROM。使う人が合法的に用意してください

## 7. 上流のファイルについての注意

- `jni/`、`Makefile`、`Makefile.common`、`link.T`、`intl/`、`.github/workflows/`、`.gitlab-ci.yml`、`.travis.yml`、`nes_emu/tools/` は上流のファイルです。PopNES のビルドには使っていません(`AppMain.exe` には入っていません)。ビルドに使うファイルは `CE/Makefile` で決まります
- 上流には `README.md` がありません。PopNES の説明は [`.github/README.md`](../.github/README.md) と [`CE/README.md`](README.md) にあります
