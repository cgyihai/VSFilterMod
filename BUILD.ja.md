# VSFilterMod のビルド

[English](BUILD.md) | [简体中文](BUILD.zh-CN.md) | **日本語** | [한국어](BUILD.ko.md)

このプロジェクトのビルドでは 2 つの成果物ができます：

| 成果物 | 説明 |
| --- | --- |
| `vsfiltermod.dll` / `.so` / `.dylib` | **CSRI** 字幕レンダリングプラグイン。Aegisub はすべてのプラットフォームでこの形式で読み込みます。**AviSynth+** プラグイン（`TextSubMod` / `MaskSubMod`）と **VapourSynth** プラグイン（`vsfm.TextSubMod`）でもあります。 |
| `vsfmrender` | コマンドラインツール。字幕を **BDN+XML**（インデックスファイルと、イベントごとの PNG）にレンダリングします。 |

両者は同じレンダラーソースを使うため、BDN+XML にエクスポートされた字幕は
Aegisub プレビューとピクセル単位で同じです。

## 環境要件

* Meson ≥ 0.59 と Ninja
* C++17 対応コンパイラ（GCC 9+、Clang 10+、MSVC 2019+）
* FreeType ≥ 2.8 と libpng ≥ 1.6：システムにあればそれを使用、なければ
  Meson サブプロジェクトとして自動ダウンロード
* Linux/BSD のみ：**fontconfig** ≥ 2.11
* macOS と Windows はそれぞれ CoreText と DirectWrite を使用。追加インストール不要

## ビルド

```bash
meson setup build
ninja -C build
meson test -C build
```

よく使うオプション：

```bash
meson setup build -Dfont_backend=fontconfig   # プラットフォームデフォルトのフォントバックエンドを上書き
meson setup build -Dbdnxml=false              # プラグインのみビルド
meson setup build -Dtests=false
```

### すべてのプラットフォームで出力が一致

レンダリングコードは純粋な可搬 C++ です：SIMD 対応、CPU ベンダー、
コンパイラに依存するコードパスはありません。ビルド時に `-ffp-contract=off`
が渡され、GCC と Clang が `a * b + c` を FMA 命令にまとめるのを禁止します
（すべての AArch64 CPU は FMA を持ち、その丸め方が異なります）。
`-ffast-math`、`/fp:fast`、`-ffp-contract=fast` でコンパイルしないでください。
`render-consistency` テストが失敗します。

`meson test` にはこのテストが含まれます：内蔵の Caladea フォントを
`tests/consistency/*.ass` に埋め込んでレンダリングし（インストール済み
フォントは一切関係しません）、各イベントのピクセルが
`tests/consistency/expected.txt` と完全に一致することを要求します——すべての
プラットフォームで同じファイルを使います。意図的にレンダリング結果を変えた
場合は、このファイルを再生成して変更と一緒にコミットしてください：

```bash
python3 tests/consistency/run.py --renderer build/src/bdnxml/vsfmrender --out build/consistency --update
```

## Aegisub へのインストール

### Linux / macOS

Aegisub 3.5.6 はこれら 2 つのプラットフォームでデフォルトでは CSRI 対応を
コンパイルしません。2 つの障害があり、`docs/aegisub-csri-posix.patch` が
両方を修正しています：

* `meson.build` は `host_machine.system() == 'windows'` のときだけ
  この機能を有効化する
* `subprojects/csri/meson.build` は `CSRI_PATH` を一度も定義していないため、
  POSIX 列挙子（`lib/posix/enumerate.c`）がコンパイルできない——その最初の
  文が `static const char csri_path[] = CSRI_PATH;`

```bash
cd /path/to/Aegisub
git apply /path/to/VSFilterMod/docs/aegisub-csri-posix.patch
meson setup build && ninja -C build
```

次にプラグインを検索パス内の任意の場所に置きます。コンパイル時に組み込まれた
デフォルトパスは `~/.csri` と `<prefix>/<libdir>/csri` で、環境変数
`CSRI_PATH` がその後ろに追加されます：

```bash
mkdir -p ~/.csri
cp build/vsfiltermod.so ~/.csri/        # macOS では .dylib
chmod +x ~/.csri/vsfiltermod.so         # 下記を参照
```

> POSIX 列挙子は `access(path, X_OK)` チェックに失敗したファイルをスキップするため、
> プラグインは**必ず**実行権限を持つ必要があります。
> `meson install` は自動で設定します。手動の `cp` では設定されないことがあります。

### Windows

Aegisub の Windows 列挙子はプログラムの隣の `csri` フォルダでプラグインを
探します：

```
Aegisub\
  aegisub.exe
  csri\
    vsfiltermod.dll
```

### プラグインの選択

Aegisub を再起動し、*オプション → 詳細 → 動画 → 字幕レンダラー* で
**CSRI/vsfiltermod_textsub** を選択します。リストにない場合、プラグインが
見つからないか読み込みに失敗しています。Linux と macOS では端末から
Aegisub を起動して、ローダーの警告メッセージを確認してください。

## 継続的インテグレーション

`.github/workflows/ci.yml` は 8 つの構成をビルドしてテストします：

| ジョブ | 実行環境 | ツールチェーン | フォントバックエンド |
| --- | --- | --- | --- |
| Linux x86_64 | `ubuntu-22.04` | GCC | fontconfig |
| Linux aarch64 | `ubuntu-22.04-arm` | GCC | fontconfig |
| macOS arm64 | `macos-15` | Clang | CoreText |
| macOS x86_64 | `macos-15-intel` | Clang | CoreText |
| Windows x86_64 | `windows-latest` | MSVC | DirectWrite |
| Windows x86 | `windows-latest` | MSVC（x64 → x86） | DirectWrite |
| Windows ARM64 | `windows-11-arm` | MSVC | DirectWrite |
| Windows x86_64（MinGW） | `windows-latest` | GCC、MSYS2 パッケージ | DirectWrite |

MinGW 以外はすべて固定バージョンのサブプロジェクトから FreeType と libpng を
ビルドして静的リンクし（Linux では `--exclude-libs` でプライベートリンク）、
各リリースバイナリが同じ FreeType を使うようにしています。`macos-universal`
ジョブは `lipo` で 2 つの macOS バージョンを統合します。`sanitizers` ジョブは
AddressSanitizer と UndefinedBehaviorSanitizer の下でテストとファジングを
実行します。

すべてのジョブが `csri-abi` テスト（Aegisub ローダーの方式でプラグインを
読み込んでレンダリング）と `render-consistency` テスト（このプラットフォームの
`tests/consistency/*.ass` のレンダリングが `expected.txt` とピクセル単位で一致
することを要求）を実行し、`tests/smoke.ass` を BDN+XML にレンダリングして
`tests/check_bdnxml.py` で結果を検証します。検証器は完全に透明な画像を拒否
するため、あるプラットフォームのフォント検索が静かに失敗した場合、空白字幕を
出力する代わりにビルドがエラーになります。

最後の `consistency` ジョブはすべてのプラットフォームの結果を集約し、どの
プラットフォームが一致し、どのイベントで不一致かを一覧します。

`vapoursynth` ジョブは Linux（x86_64 と aarch64）、macOS（arm64 と x86_64）、
Windows x64 で PyPI から VapourSynth をインストールし、プラグインを読み込んで
`vapoursynth` テストを実行します。各プラットフォームの出力は
`tests/vsplugin/expected.txt` と一致しなければなりません。他の環境で
`vapoursynth` Python モジュールがインストールされていなければ、このテストは
スキップされます。

2 点の注意：

* **ARM Linux ビルドマシンは公開リポジトリのみ無料です。** リポジトリが
  プライベートな場合は `ubuntu-24.04-arm` の項目を削除してください。さもないと
  そのジョブはエラーになる代わりに永遠にキューイングされ続けます。
* **配布できるのは MSVC ビルド成果物です**：freetype、libpng、zlib、C ランタイムは
  すべて静的リンクで、`vsfiltermod.dll` は単一ファイルで `DWrite.dll`、
  `KERNEL32.dll`、`USER32.dll`、`ADVAPI32.dll` のみに依存します。MinGW 成果物は
  MSYS2 の DLL に依存し、コンパイル可能かの確認にのみ使います。

### リリース

`v` で始まるタグをプッシュすると、同じビルドマトリクスが実行され、その後
GitHub Release が公開されます。各プラットフォームに 1 つの圧縮パッケージ
（プラグイン、`vsfmrender`、ライセンスと README）が付きます：

```bash
git tag v1.0.0
git push origin v1.0.0
```

MinGW バージョンは公開されません。コンパイル可能かの確認にのみ使います。

## Aegisub の外でプラグインを検証する

`tests/csri_loader_test.c` は CSRI ローダーとまったく同じ方式で動的ライブラリを
読み込み（パスで読み込み、名前で各エントリ関数を解決）、メモリから字幕を
パースして Aegisub が使うピクセルフォーマットを要求し、1 フレームを
レンダリングします。Meson テストとして登録済みです：

```bash
meson test -C build csri-abi
```

## BDN+XML のレンダリング

```bash
build/vsfmrender --size 1920x1080 --fps 23.976 input.ass output.xml
```

同じディレクトリに `output.xml` と `output_NNNN.png` が書き出されます。

イベントリストは avs2bdnxml がオリジナルの `MaskSubMod` から得るものと一致
します：`(REFERENCE_TIME)(10000000 * n / fps)` でフレームをサンプリングし、
オリジナルと同じく単精度で計算します。隣接する同一フレームは統合され、空の
フレームがイベントを終了させます。ピクセルには `MaskSubMod` の逆プレマルチ
プライ式を使います。avs2bdnxml-rs + オリジナル VSFilterMod と `bdn-diff` で
照合済み：`tests/smoke.ass` の各イベントの InTC と OutTC はすべて同じです。

`--fps` は `分子/分母` または小数を受け付けます。23.976、29.97、59.94 は
対応する `/1001` 分数を意味し、これが AviSynth が `MaskSub` に渡すクリップの
フレームレートです。スクリプトが別のフレームレートを使う場合は、正確な分数を
渡してください。各フレームは `MaskSubMod` と同じくそれぞれの時刻で個別に
レンダリングされます：その字幕ソースはすべての行をアニメーションとして
報告します。

`vsfmrender` が**行わない**、avs2bdnxml が行うこと：パレット（8 ビット PNG）
出力、1 つのイベントを 2 枚の画像に分割、PGS `.sup` 出力、Y 座標の偶数強制。
各イベントに 1 枚の RGBA 画像を書きます。

すべてのオプションは `vsfmrender --help` を実行してください。

## AviSynth+

同じライブラリは AviSynth+ C プラグインでもあります：

```
LoadPlugin("vsfiltermod.dll")
MaskSubMod("subs.ass", width=1920, height=1080, fps=24000.0/1001.0, length=34000)
# または動画にオーバーレイ：
TextSubMod(clip, "subs.ass", charset=1, fps=-1, vfr="timecodes.txt")
```

`TextSub`、`MaskSub`、`TextSubModSwapUV` も登録済みで、引数はオリジナルと同じ
です。ビデオクリップは RGB32、RGB24、YUY2、YV12、I420 が使えます。YUV 出力は
オリジナルと同じく、字幕の `YCbCr Matrix` ヘッダーが指定するマトリクスを使います。

レンダリングフローはオリジナルの `CFilter::Render` に段階的に対応します——
単精度タイムスタンプ、メモリ字幕レイヤーで前回のダーティ矩形を消去、実際に
レンダリングされたバウンディングボックスのみ合成、RGB32 と RGB24 は上下反転で
`MaskSubMod` の RGBA は反転しない——これらは avs2bdnxml がどのピクセルを得るかを
決定する要因だからです。avs2bdnxml-rs と `tests/avs_dump.c` でオリジナルと照合
済み：イベント時間は同じ。`MaskSubMod` の RGBA、および `TextSubMod` の RGB32、
RGB24、YUY2、YV12 での字幕行は同じ（I420 と YV12 はコードパスを共有、個別には
未テスト）。VFR のフレーム選択も同じ。

AviSynth はリンク時の依存ではありません。プラグインは読み込み時に、それを
読み込んだ AviSynth モジュールから C API を解決するため、AviSynth が
インストールされていないマシンでも、同じファイルが CSRI プラグインとして
読み込めます。`-Davisynth=false` でコンパイルするとこれらのエントリ関数を
除けます。

## VapourSynth

同じライブラリは VapourSynth プラグインでもあります。API 4（VapourSynth R55
以降）ベースで、識別子、名前空間、引数はオリジナルと同じです：

```python
clip = core.vsfm.TextSubMod(clip, "subs.ass", charset=1, fps=23.976, vfr="timecodes.txt")
```

`file` と `vfr` は UTF-8 パスです（Windows ではオリジナルと同じく ANSI パスも
受け付けます）。`fps` のデフォルトはクリップのフレームレート。可変フレーム
レートのクリップでは `fps` または `vfr` を指定する必要があります。

| クリップ | レンダリング方式 |
| --- | --- |
| YUV420P8 | オリジナルと同じ：YV12 字幕レイヤーの変換と混合。 |
| RGB24 | オリジナルと同じ：オリジナルがフレームコピー上で行う RGB32 パック混合。 |
| 4:2:0 と 4:2:2 YUV、9–16 ビット | 同じアルゴリズム、完全なサンプルに作用。 |
| YUV422P8、グレースケール、8 ビット超 RGB | 同じアルゴリズム、クリップ自身のビット深度で計算。 |

オリジナルが YUV420P10 と P16 をサポートした方式は、各サンプルを 2 つの 8 ビット
半分に分割し、上位半分にのみ描画するものでした。`accurate=1` では両半分を描き、
色が誤りになります。こちらでは `accurate` は渡せますが無視されます。
8 ビットで正確な背景では、高ビット深度の輝度を 8 ビットに右シフトしたものは
8 ビット出力と完全に同じです。`tests/vsplugin/run.py` は `vapoursynth` テストで
これをチェックし、出力ダイジェストを比較します。
4:4:4 YUV は拒否されます。字幕の彩度はピクセルペアで計算されるためです。
RGB に変換してください。浮動小数点クリップも同様に非対応です。

`-Dvapoursynth=false` でコンパイルするとこのエントリ関数を除けます。

## オリジナルとの比較

`tests/csri_compare.c` は任意の CSRI プラグインで字幕をレンダリングして生の
フレームをエクスポートします。`tests/compare_raw.py` は `MaskSubMod` の式で
2 つのエクスポート結果を変換し、カバレッジ領域の IoU、透明度の差異を報告し、
並べた比較画像を生成します。1 つ目には Aegisub の `csri/VSFilterMod.dll` を、
2 つ目にはこのプロジェクトのビルド成果物を指定してください。オリジナルに渡す
字幕は**必ず UTF-8 BOM 付き**にしてください：CSRI で BOM なしのファイルを読むと、
オリジナルは未処理の C++ 例外を投げます。

`tests/frs.ass` と `tests/vertical.ass`（毎秒 1 行、`n.5` 秒でサンプリング）は、
エンジンが GDI の非公開組版規則を再現した 2 つの箇所をカバーします：各角度の
`\frs`（CJK 文字、`\fsp`、ボールド、イタリック、スケールと改行を含む）と、
`@` 縦書きフォント（句読点、拼音、ギリシャ文字、全角文字を含む）。これらは
宋体、黒体、Microsoft YaHei、DengXian、MS Gothic、Yu Gothic、Malgun Gothic、
Times New Roman を使います。これらのフォントがインストールされたマシンで
実行してください。オリジナルとの比較 IoU はそれぞれ 0.977–0.999 と
0.967–1.000 です。

`tests/fallback.ass` はグリフフォールバックをカバーします：Arial、
Times New Roman、Tahoma、Segoe UI の CJK・ハングル文字、イタリック、下線・
取り消し線、`\fsp`、`\frz`、`\frs`、SRT 風デフォルトスタイルを含みます。
中国語版 Windows 11 でのオリジナルとの比較で、全 11 行の IoU は
0.985–0.999（グリフフォールバック追加前は 0.12–0.67）。`tests/bold.ass` は
黒体、宋体、楷体の合成ボールドをカバーし、イタリックと `\frs`、および
`\frs` 下のボールドフォールバックグリフを含みます：0.977–1.000
（以前は 0.93–0.95）。

## 削除された内容

このプロジェクトは VSFilterMod の縮小版です。DirectShow 再生専用の部分は
Aegisub も BDN+XML 出力も使わないため、すべて削除されました：DirectShow
フィルタとそのプロパティページ、VobSub/DVB/HDMV/CC/USF/SSF/RealText
パーサー、DirectX 字幕レイヤーバックエンド、`VobSub()`、および
VapourSynth プラグイン（旧 API）。

残されているのは本当に重要な部分です：完全な VSFilterMod タグセット
（拡張タグ `\pngimg` 画像背景、グラデーション、移動可能なベクター clip、
ジッター、distort、文字回転、Z 座標、縦方向スペーシングを含む）。
