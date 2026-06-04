# XSR Dump 架构文档

## 架构总览

单次 PIE 运行同时导出 GT（原生 100% 渲染）和 XSR（超分输出）配对帧。

```
PIE 同一帧
  ├─ 主渲染 (ScreenPercentage=QualityLevel, SGSR/TSR/TAA)
  │     UpscalerPassInputs.SceneColor (低分辨率)
  │         → 超分 → Outputs.FullRes (高分辨率)
  │                         │
  │                         └── Dump → XSR_XXXX.bin
  │
  └─ GT SceneCapture (100%, 无超分)
        SceneColor (原生高分辨率)
            │
            └── Dump → GT_XXXX.bin

manifest.jsonl 按 frameIndex 配对
```

## 组件分布

| 组件 | 文件 | 职责 |
|---|---|---|
| **XSRDump** | `PostProcessing.cpp` (Engine) | XSR 输出 Readback + 文件写入 + manifest |
| **FSGSRDumpManager** | `SGSRDumpManager.cpp` (SGSR Plugin) | CVar 管理、GT manifest、输出目录 |
| **USGSRDumpSubsystem** | `SGSRDumpSubsystem.cpp` (SGSR Plugin) | GT SceneCapture 管理、相机同步、帧时序 |

## CVar 列表

| CVar | 默认 | 说明 |
|---|---|---|
| `r.XSR.Dump.OnePassGT` | 0 | 1 = 启用配对导出 |
| `r.XSR.Dump.MaxFrames` | 60 | 最大导出帧数，0 = 不限 |
| `r.XSR.Dump.WarmupFrames` | 5 | 跳过前 N 帧等待 History 稳定 |
| `r.XSR.Dump.OutputDir` | 空 | 输出根目录，空 = `Saved/XSR_Dump/<timestamp>/` |
| `r.XSR.Dump.CurrentFrame` | 0 | [内部] 当前帧号，GT 侧递增，XSR 侧读取 |
| `r.XSR.Dump.OutputWidth` | 0 | [内部] XSR 输出宽度，RenderThread → GameThread 同步 |
| `r.XSR.Dump.OutputHeight` | 0 | [内部] XSR 输出高度 |

## XSR 输出 Dump 点

`Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp`

挂载在 `AddPostProcessingPasses` 的三分支汇聚点（TSR / TAA / ThirdParty 统一）：

```cpp
// Line ~1027: SceneColorSlice = Outputs.FullRes 之后
{
    XSRDump::Tick();  // 处理上一帧的 Readback
    if (CVarOnePassGT && View.bIsGameView) {
        XSRDump::EnqueueReadback(GraphBuilder,
            Outputs.FullRes.TextureSRV->Desc.Texture,   // 超分后纹理
            Outputs.FullRes.ViewRect,
            "XSR", FrameIdx, GameFrame);
    }
}
```

**适用于所有时域超分算法**：TSR、TAA (Gen4)、ThirdParty (SGSR等)。

## GT SceneCapture 流程

```
USGSRDumpSubsystem::Tick() (GameThread, TG_DuringPhysics)
  │
  ├─ 处理上一帧的 GT Readback → 写 GT_XXXX.bin + manifest
  │
  ├─ 同步相机参数 (FOV, Location, Rotation, AE settings)
  │
  └─ GTCapture->CaptureSceneDeferred() → 帧末渲染
         │
         └─ RenderThread: ReadSurfaceFloatData → 写文件
```

GT 使用 `USceneCaptureComponent2D` + `SCS_SceneColorHDR`，100% 分辨率，SGSR 不注入。

## 时序

| 帧 N | GameThread | RenderThread |
|---|---|---|
| Tick | 读回帧 N-1 的 GT → 写文件 | — |
| Tick | 设 SceneCapture → CaptureSceneDeferred | — |
| Draw | ENQUEUE 主渲染命令 | 主渲染 → 超分 → Dump XSR |
| FrameEnd | — | SceneCapture 渲染 → GT RT 就绪 |
| 帧 N+1 Tick | 读回帧 N 的 GT | — |

GT 有 1 帧延迟（deferred capture），XSR 有 1-2 帧延迟（GPU Readback 异步）。双端延迟对称，frameIndex 对齐。

## 输出

```
Saved/XSR_Dump/Run_20260602_143000/
  GT_000005.exr.bin         ← 100% 原生 HDR (R11G11B10)
  XSR_000005.exr.bin        ← 超分后 HDR (RGBA FFloat16)
  GT_000006.exr.bin
  XSR_000006.exr.bin
  manifest.jsonl             ← 配对元数据
```

文件格式：`[int32 w][int32 h][像素数据]`

## 使用

```ini
# PIE 前在控制台设置
r.XSR.Dump.OnePassGT 1
r.XSR.Dump.MaxFrames 120
r.XSR.Dump.WarmupFrames 10
r.EyeAdaptationQuality 0     # 建议禁用自动曝光
```

## 质量评估工具

| 脚本 | 功能 |
|---|---|
| `convert_png.py` | .bin → PNG (ACES tonemap) |
| `ssim.py` | GT vs XSR 逐帧 SSIM |
| `psnr.py` | GT vs XSR 逐帧 PSNR |
| `diff.py` | GT vs XSR 逐像素差异图 |
| `temporal_lpips.py` | 帧间 LPIPS 时序稳定性 |
| `temporal_stability.py` | 相邻帧差异图 SSIM 时序稳定性 |
| `plot_temporal_lpips_mpl.py` | LPIPS 曲线图 (matplotlib) |
| `plot_temporal_ssim.py` | SSIM 曲线图 (matplotlib) |
| `match.py` | 帧配对检测 |
| `compute_lpips.py` | GT vs XSR 逐帧 LPIPS |
