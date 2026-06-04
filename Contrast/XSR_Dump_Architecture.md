# XSR Dump 架构文档

## 目标

单次 PIE 运行中，同一游戏帧同时导出 GT（原生 100% 渲染）和 XSR（超分输出）配对帧，用于超分算法画质评估。

## 整体流程

```
PIE 启动
  → UWorld::InitWorld → InitializeSubsystems
    → USGSRDumpSubsystem 自动创建、Initialize
      → 创建 USceneCaptureComponent2D + UTextureRenderTarget2D
      → 注册到 FTickableGameObject 全局列表

引擎主循环每帧:
  UWorld::Tick
    → FTickableGameObject::TickObjects
      → USGSRDumpSubsystem::Tick()
        ├─ [1] 读回上一帧 GT RenderTarget → 写 GT_XXXX.bin
        ├─ [2] 同步相机参数 (Transform, FOV, AE)
        ├─ [3] GTCapture->CaptureSceneDeferred() → 帧末渲染
        └─ [4] 帧号计数 (BeginFrame)
    → Slate Paint → Draw → 主渲染
      → AddPostProcessingPasses
        → [TSR|TAA|ThirdParty] 超分
        → Outputs.FullRes 就绪
        → XSRDump::Tick()         ← 处理上一帧 Readback
        → XSRDump::EnqueueReadback ← 入队当前帧超分输出
```

## 组件架构

```
┌─────────────────────────────────────────────────────────┐
│                    PostProcessing.cpp (Engine)          │
│  namespace XSRDump {                                    │
│    Init()        → 输出目录 & 路径发现                    │
│    Tick()        → 处理就绪 GPU Readback → 写 .bin+manifest│
│    EnqueueReadback() → AddEnqueueCopyPass 入队          │
│  }                                                      │
│  通用挂载点: SceneColorSlice = Outputs.FullRes 之后       │
│  适用于 TSR / TAA / ThirdParty 全部超分算法              │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                SGSR Plugin (SGSRTUModule)                │
│                                                          │
│  FSGSRDumpManager (单例)                                 │
│    Init()        → 时间戳子目录 + CVar 注册              │
│    BeginFrame()  → 帧号管理、Warmup、MaxFrames 检查      │
│    OnGTWritten() → GT manifest 行 + TotalDumped 计数    │
│                                                          │
│  USGSRDumpSubsystem : UTickableWorldSubsystem            │
│    Initialize()  → 创建 SceneCapture + RenderTarget      │
│    Tick()        → 同步相机 / 触发捕获 / 读回 GT         │
│    Deinitialize()→ 清理资源                              │
└─────────────────────────────────────────────────────────┘
```

## Subsystem 自动注册与生命周期

### 类声明

```cpp
UCLASS()
class USGSRDumpSubsystem : public UTickableWorldSubsystem
{
    // UTickableWorldSubsystem 继承 UWorldSubsystem + FTickableGameObject
    // 不需要手动注册，UWorld 通过反射系统 GetDerivedClasses 自动发现
};
```

### 自动注册机制

`UWorld::InitWorld()` → `InitializeSubsystems()` → `FSubsystemCollectionBase::Initialize()`：

```cpp
// SubsystemCollection.cpp:184-185
TArray<UClass*> SubsystemClasses;
GetDerivedClasses(UWorldSubsystem, SubsystemClasses, true);
// 扫描所有 UCLASS() 标记的 UWorldSubsystem 子类
for (UClass* SubsystemClass : SubsystemClasses)
    AddAndInitializeSubsystem(SubsystemClass);
```

`AddAndInitializeSubsystem`:
1. `NewObject<USGSRDumpSubsystem>()` — 反射构造 → `FTickableGameObject` 构造入队
2. `Subsystem->Initialize()` → `SetTickableTickType(Conditional)` → 移入 Tick 主列表

### Tick 触发链

```
UWorld::Tick
  → TG_DuringPhysics
    → FTickableGameObject::TickObjects()
      → USGSRDumpSubsystem::Tick(DeltaTime)
```

每帧自动调用，PIE 退出时 `Deinitialize()` 自动清理。

### SceneCapture 初始化

```cpp
void USGSRDumpSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    // 1. 创建 RenderTarget (RGBA16F HDR)
    GTRenderTarget = NewObject<UTextureRenderTarget2D>();
    GTRenderTarget->RenderTargetFormat = RTF_RGBA16f;

    // 2. 创建 SceneCapture，捕获 HDR SceneColor，关联 RenderTarget
    GTCapture = NewObject<USceneCaptureComponent2D>();
    GTCapture->TextureTarget = GTRenderTarget;
    GTCapture->CaptureSource = SCS_SceneColorHDR;
    GTCapture->bCaptureEveryFrame = false;

    // 3. 注册到 World（使 GetWorld() / Scene 引用有效）
    GTCapture->RegisterComponentWithWorld(GetWorld());
}
```

## GT Tick 帧捕获流程

```cpp
void USGSRDumpSubsystem::Tick(float DeltaTime)
{
    // === 1. 读回上一帧 GT ===
    if (PendingReadbackFrame >= 0) {
        // 在 GameThread 获取 RenderTarget 资源
        FTextureRenderTargetResource* RTRes = GTRenderTarget->GameThread_GetRenderTargetResource();
        // 入队 RenderCommand：ReadSurfaceFloatData → 写 GT_XXXX.bin
        ENQUEUE_RENDER_COMMAND(SGSRDumpGTReadback)(
            [RTRes, RTSize, FrameIdx](FRHICommandListImmediate& RHICmdList) {
                TArray<FFloat16Color> Pixels;
                RHICmdList.ReadSurfaceFloatData(RTRes->GetTexture(), Rect, Pixels, Flags);
                FFileHelper::SaveArrayToFile(Out, *Path);
                FSGSRDumpManager::Get().OnGTWritten(FrameIdx, ...);
            });
        PendingReadbackFrame = -1;
    }

    // === 2. BeginFrame (帧号管理) ===
    CurrentFrameIndex = FSGSRDumpManager::Get().BeginFrame(GFrameCounter);
    if (CurrentFrameIndex < 0) return;  // warmup / max reached

    // === 3. 更新 RenderTarget 尺寸 ===
    if (GEngine->GameViewport->Viewport) {
        FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
        GTRenderTarget->InitAutoFormat(Size.X, Size.Y);
    }

    // === 4. 同步相机参数 ===
    SyncCameraFromPlayer();

    // === 5. 延迟捕获 (帧末渲染) ===
    GTCapture->CaptureSceneDeferred();
    PendingReadbackFrame = CurrentFrameIndex;  // 下帧读回
}
```

## GT 相机参数同步

```cpp
void USGSRDumpSubsystem::SyncCameraFromPlayer()
{
    APlayerCameraManager* CamMgr = CachedPlayerController->PlayerCameraManager;

    // 1. 位置和旋转
    GTCapture->SetWorldLocationAndRotation(
        CamMgr->GetCameraLocation(), CamMgr->GetCameraRotation());

    // 2. FOV (从 GetCameraCacheView 获取，包含 CameraComponent 覆盖值)
    const FMinimalViewInfo& CachedView = CamMgr->GetCameraCacheView();
    GTCapture->FOVAngle = CachedView.FOV;

    // 3. 投影矩阵 (匹配主视图的 AspectRatio)
    GTCapture->bUseCustomProjectionMatrix = true;
    // ... 计算匹配主视图的 ProjectionMatrix ...

    // 4. 曝光参数同步
    GTCapture->PostProcessSettings.bOverride_AutoExposureBias = true;
    GTCapture->PostProcessSettings.AutoExposureBias =
        CachedView.PostProcessSettings.AutoExposureBias;
    // ... MinBrightness, MaxBrightness ...
}
```

## XSR 超分输出 Dump

### 管线位置

```
AddPostProcessingPasses
  │
  ├─ FDefaultTemporalUpscaler::FOutputs Outputs;
  │
  ├─ [TSR]      AddTemporalSuperResolutionPasses → Outputs
  ├─ [TAA]      AddGen4MainTemporalAAPasses       → Outputs
  └─ [ThirdParty] AddThirdPartyTemporalUpscalerPasses → Outputs
  │
  ├─ SceneColorSlice = Outputs.FullRes;    ← ★ 通用挂载点
  │
  └─ // --- XSR Dump (适用于所有超分算法) ---
     {
         XSRDump::Tick();  // 处理上一帧 GPU Readback
         if (r.XSR.Dump.OnePassGT && View.bIsGameView) {
             // AddEnqueueCopyPass: GPU → CPU 异步拷贝
             XSRDump::EnqueueReadback(
                 GraphBuilder,
                 Outputs.FullRes.TextureSRV->Desc.Texture,  // FRDGTexture*
                 Outputs.FullRes.ViewRect,
                 "XSR", FrameIdx, GameFrame);
         }
     }
```

### Readback 流程

```
帧 N RenderThread:
  EnqueueReadback()
    → AddEnqueueCopyPass(GraphBuilder, Readback, Texture)
       → GPU 异步把超分后纹理拷贝到 Staging Buffer

帧 N+1 RenderThread (XSRDump::Tick):
  → Readback->IsReady()?
    → Lock()  → 读取 RGBA FFloat16 数据
    → SaveArrayToFile → XSR_XXXX.exr.bin
    → Unlock() → delete Readback
    → 追加 manifest.jsonl
```

## 输出

```
Saved/XSR_Dump/Run_20260602_143000/
  GT_000005.exr.bin     ← 100% 原生 HDR (R11G11B10, 4 bytes/pixel)
  XSR_000005.exr.bin    ← 超分后 HDR (RGBA FFloat16, 8 bytes/pixel)
  GT_000006.exr.bin
  XSR_000006.exr.bin
  manifest.jsonl        ← 配对元数据
```

### 二进制格式

```
[int32] width
[int32] height
[width × height × bytesPerPixel] 像素数据 (row-major, FFloat16 或 R11G11B10)
```

### manifest.jsonl

```json
{"frameIndex":5,"gameFrame":1927,"tag":"XSR","file":"XSR_000005.exr.bin","width":2552,"height":1312}
{"frameIndex":5,"gameFrame":1927,"tag":"GT","file":"GT_000005.exr.bin","width":2549,"height":1309}
```

## 使用

```ini
# PIE 控制台 (建议 PIE 启动前设好)
r.XSR.Dump.OnePassGT 1
r.XSR.Dump.MaxFrames 120
r.XSR.Dump.WarmupFrames 10
r.EyeAdaptationQuality 0        # 建议禁用自动曝光
```

## 质量评估工具

所有脚本位于 `Plugins/SGSR/Tools/`，零依赖（除 `compute_lpips.py` 需 PyTorch）。

### 工具总览

| 脚本 | 功能 | 依赖 |
|---|---|---|
| `convert_png.py` | `.bin` → PNG (ACES tonemap) | stdlib |
| `psnr.py` | 逐帧 GT vs XSR PSNR | stdlib |
| `ssim.py` | 逐帧 GT vs XSR SSIM | stdlib |
| `diff.py` | 逐像素差异热力图 | stdlib |
| `match.py` | 帧配对偏移检测 | stdlib |
| `compute_lpips.py` | 逐帧 GT vs XSR LPIPS | torch, lpips |
| `temporal_lpips.py` | 时序 LPIPS 稳定性 | torch, lpips |
| `temporal_stability.py` | 时序 SSIM 稳定性 + 差异图 | stdlib |
| `plot_temporal_lpips_mpl.py` | LPIPS 曲线图 | matplotlib |
| `plot_temporal_ssim.py` | SSIM 曲线图 | matplotlib |
| `plot_temporal_lpips.py` | LPIPS 曲线图 (纯 PIL) | PIL |

### 1. 转 PNG

```bash
py convert_png.py Saved/XSR_Dump/Run_xxx/                 # 全部
py convert_png.py Saved/XSR_Dump/Run_xxx/ --range 5 60    # 指定范围
```

输出到输入目录下的 `png/` 子目录。自动识别 `R11G11B10` (4B/pixel) 和 `RGBA16F` (8B/pixel) 两种格式。ACES filmic tonemap + sRGB gamma。

### 2. 逐帧 PSNR

```bash
py psnr.py Saved/XSR_Dump/Run_xxx/png 7                  # 单帧
py psnr.py Saved/XSR_Dump/Run_xxx/png --range 5 60       # 批量
py psnr.py Saved/XSR_Dump/Run_xxx/png --all              # 全部配对帧
```

PSNR = `20×log₁₀(255/√MSE)`。纯 Python stdlib，1500×900 单帧约 1-2 秒。

输出示例：
```
  frame      7  PSNR = 23.45 dB  (1520x912)
  frame      8  PSNR = 23.51 dB  (1520x912)
  ---- Average PSNR: 23.48 dB (8 frames) ----
```

### 3. 逐帧 SSIM

```bash
py ssim.py Saved/XSR_Dump/Run_xxx/png 7                  # 单帧
py ssim.py Saved/XSR_Dump/Run_xxx/png --range 5 60       # 批量
py ssim.py Saved/XSR_Dump/Run_xxx/png --all              # 全部配对帧
```

8×8 非重叠块亮度 SSIM。纯 Python stdlib，1500×900 单帧约 15-30 秒。

输出示例：
```
  frame      7  SSIM = 0.852310
  frame      8  SSIM = 0.851240
  ---- Avg SSIM: 0.851780 (8 frames) ----
```

### 4. 逐帧 LPIPS

```bash
pip install torch torchvision lpips pillow
py compute_lpips.py Saved/XSR_Dump/Run_xxx/png 7
py compute_lpips.py Saved/XSR_Dump/Run_xxx/png --range 5 60
```

LPIPS = AlexNet 特征空间感知距离。首次运行下载模型 (~233MB)。

### 5. 差异图

```bash
py diff.py Saved/XSR_Dump/Run_xxx/png 7                  # 单帧
py diff.py Saved/XSR_Dump/Run_xxx/png --range 5 20       # 批量
```

对每个像素计算 `|GT - XSR| × 5`，输出 `diff_000007.png`。红色 = 差异大。

### 6. 帧配对检测

```bash
py match.py Saved/XSR_Dump/Run_xxx/png 7                 # 搜索 ±5 帧
py match.py Saved/XSR_Dump/Run_xxx/png 7 --radius 10     # 搜索 ±10 帧
```

给定 GT 帧 N，搜索周围 [N-R, N+R] 范围的 SGSR 帧，找 PSNR 最高的匹配。用于检测帧偏移。

### 7. 时序稳定性 (Temporal SSIM)

```bash
py temporal_stability.py Saved/XSR_Dump/Run_xxx/png --range 5 60
```

**原理**：
- GT 序列：计算相邻帧 (N, N+1) 差异图 → `GT_diff_XXXX_YYYY.png`
- XSR 序列：同样计算相邻帧差异图 → `SGSR_diff_XXXX_YYYY.png`
- 对每对差异图计算 SSIM(GT_diff, SGSR_diff)

**输出**：
```
Temporal stability: frames 5-60
       Pair     SSIM
      5-6       0.8234
      6-7       0.8156
      ...
Temporal Stability SSIM (GT-diff vs SGSR-diff):
  Mean: 0.8200  Std: 0.0120
  Higher = more temporally stable (closer to GT)
  Diffs: Saved/XSR_Dump/Run_xxx/png/temporal_diff/
```

### 8. 时序稳定性 (Temporal LPIPS)

```bash
pip install torch torchvision lpips pillow
py temporal_lpips.py Saved/XSR_Dump/Run_xxx/png --range 5 60
```

**原理**：
- GT 序列：计算每对相邻帧 (N, N+1) 的 LPIPS → `GT_LPIPS[0..M]`
- XSR 序列：同样计算 → `SGSR_LPIPS[0..M]`
- 核心指标：`MAE = mean(|GT_LPIPS[i] - SGSR_LPIPS[i]|)`

**输出**：
```
    5-6      GT_LPIPS=0.02345  SGSR_LPIPS=0.02456  |err|=0.00111
    6-7      GT_LPIPS=0.02210  SGSR_LPIPS=0.02340  |err|=0.00130
Temporal LPIPS Error (MAE = mean |GT_LPIPS - SGSR_LPIPS|):
  MAE: 0.00120  (lower = better temporal stability)
  Std: 0.00015
  GT_LPIPS  mean: 0.02280
  SGSR_LPIPS mean: 0.02400
```

| MAE 范围 | 含义 |
|---|---|
| < 0.01 | XSR 帧间变化几乎和 GT 一致 |
| 0.01-0.03 | 有微量额外 flicker |
| > 0.05 | 时序不稳定明显 |

### 9. LPIPS 曲线图 (matplotlib)

```bash
py plot_temporal_lpips_mpl.py Saved/XSR_Dump/Run_xxx/png --range 5 60
py plot_temporal_lpips_mpl.py Saved/XSR_Dump/Run_xxx/png --range 5 60 --output my_curve.png
```

生成 `temporal_lpips_curve.png`：X 轴帧号，Y 轴 LPIPS。蓝色 = GT，红色 = XSR。右上角标注 MAE/Std。

### 10. SSIM 曲线图 (matplotlib)

```bash
py plot_temporal_ssim.py Saved/XSR_Dump/Run_xxx/png --range 5 60
py plot_temporal_ssim.py Saved/XSR_Dump/Run_xxx/png --range 5 60 --output my_curve.png
```

生成 `temporal_ssim_curve.png`：X 轴帧号，Y 轴 SSIM（相邻帧相似度）。蓝色 = GT，红色 = XSR。

## 修改的文件

| 文件 | 改动 |
|---|---|
| `Engine/.../PostProcess/PostProcessing.cpp` | XSRDump namespace + 通用挂载点 |
| `Plugins/SGSR/.../SGSRDumpManager.h/cpp` | CVar 管理、GT manifest、目录初始化 |
| `Plugins/SGSR/.../SGSRDumpSubsystem.h/cpp` | UTickableWorldSubsystem、SceneCapture、GT Readback |
| `Plugins/SGSR/.../SGSRSUModule/.../SGSRSUViewExtension.cpp` | 加 `GetTemporalUpscalerInterface() == nullptr` 保护 |
