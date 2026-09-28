# PopNES のライセンス

PopNES は、ファミコン / NES のエミュレータ [QuickNES](https://github.com/libretro/QuickNES_Core)(Shay Green 氏(blargg)作、libretro 版)を、SHARP Brain PW-G5200(Windows CE)向けに移植した**非公式**の改変版です。QuickNES の作者や libretro のメンテナーはこの移植に関わっていません。

ライセンスは2段になっています。

## 1. アプリ全体: GPL-2.0

アプリ全体(`AppMain.exe`)は、**GNU General Public License, version 2(GPL-2.0)** で配布します。本文はリポジトリ直下の [`LICENSE`](../LICENSE) にあります。上流の libretro/QuickNES_Core が GPL-2.0 で、`AppMain.exe` はそれを組み込んでいるためです。

- 販売や商用の利用を禁じる条件はありません
- `AppMain.exe` を配布するときは、対応するソース(このリポジトリ)も入手できるようにしてください
- 改変したものを配るときも、GPL-2.0 で、ソースを付けて配ってください
- 著作権表示とライセンスの文を残してください。保証はありません

この条件は GPL-2.0 そのものが認めているので、作者への個別の確認は必要ありません。

## 2. QuickNES 本体(上流のファイル)

リポジトリ直下のファイル(`CE/` と `.github/README.md` 以外)は、上流の [libretro/QuickNES_Core](https://github.com/libretro/QuickNES_Core) のコミット `26bb785`(2026-07-06)そのままです。

**PopNES は上流のファイルを変えていません。** 変えたのは `.gitignore` の末尾に PopNES 用の除外ルールを足したことだけです。この端末に合わせるための設定(たとえば、そろっていない場所のメモリを読まないようにする `-DNO_UNALIGNED_ACCESS`)は、すべて `CE/Makefile` の中で指定しています。

上流のソースの著作権表示は、変えずに残しています。

## 3. PopNES の自作部分: MIT

PopNES の自作部分は、MIT ライセンスです。本文は [`LICENSE`](LICENSE) にあります。対象は、先頭に `SPDX-License-Identifier: MIT` と書いてある次のファイルです。

- `CE/` のフロントエンド(`ce_*.c`、`ce_*.h`、`ce_res.rc`、`Makefile`、`compat/`)。ただし次のものは除きます
  - フォントのデータ `ce_shinonome16.h`、`ce_galmuri14.h`、`ce_galmuri11.h`(下の「第三者のもの」を参照)
  - マスコットの画像(下の「マスコットの絵」を参照)

自作部分だけを取り出して、ほかのプロジェクトで MIT として使うことができます。QuickNES と組み合わせた `AppMain.exe` を配布する場合は、1 の GPL-2.0 の条件を守る必要があります。

## 4. マスコットの絵: CC0 1.0

メニューのマスコットの絵(`CE/popnes_mascot.bmp`)は、AI(Claude)で作ったもので、[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)(パブリックドメイン)です。

## 5. 第三者のもの

`AppMain.exe` に入っている第三者のものは次のとおりです。著作権表示と許諾文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

| もの | 作者 | ライセンス |
|---|---|---|
| QuickNES 本体(`nes_emu/`: Nes_Emu、Nes_Snd_Emu、Blip_Buffer、nes_ntsc など) | Shay Green 氏(blargg) | LGPL-2.1 以降 |
| `nes_emu/abstract_file.cpp` | Shay Green 氏 | MIT |
| emu2413(VRC7 の FM 音源) | Mitsutaka Okazaki 氏(VRC7 向けの改変は xodnizel 氏) | zlib に近いゆるいライセンス |
| libretro の接続部分(`libretro/libretro.cpp` など) | libretro のメンテナー | GPL-2.0 |
| libretro API のヘッダ、libretro-common の一部 | The RetroArch team | MIT |
| Galmuri フォント(メニューの既定の文字) | Lee Minseo 氏 | SIL Open Font License 1.1 |
| 東雲 16 ドットフォント | 古川泰之 氏ほか、/efont/ | 実質パブリックドメイン |

どれも GPL-2.0 と一緒に配布できるライセンスです。

MMC5(マッパー5)の処理は、FCEUmm(FCEUX プロジェクトの一部、GPL-2.0)の実装を参考にして上流で書かれたものです。

## 6. 同梱していないもの

- ゲームの ROM。使う人が合法的に用意してください

## 7. 上流のファイルについての注意

- `jni/`、`Makefile`、`Makefile.common`、`link.T`、`intl/`、`.github/workflows/`、`.gitlab-ci.yml`、`.travis.yml`、`nes_emu/tools/` は上流のファイルです。PopNES のビルドには使っていません(`AppMain.exe` には入っていません)
- 上流には `README.md` がありません。PopNES の説明は [`.github/README.md`](../.github/README.md) と [`CE/README.md`](README.md) にあります
