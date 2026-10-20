# Kinu Tune の開発ルール(設計の約束)

コードを変える前に読むファイルです。ここには「守らないと別の所が壊れる約束」だけを書きます。ビルド手順・検証の記録・過去の経緯は、リポジトリの外の `verify-map.md` にあります。

約束を新しく見つけたら、その変更と同じコミットでここに書き足します。1つの約束につき「何を守るか」「必ず通す入口」「破るとどのテストで分かるか」を書きます。

## 1. 音程を書き換える処理は NotePitch だけを通す

- ノートの音程(`Note::midiNote`)と「ピッチを補正」の基準(`Note::lastNonMacroMidiNote`、歌った音程)は、`Source/Utils/NotePitch.h` の関数だけで書き換える
  - 編集で動かす(ドラッグ・矢印キー・ハモリ・吸着・傾き・スマートコントロール): `NotePitch::applyEdit`(元に戻す記録用の値は `sourceAfterEdit`)
  - 記録した値を戻す(元に戻す・やり直し・取り消し・読み込み・分割と結合): `NotePitch::restore`
  - 補正そのもの(ピッチを補正・ピッチセンター): `NotePitch::applyCorrection` と `setCorrectionSource`
  - 新しく歌った音程にする(ピッチの描き込み): `NotePitch::setSungPitch`
- 元に戻す動作(`Source/Undo/`)は、基準の値を省略できない作りにしてある。新しい動作を足す時も、前後の基準を必ず受け取る
- 破ると: テスト `SourceGuard_pitch-writes` が、直接書き換えている行を示して落ちる。基準の扱いの誤りは `VariAudioInspectorTests` が拾う
- この検査は1行ずつ読むので、メンバー関数ポインタ経由の呼び出しや、呼び出しを複数行に分けた書き方は見逃す。そういう書き方をしない

## 2. 再生以外で音を出す処理は AuditionOutput だけを通す

- ドラッグ中の試聴・鍵盤の試聴・範囲の試聴は、出口が1つ(プラグインではホストの試聴経路、単体アプリでは AudioEngine の試聴ループ)。`Source/UI/Main/AuditionOutput.h` の `startHostPreview` / `playLoop` / `stopLoop` / `stopHostPreview` だけで鳴らす・止める
- 鳴らし始める時は、出口を前の持ち主から取り上げる(`claim`。前の持ち主には `setOnTakenOver` で登録した処理が呼ばれる)。新しい持ち主もループを鳴らすなら前のループは置き換わり、そうでなければ(範囲の試聴)前のループは止める。再生・一時停止・停止は、先に `yieldToTransport` を呼ぶ
- 新しい種類の試聴を足す時は、`AuditionOutput::Source` の `Count` より前に種類を足し、ループを鳴らす種類なら `playsLoop` にも足す。取り上げられた時の処理を MainComponent の構築時に登録する
- ホストへの4つのコールバック(試聴範囲の開始・停止、ループの再生・停止)は AuditionOutput の中にしか無い。MainComponent から直接は呼べない
- 破ると: テスト `SourceGuard_audition-output` が、AuditionOutput と AudioEngine 以外から試聴ループを直接呼んでいる行を示して落ちる(1行ずつ読む検査なので、§1 と同じ書き方の制限がある)。受け渡しの誤りは `AuditionOutputTests`(単体アプリの経路を含む)と `AuditionHandoffTests`(本物の MainComponent)が拾う

## 3. プロジェクトは持たずに毎回問い合わせる

- プロジェクトが差し替わる経路は十数か所ある(ファイルを開く・スナップショットの復元・ARA など)。画面の部品にプロジェクトのポインタを持たせず、`getCurrentProject` で毎回問い合わせるか、`parameterPanel.onProjectBound` に乗せる
- 破ると: 自動テストは無い。キーが「-」になる、別のイベントの値が出る、といった症状で現れる

## 4. ノートのポインタは編集のたびに捨てる

- 元に戻す・やり直し・結合・分割の後はノートの一覧が作り直される。マウスを乗せたノートやハンドルのポインタは `PianoRollComponent::dropTransientNotePointers()` で捨てる(`MainComponent::afterHistoryChange` から呼ぶ)
- 破ると: 消えたノートを触って落ちる

## 5. キーとスケールの読み方

- ハモリとエディタースケールのキーは `Project::getPreferredScaleMode()` / `getScaleRootNote()` で読む。`getScaleMode()` はドラッグの吸着の設定で、クロマチックを返すことがある
- ユーザーがキーを変える入口は、左のインスペクタのエディタースケールだけ(`ParameterPanel` は未設定のルートを整える時にだけ書き込む)

## 6. インスペクタの表示の読み直し

- インスペクタは 1 秒に 8 回、表示を読み直す。プルダウンを読み直す時は `VariAudioInspector::showMenuValue` を通す(ポップアップを開いている間と、自分が最後に書いた値と違う間は触らない)。直接 `setSelectedId` すると、選んだ値が消える

## 7. Windows でも通る書き方

- キャプチャなしのラムダ `[]` の中でローカルの `constexpr` を読まない(Windows のコンパイラ MSVC が通さない)。`[&]` にする
- 破ると: GitHub の自動チェック(CI)の `windows-build` が落ちる
- Windows のプラグインは C++ の実行時ライブラリ(MSVC ランタイム)を静的にリンクする(`CMakeLists.txt` の `CMAKE_MSVC_RUNTIME_LIBRARY`。ターゲットを作る前に設定する)。共有のランタイムだと、DAW が自分のフォルダーに持っている古い `msvcp140.dll` を使うことになり、新しいコンパイラーで作った `std::mutex` が最初のロックで落ちる(一覧には出るが、挿した瞬間に落ちる)。ONNX Runtime の DLL は共有のランタイムのままだが、Microsoft が古いランタイムでも動くように作っている
- 破ると: CI の `windows-build` で `tools/windows/check-vst3.ps1` が落ちる(プラグインが `msvcp140.dll`・`vcruntime140.dll` を読み込む指定を持っている、または Visual C++ 14.29 のランタイムで作成できない)。Windows 版のリリース用ワークフローも、同じ検査に通らなければ zip を付けない

## 8. 文面を変えたらバイナリも作り直す

- `THIRD_PARTY_NOTICES.txt` と `Resources/lang/*.json` はプラグイン本体に埋め込まれる。変えたら Mac 版・Windows 版を作り直して配布物を差し替える

## 9. 配布するバイナリにビルドした Mac のパスを残さない

- リンクの時に、ビルドフォルダー内の ONNX Runtime の場所(ユーザー名を含む絶対パス)が、ライブラリを探す場所(rpath)として記録される。Mac のバンドルは `cmake/StripBuildRpaths.cmake` が絶対パスの rpath を消し、残っていればビルドを失敗させる
- 新しい Mac 用のターゲット(バンドル)を足したら、同じ処理を POST_BUILD に足す
- 確かめ方: `otool -l <バイナリ> | grep -A2 LC_RPATH` に `/` で始まるパスが無いこと。`strings` では rpath が出ないので、これで確かめる

## 10. 変更の進め方

- 不具合を直す時は、直す前に失敗するテストを足す
- コミットの前に、テスト全部(`ctest --test-dir build`)と pluginval(厳しさ 5)を通す。`pitchnet_ui` にリンクするテストは、UI を変えたら全体をビルドしてから流す
- GitHub に送ったら、自動チェック(CI)が通ったことを確かめる
- 新しく作るソースファイルの先頭には、Kinu Tune の著作権とライセンスの3行を書く(既存の新規ファイルと同じ文面)
