<div align="center">
  <h1>Kinu Tune</h1>
  <p>Cubase VariAudio 風の操作で使える AI ボーカルピッチエディター(VST® 3 / ARA)。<br>
  A vocal pitch editor with Cubase VariAudio-style controls and one-click scale-degree harmony.</p>
</div>

[日本語](#日本語) | [English](#english)

## 由来・著作権・ライセンス / Origin, attribution and license

Kinu Tune は、Session Loops による [PitchNet](https://github.com/SessionLoops/PitchNet) 0.7.2 を Keinoaza が改変したものです(2026-10-19 から)。PitchNet は [HachiTune](https://github.com/KCKT0112/HachiTune) の派生物です。変更点は [MODIFICATIONS.md](MODIFICATIONS.md) にあります。

Kinu Tune is a modified version of [PitchNet](https://github.com/SessionLoops/PitchNet) 0.7.2 by Session Loops, made by Keinoaza from 2026-10-19. PitchNet is itself a modified derivative of [HachiTune](https://github.com/KCKT0112/HachiTune) by the HachiTune contributors. Kinu Tune includes substantial code derived from PitchNet and HachiTune, along with the changes by Session Loops, other contributors and Keinoaza; this repository does not claim sole authorship of the derived work. The changes are listed in [MODIFICATIONS.md](MODIFICATIONS.md).

- Copyright (C) 2026 Keinoaza (modifications)
- Copyright (c) 2026 Session Loops and contributors (PitchNet)
- Copyright the HachiTune contributors (HachiTune)

Kinu Tune is free software, distributed under the [GNU Affero General Public License, version 3](LICENSE) (AGPLv3), WITHOUT ANY WARRANTY. When you convey copies or modified versions, keep all copyright, license and attribution notices and make the corresponding source available, as the AGPLv3 requires. Third-party components and model files keep their own licenses: see [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

Kinu Tune is not affiliated with or endorsed by Session Loops, the HachiTune contributors, OpenVPI, Steinberg or Celemony. Cubase and VariAudio are trademarks of Steinberg Media Technologies GmbH. VST is a registered trademark of Steinberg Media Technologies GmbH. Melodyne and ARA are trademarks of Celemony Software GmbH.

---

## 日本語

### 概要

PitchNet(Melodyne に近い操作)を、Cubase の VariAudio に慣れた人が使いやすいように作り替えたピッチエディターです。いちばんの目的は、複製したボーカルトラックからハモリを手早く作ることです。

- セグメントは VariAudio と同じ四角で表示し、マウスを乗せると音名とスマートコントロール(カーブのストレート化、ピッチ補正、結合、傾き、フォルマント、音量)が出ます
- ピッチスナップモード(絶対 / 相対 / オフ)、ドラッグ中の Shift / Cmd / Option による切り替え、矢印キー操作
- 左のインスペクタ: Tools(スライダー: ピッチを補正、カーブを平坦化、ピッチドリフト、フォルマントをシフト、ボリューム)、エディタースケール、ハモリ
- ハモリ: 3度・4度・5度・6度・オクターブの上下をワンクリックで。長3度か短3度かはスケールに合わせて自動で決まります
- 編集前のピッチカーブを灰色で表示、ダブルクリックでセグメントを元に戻す

### 動作環境

- macOS 13 以降、Apple シリコン(arm64)。配布している Mac 版は Intel Mac では動きません
- Windows 10/11(x64)。Windows 版は GitHub Actions で自動ビルドしたもので、Windows 上での動作はまだ確認していません
- Cubase など ARA 2 に対応したホスト(ARA なしでも、通常のインサートとして録り込み方式で使えます)

### インストール(配布版)

1. [Releases](https://github.com/keinoaza666-stack/kinu-tune/releases) から `Kinu-Tune-<版>-macOS-arm64.zip` をダウンロードして展開します
2. `Kinu Tune.vst3` を `~/Library/Audio/Plug-Ins/VST3/`(ユーザー > ライブラリ > Audio > Plug-Ins > VST3)に入れます
3. `models` フォルダーを `~/Library/Application Support/Keinoaza/Kinu Tune/` の中に入れます(`.../Kinu Tune/models/fcpe.onnx` などになる配置)
4. 配布版は Apple の公証を受けていないため、ターミナルで次を実行して「開発元未確認」の制限を外します

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Kinu\ Tune.vst3 ~/Library/Application\ Support/Keinoaza/Kinu\ Tune
   ```

5. DAW を起動し直し、プラグインを再スキャンします(Cubase では、スタジオ > VST プラグインマネージャーに Keinoaza の「Kinu Tune」が出ます)

Windows 版(`Kinu-Tune-<版>-Windows-x64.zip`):

1. `Kinu Tune.vst3` フォルダーを `C:\Program Files\Common Files\VST3\` に入れます
2. `models` フォルダーを `%APPDATA%\Keinoaza\Kinu Tune\` の中に入れます(`%APPDATA%\Keinoaza\Kinu Tune\models\fcpe.onnx` などになる配置)
3. DAW を起動し直してプラグインを再スキャンします

### Cubase でハモリを作る

1. ボーカルトラックを複製し、複製側のオーディオイベントを選びます
2. オーディオ > エクステンション > Kinu Tune で解析します
3. 左のエディタースケールで曲のキーを選びます
4. その下のハモリ欄で「3度 上」などを押します(何も選んでいなければ全ノート、選んでいればそのノートだけが動きます)

### AI モデルについて

配布版には次のモデルファイルが入っています。モデルにはそれぞれのライセンスがあります(詳しくは [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt))。

| モデル | 用途 | ライセンス |
|---|---|---|
| FCPE | ピッチ検出(既定) | MIT |
| GAME | ノート分割 | 学習済みモデルは CC BY-NC-SA 4.0(ソースは MIT) |
| PC-NSF-HiFiGAN | 再合成(ボコーダー) | 学習済みモデルは CC BY-NC-SA 4.0([OpenVPI の通知](licenses/PC-NSF-HiFiGAN-NOTICE.txt)) |

- GAME と PC-NSF-HiFiGAN のモデルは CC BY-NC-SA 4.0(非営利・継承)です。ライセンスの「非営利」は "not primarily intended for or directed towards commercial advantage or monetary compensation"(主として商業上の利益や金銭的な対価を目的としないこと)と定義されています。Kinu Tune の配布版は無料です。どんな使い方が許されるかは、[ライセンスの本文](https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode.ja)を確認してください
- モデルは配布版 zip の `models` フォルダーに入っています。各ライセンスの全文と通知は zip とリポジトリの `licenses` フォルダー、[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) にあります
- RMVPE(設定で選べるもう1つのピッチ検出)は、配布元がライセンスを示していないため、配布版には入れていません。自分で入手する権利がある場合は、`rmvpe.onnx` を上記の `models` フォルダーに置くと使えます

### 自分でビルドする

下の [Build](#build) を見てください。`fcpe.onnx`・`cent_table.bin`・`mel_filterbank.bin`・`pc_nsf_hifigan.onnx` はリポジトリの `Resources/models` に入っています。macOS 用の `pc_nsf_hifigan.mlmodelc` はリポジトリに無いので、配布版 zip の `models` フォルダーから `Resources/models` に置いてください。GAME のモデルは構成時に自動でダウンロードされます。

---

## English

### Overview

Kinu Tune reworks PitchNet's Melodyne-style editor for Cubase VariAudio users. Its main goal is quick harmony creation on a duplicated vocal track.

- VariAudio-style rectangular segments with pitch names and smart controls on hover (straighten curve, correct pitch, glue, tilt, formant, volume)
- Pitch Snap Mode Absolute / Relative / Off, with Shift / Cmd / Option overrides while dragging, and arrow-key editing
- Left inspector: Tools (Correct Pitch, Flatten Curve, Pitch Drift, Shift Formant, Volume), Editor Scale, Harmony
- Harmony: one click shifts notes by a 3rd, 4th, 5th, 6th or an octave, up or down; major or minor thirds follow the scale
- The original pitch curve of an edited segment stays visible in grey; double-click resets a segment

### Requirements

- macOS 13 or later on Apple silicon (arm64). The released macOS build does not run on Intel Macs.
- Windows 10/11 (x64). The Windows build is made automatically on GitHub Actions and has not been tested on Windows yet.
- An ARA 2 host such as Cubase. Without ARA, Kinu Tune works as a regular insert in capture mode.

### Installing a release

1. Download `Kinu-Tune-<version>-macOS-arm64.zip` from [Releases](https://github.com/keinoaza666-stack/kinu-tune/releases) and unzip it.
2. Copy `Kinu Tune.vst3` to `~/Library/Audio/Plug-Ins/VST3/`.
3. Copy the `models` folder into `~/Library/Application Support/Keinoaza/Kinu Tune/` (so that `.../Kinu Tune/models/fcpe.onnx` exists).
4. The release is not notarized. Remove the quarantine flag in Terminal:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Kinu\ Tune.vst3 ~/Library/Application\ Support/Keinoaza/Kinu\ Tune
   ```

5. Restart your DAW and rescan plugins.

Windows (`Kinu-Tune-<version>-Windows-x64.zip`):

1. Copy the `Kinu Tune.vst3` folder to `C:\Program Files\Common Files\VST3\`.
2. Copy the `models` folder into `%APPDATA%\Keinoaza\Kinu Tune\` (so that `%APPDATA%\Keinoaza\Kinu Tune\models\fcpe.onnx` exists).
3. Restart your DAW and rescan plugins.

### Model files

| Model | Purpose | License |
|---|---|---|
| FCPE | Pitch detection (default) | MIT |
| GAME | Note segmentation | Model weights: CC BY-NC-SA 4.0 (source: MIT) |
| PC-NSF-HiFiGAN | Resynthesis (vocoder) | Model weights: CC BY-NC-SA 4.0 ([OpenVPI notice](licenses/PC-NSF-HiFiGAN-NOTICE.txt)) |

The GAME and PC-NSF-HiFiGAN weights are licensed under CC BY-NC-SA 4.0 (NonCommercial, ShareAlike). The license defines NonCommercial as "not primarily intended for or directed towards commercial advantage or monetary compensation". Kinu Tune releases are free of charge; read [the license](https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode.en) for what it allows. The models are in the `models` folder of the release zip; full notices and license texts are in the `licenses` folder and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). RMVPE (the optional second pitch detector) is not included because its repository declares no license; if you have the right to use it, put `rmvpe.onnx` in the `models` folder above.

### Build

Requirements: CMake 3.22+, a C++17 compiler, Git (for submodules). On macOS, Xcode or the Command Line Tools.

```bash
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On macOS the default is a universal (arm64 + x86_64) build; the released binary is built with `-DCMAKE_OSX_ARCHITECTURES=arm64`.

The build expects these files in `Resources/models`: `fcpe.onnx`, `cent_table.bin`, `mel_filterbank.bin` and `pc_nsf_hifigan.onnx` are in this repository; `pc_nsf_hifigan.mlmodelc` (required on macOS) is not, so take it from the `models` folder of a release zip. The GAME models are downloaded into `Resources/models/GAME` during configure if missing. `rmvpe.onnx` is optional.

Options added or used by Kinu Tune:

- `-DPITCHNET_BUILD_TESTS=ON` builds the unit tests (`ctest --test-dir build`)
- `-DPITCHNET_DEV_BUILD=ON` builds with a separate development plugin identity (`Kntd`)
- `-DPITCHNET_ARA_DIAGNOSTICS=ON` writes ARA diagnostics to `~/Library/Logs/Kinu Tune/`

Other options from PitchNet:

- `-DUSE_CUDA=ON` / `-DUSE_DIRECTML=ON` (Windows, not both), `-DUSE_ASIO=ON` (Windows)
- `-DUSE_BUNDLED_CUDA_RUNTIME=ON`, `-DUSE_BUNDLED_DIRECTML_RUNTIME=ON` (Windows)
- ONNX Runtime 1.19.2 is downloaded during configure

VST3 and AU are built by default; AAX only when `AAX_SDK_PATH` is set. The Windows release is built by `.github/workflows/windows-release.yml` (run it by hand for a release tag; it uses the model files in this repository and the GAME models that CMake downloads). ARA is enabled when the `third_party/ARA_SDK` submodule is present. Kinu Tune has been tested on macOS (Apple silicon) with Cubase only.

### Project structure

```
Source/
  Audio/        Audio engine, pitch detection, synthesis, vocoder
  Models/       Project, note and audio data models
  UI/           Piano roll, inspector, harmony panel, toolbar
  Utils/        Resampling, localization, harmony and scale utilities
  Undo/         Undo/redo actions
  Plugin/       VST3/AU/AAX/ARA plugin integration
Resources/      Models (not in git), languages, fonts, images
licenses/       Model notices and third-party license texts
tests/          Unit tests
third_party/    JUCE, ARA SDK, r8brain-free-src (submodules)
```

## Acknowledgements

- [PitchNet](https://github.com/SessionLoops/PitchNet) by Session Loops and [HachiTune](https://github.com/KCKT0112/HachiTune) by the HachiTune contributors
- [GAME](https://github.com/openvpi/GAME) and [PC-NSF-HiFiGAN](https://github.com/openvpi/vocoders) by [OpenVPI](https://github.com/openvpi)
- [FCPE](https://github.com/CNChTu/FCPE) by CN_ChiTu, [RMVPE](https://github.com/yxlllc/RMVPE) by yxlllc
- [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) by Aleksey Vaneev, [JUCE](https://juce.com), [ARA SDK](https://github.com/Celemony/ARA_SDK), [ONNX Runtime](https://onnxruntime.ai)

## License

GNU Affero General Public License, version 3. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
