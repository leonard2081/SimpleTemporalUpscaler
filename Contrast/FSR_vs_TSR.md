# FSR3 vs TSR — 关键技术对比分析

> FSR: `C:\Work\Reference\FidelityFX-SDK-main\Kits\FidelityFX\upscalers\fsr3\`
> TSR: `C:\Work\Repo\GUSD\UE_553\Engine\Shaders\Private\TemporalSuperResolution\`

---

## 一、总体架构对比

| 维度 | FSR3 | TSR |
|------|------|-----|
| **Pass 数量** | 7 passes（PrepareInputs → Reproject → Accumulate → 等） | 7-8 passes（DilateVel → Decimate → Reject → SAA → Update → Resolve） |
| **颜色空间** | YCoCg | GCS (guide) + SMCS (shading measurement) |
| **历史存储** | 单帧 color + lock 状态 | 多帧 Texture2DArray (rolling buffer) + metadata validity |
| **上采样核** | Lanczos（近似/LUT/参考 三档） | Catmull-Rom 5-tap bicubic |
| **锐化** | RCAS (Robust Contrast-Adaptive Sharpening) | 无显式锐化 pass |
| **深度使用** | Reconstructed depth + depth clip | 3×3 深度邻域 + ClosestOccluder + parallax disocclusion |
| **HDR 处理** | Tonemap(Hable) / InverseTonemap | GCS/SMCS 感知色调编码 + HDR Karis Weight |
| **Resurrection** | 无 | 多帧回退 + ClipToResurrectionClip |
| **Spatial AA** | 无独立模块 | 独立 SpatialAntiAliasing pass |
| **Hole filling** | 无 | 18-bit 极坐标 velocity 编码 |
| **Reprojection field** | 无 | 3-slice reprojection field (Vector + Jacobian + Boundary) |
| **Lens distortion** | 不支持 | 支持 |

---

## 二、逐技术对比

### 1. 当前帧上采样

| | FSR3 | TSR |
|---|---|---|
| **核函数** | Lanczos (lobes=2), 3 档近似级别 | Catmull-Rom 5-tap bicubic |
| **采样模式** | Jitter 驱动：`hrUv + Jitter/RenderSize` → Lanczos 加权 2×2 邻域 | `ViewportUVToInputPPCo` → Catmull-Rom 5 样本 |
| **关键文件** | `ffx_fsr3upscaler_upsample.h:33-65` | `TSRKernels.ush:355-429`, `TSRUpdateHistory.usf:941-971` |
| **权重公式** | `Lanczos2(dot(sampleOffset * kernelWeight, sampleOffset * kernelWeight).sq)` | `saturate((0.9*d²-1.9)*d²+1.0+min)` |
| **HDR 加权** | 无显式 HDR 加权 | `HdrWeight4` (Karis: `1/(Luma+4)`) 用于空间滤波 |

### 2. 颜色 Clamp (history rectification)

| | FSR3 | TSR |
|---|---|---|
| **方法** | **椭圆 clamp**: RectificationBox (boxCenter + boxVec + sphere test) | **AABB clamp**: InputMinColor / InputMaxColor + HDR 加权 lerp |
| **颜色空间** | YCoCg → Tonemap → clamp → InverseTonemap | SMCS (GCS²) 空间 |
| **Box 构建** | 当前帧 2×2 邻域 + AABB min/max + center/vec 推导 | 当前帧 CONFIG_SAMPLES_COUNT 邻域 + AABB min/max |
| **Clamp 策略** | 球面归一化到 box 内 (`normalize → *boxVec + center`) | `fastClamp(history, InputMin, InputMax)` |
| **HDR 处理** | Tonemap(RGB) 后再 YCoCg，最后 InverseTonemap | GCS/SMCS 感知编码，DisableHistoryClamp + HDR-weighted lerp |
| **关键文件** | `ffx_fsr3upscaler_common.h:149-195`, `accumulate.h:44-70` | `TSRUpdateHistory.usf:1185-1213`, `1235-1237` |
| **Box 缩放因子** | `fBoxScaleT = max(velocity, distance, accumulation, reactive, shadingChange)` → `lerp(3.0, 1.0)` | `MaxValidity = 1.0 - WeightClampingRejection * DecreaseValidityMultiplier` |

### 3. Disocclusion / 反遮挡检测

| | FSR3 | TSR |
|---|---|---|
| **主要方法** | Depth Clip（深度裁剪）| Parallax Disocclusion（视差反遮挡） |
| **深度采样** | ReconstructPrevDepth: 当前深度投影到上一帧位置，bilinear scatter | ClosestOccluder: InterlockedMax 原子散射到 PrevAtomic |
| **深度比较** | `viewSpaceDepthDiff` vs `Ksep * Kfov * halfViewportWidth * depthThreshold` | `abs(HistoryClosestWorldDepth - WorldDepth)` vs `6 × pixelFootprint` |
| **输出** | `fDisocclusion` mask (float 0→1) | `bIsParallaxDisocclusion` (binary bool) |
| **运动发散** | `ComputeMotionDivergence`: 3×3邻域velocity方向一致性，输出 `1.0 - minConvergence` | `ComputeVelocityJacobian`: 2×2 Jacobian估计 |
| **深度发散** | `ComputeDepthDivergence`: 3×3邻域深度range/offset | `ComputeDepthError`: 邻域梯度 → parallax深度误差 |
| **关键文件** | `ffx_fsr2_depth_clip.h:28-78`, `ffx_fsr2_depth_clip.h:80-107` | `TSRClosestOccluder.ush:101-330`, `TSRDepthVelocityAnalysis.ush:41-67` |

### 4. MV 膨胀

| | FSR3 | TSR |
|---|---|---|
| **方法** | 9-sample depth extents → 最近深度邻居的 velocity | 3×3 深度+velocity 邻域 → 最近遮挡物 velocity |
| **深度选择** | `FindDepthExtents`: 9 邻域中找最近/最远深度，用最近深度像素的 velocity | `FindClosestDepthOffset`: 3×3 中找最近深度像素 |
| **HR/LR 分辨率** | 支持 LR MV 模式：MV 在低清分辨率，膨胀到渲染分辨率 | Vel 和 Depth 均在渲染分辨率 |
| **关键文件** | `ffx_fsr3upscaler_prepare_inputs.h:59-133` | `TSRDilateVelocity.usf:58-395`, `TSRDepthVelocityAnalysis.ush:269` |

### 5. 时间累积 / History Blending

| | FSR3 | TSR |
|---|---|---|
| **权重计算** | `fAlpha = UpsampledWeight / HistoryWeight`; `History = lerp(History, Upsampled, alpha)` | `PrevWeight = min(CurrentWeight * Multiplier, ClampedValidity)`; `CurrentWeight = InputPixelAlignement * ActualHistoryHisteresis` |
| **历史权重来源** | `fBaseAccumulation = params.fAccumulation` (mask)，受 velocity 调制: `min(accumulation, lerp(accumulation, 0.15, velocity/0.5))` | `PrevHistoryValidity` (多帧累积) + `ClampedPrevHistoryValidity = min(Validity, 1.0 - ClampingRejection * DecreaseMultiplier)` |
| **Lock 机制** | Lock 防止新样本被过早平均：`fLockContributionThisFrame = saturate(fLock - threshold) * (max - threshold)` | 无类似机制；通过 accumulated validity 自然衰减 |
| **关键文件** | `ffx_fsr3upscaler_accumulate.h:23-42`, `ffx_fsr3upscaler_accumulate.h:95-101` | `TSRUpdateHistory.usf:1104-1148` |

### 6. Luma Instability / 闪烁检测

| | FSR3 | TSR |
|---|---|---|
| **方法** | 4 帧亮度历史比较：符号一致性检测 | 多帧亮度追踪 + TotalVariation 计数 + MoireError |
| **检测逻辑** | `sign(diff[N-1]) == sign(diff[older])` → 不稳定方向确认 | `FlickeringLumaCS` pre-pass → `ComputeMoireError` → box扩大 |
| **应用** | `fLumaInstabilityFactor` 乘入 `fHistoryContribution`，降低历史权重 | MoireError 扩大 clamp box，放开历史贡献 |
| **关键文件** | `ffx_fsr3upscaler_luma_instability.h:29-67` | `TSRShadingAnalysis.ush:196-233`, `TSRMeasureFlickeringLuma.usf` |

### 7. Shading Change / 光照变化检测

| | FSR3 | TSR |
|---|---|---|
| **方法** | SPD mip-pyramid 下采样 → 金字塔层级间信号差 | MeasureRejection: 过滤后 input vs filtered history |
| **输入** | 3 级 mip 金字塔 | SMCS color space |
| **输出** | `fShadingChange` → 缩小 clamp box，降低历史权重 | `RejectionBlendFinal` / `RejectionClampBlend` → `DecreaseValidityMultiplier` |
| **关键文件** | `ffx_fsr3upscaler_shading_change.h:30-67` | `TSRShadingAnalysis.ush:446-555`, `TSRRejectShading.usf:589-590` |

### 8. Reactive Mask (反应性遮罩)

| | FSR3 | TSR |
|---|---|---|
| **来源** | 引擎/应用提供（材质标记），如半透明、粒子等 | 无显式 reactive mask |
| **膨胀** | `SampleDilatedReactiveMasks` — 已膨胀的 mask 直接采样 | N/A |
| **应用** | 缩小 clamp box、降低 accumulation | 通过 `DecreaseValidityMultiplier` 等效 |

---

## 三、核心差异速查

| 能力 | FSR3 | TSR |
|------|:--:|:--:|
| Color space | YCoCg + Hable Tonemap | GCS + SMCS (自研) |
| Box clamp shape | Elliptical (sphere in rectified space) | AABB (axis-aligned) |
| Disocclusion | Depth clip + motion divergence | Parallax disocclusion (depth-adaptive) |
| History storage | 1 frame | Rolling multi-frame Texture2DArray |
| Resurrection | 无 | 支持（多帧回退） |
| Velocity encoding | Float32 × 2 | 32-bit encoded + 18-bit hole-fill |
| Spatial AA | 无 | 独立模块 |
| Lens distortion | 不支持 | 支持 |
| Sharpening | RCAS (独立 pass) | 无 |
| HDR weight | Tonemap/InverseTonemap (Hable) | Karis HdrWeight (1/(Luma+4)) |
| Locking | Lock 状态机（persistent sample 保护） | 无 |
| Reprojection field | 无 | Vector + Jacobian + Boundary |
| Noise filtering | 无 | NoiseFiltering 软化 kernel |
| Moire detection | 无 (luma instability 部分覆盖) | FlickeringLuma + MoireError + TV |
| Depth confidence | Depth clip (连续值) | 无 per-pixel depth weight（仅二进制 disocclusion） |
| API 设计 | 跨平台 SDK (D3D12/Vulkan) | UE5 引擎内集成 |
| Jitter 驱动 | `hrUv + Jitter/RenderSize` | `ViewportUVToInputPPCo` |
| 质量层级 | 无（固定） | 4 级 CONFIG_SAMPLES_COUNT (5/6/9/TAAU) |
