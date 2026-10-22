# UI設定の場所

既存の見た目・サイズ・操作量を維持して、設定値と描画処理を分離しています。

| 変更したい項目 | 定義場所 |
| --- | --- |
| 初期横幅、プレイヤー・プレイリストの高さ、余白 | `../MediaPlayerEditorLayout.h` の共通定数 |
| タイトル・時間・各ボタン・音量の位置と寸法 | 同ファイルの `Player` |
| Playlist／Clear、行内の列、枠、リサイズグリップ | 同ファイルの `Playlist` |
| 幅・高さに追従する実座標の計算 | `../MediaPlayerEditorLayout.cpp` |
| 本体・動画画面で共通の色の用途割り当て | `MediaPlayerEditorStyle.h` の `Colors` |
| ロゴ・ファイル名・補助文字・リストのフォント | 同ファイルの `Fonts` |
| シークバー・再生アイコン・Openアイコン・線・スクロールバーの描画寸法 | 同ファイルの `Seek`／`Transport`／`Folder`／`Playlist`／`Scrollbar` |
| 更新周期、ドラッグスクロール、シークの段階数、音量操作量 | 同ファイルの `Behavior` |
| 共通パレットのRGB値 | `vst_base_palette.h` |
| 共通ノブの寸法・線幅・角度 | `vst_base_controls.h` の `KnobStyle` |
| 共通ノブの描画処理 | `vst_base_knob_win32.h` |

`vst_base_*` は同梱した共通部品です。プレイヤー固有の調整はレイアウト定義と `MediaPlayerEditorStyle.h` に置き、共通パレットのRGB値を描画側へ複製しません。

配置・文字列の描画領域は `EditorLayout::calculate`／`calculatePlaylistRow`／`calculateResizeGrip` の結果を使用します。プレイリストの初期高はUI・メッセージ・状態同期で同じ定義を参照します。フォント生成とシーク位置への変換も共通処理を通します。
