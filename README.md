# 🎚️ MP3 Player + VU / LED メーター (C/C++ → WASM)

**日本語** | [English](README.en.md)

MP3 の**デコード**・**音量レベル計算**・**メーターの描画**まで、**すべて C/C++** で行い WebAssembly にコンパイルしたブラウザ用 MP3 プレーヤーです。
JavaScript はブラウザに必須の部分（ファイル選択・Web Audio 再生・canvas への転送）だけを担当します。

2 種類のレベル表示をボタンで切り替えられます:

- **VU メーター** — アナログ針・目盛り・レッドゾーンをC++が毎フレーム描画。RMS＋VU的な針の弾道、**ピークホールド**と**クリップLED**付き。
- **LED スペクトラム** — 自作 FFT で対数帯域に分割し、緑/黄/赤の LED 段で表示。各バンドに**ピーク保持ドット**。

## 🎮 オンラインデモ

[▶ デモを開く](https://yomei-o.github.io/mp3vu_cpp/wasmdist)

MP3 を開く（またはドラッグ&ドロップ）→ 再生 → ボタンで **VU ⇄ LED** を切替。
**すべて端末内で完結**し、ファイルはどこにも送信しません。

<p>
  <img src="vu_test.png" width="360" alt="VU メーター表示">
</p>

*スクリーンショットは VU モード（L / R の 2 連メーター）*

## 💡 仕組み

```
MP3 file ─▶ [C/C++ WASM] minimp3 でデコード ─▶ float PCM
                    │
                    ├─▶ Web Audio (JS) ────────────▶ 🔊 再生
                    │
                    └─▶ [C/C++ WASM] レベル計算＋メーター描画
                            VU : RMS → dB → 針の弾道（+ピーク/クリップ）
                            LED: FFT → 対数帯域 → LED段（+ピークドット）
                          → RGBA バッファ ─▶ (JS) canvas へ blit
```

- **デコード**：`minimp3`（public domain）。`mp3dec_load_buf` でファイル全体を float PCM に展開。
- **VU レベル**：直近 50ms の RMS を dB 化し、`-45〜0 dBFS` を `0〜1` に正規化。**アタック速め・戻り遅め**の針の弾道で滑らかに追従。ピークは一定時間保持後にゆっくり降下。レッドゾーン（0 VU 以上）到達でクリップ LED 点灯。
- **LED スペクトラム**：2048 点の自作 radix-2 FFT（依存なし）。Hann 窓 → 40Hz〜16kHz を対数で 20 バンドに集約 → dB 化して LED 段数に変換。高域が見えやすいよう緩やかなチルトを加算。
- **描画**：針・弧・目盛り・LED はすべて C++ が RGBA フレームバッファに直接ピクセルを打って描いています（5×7 の自作フォント含む）。

## 🛠 WASM のビルド

[Emscripten](https://emscripten.org/) が必要です。

```bash
# EMSDK のパスを環境変数で指定（既定: /c/prog/emsdk/emsdk）
EMSDK=/path/to/emsdk ./build_wasm_mp3.sh
```

`wasmdist/`（`mp3.js` + `mp3.wasm` + `index.html`）が生成されます。任意の静的ホスティングに置けば動作します（`file://` ではなく HTTP 経由で開いてください）。

## 📁 ファイル構成

| ファイル | 役割 |
|---------|------|
| `wasm_mp3.cpp` | 本体：デコード呼び出し・VU/LED レベル計算・メーター描画（C++）|
| `minimp3.h` / `minimp3_ex.h` | MP3 デコーダ（public domain, [lieff/minimp3](https://github.com/lieff/minimp3)）|
| `build_wasm_mp3.sh` | Emscripten ビルドスクリプト |
| `wasmdist/` | ビルド済みデモ一式（`mp3.js` / `mp3.wasm` / `index.html`）|
| `vu_*.png` | スクリーンショット |

## 📝 ライセンス / 注意

- 本体コードはオリジナルです。
- `minimp3` は public domain（CC0）です。詳細は各ヘッダを参照してください。
