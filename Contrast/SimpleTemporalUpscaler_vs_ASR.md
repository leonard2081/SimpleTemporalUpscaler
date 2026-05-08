# SimpleTemporalUpscaler vs ASR — 关键技术对比

> SimpleTemporalUpscaler: `C:\Work\Repo\GUSD\UE_553\Engine\Plugins\Runtime\SimpleTemporalUpscaler\`
> ASR: `C:\Work\Reference\accuracy-super-resolution-for-unreal-main\` (基于 AMD FSR2 v2.2.2)

---

## 一、架构对比

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| Pass 数量 | **1 个 Compute Shader** | 8—10 个 pass |
| 集成方式 | `ITemporalUpscaler::AddPasses` 内同 pass | 多个独立 RDG pass |
| 历史存储 | 1 帧 ColorHistory + DepthHistory (2 纹理) | 多帧 (Color + LumaHistory 4 帧 + Lock 状态) |
| 颜色空间 | RGB 线性 | YCoCg (Quality) / Tonemapped RGB (Perf) / 裸 RGB (Ultra) |
| 上采样核 | Hardware Bilinear | Lanczos 9-tap / 5-tap / 单采样 (分档) |
| 锐化 | 无 | RCAS 独立 pass |
| 代码行数 (shader) | ~220 行 (.usf) | ~6000+ 行 (FSR2 库 + ASR 新增) |

---

## 二、逐功能对比

### 1. MV 膨胀

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 搜索范围 | 3×3 | 9-sample depth extents |
| 选择策略 | **最近深度** (largest DeviceZ) | **最近深度** + 最远深度同时记录 |
| 深度用途 | 仅用于选择邻居 | 选择邻居 + 存储 FarthestDepth 给后续 pass |
| 膨胀后操作 | 直接用于 reprojection | Dilated → ReconstructPrevDepth scatter → Depth Clip 再用 |
| 代码位置 | `SimpleTemporalUpscalerBlend.usf:63-100` | `ffxm_fsr2_reconstruct_dilated_velocity_and_previous_depth.h` |

### 2. 相机 MV 回退 (ConvertVelocity)

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 何时触发 | `EncodedVelocity.x == 0` 且膨胀没找到 | 同样的条件 |
| 计算方式 | `View.ClipToPrevClip` 矩阵乘法 | `ComputeStaticVelocity` → `ClipToPrevClip` |
| 格式转换 | 无 (直接用 NDC velocity) | `Velocity * (-0.5, 0.5)` — FSR2 要求的负值+缩放 |
| 代码位置 | usf:180-186 | `ConvertVelocity.usf:26-34, 46-67` |
| 独立 pass | 无 (inline) | **单独 ConvertVelocity pass** |

### 3. 去抖动 (Dejitter)

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 方式 | 当前帧 bilinear 采样 `PixelPos + TemporalJitterPixels` | Lanczos `HrUv + Jitter/RenderSize` |
| 公式 | 直接偏移 + bilinear | jitter 驱动 Lanczos 权重计算 |
| 代码位置 | usf:102-119 | `ffxm_fsr2_common.h:289` |

### 4. 颜色 Clamp / History Rectification

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 方法 | 3×3 邻域 **AABB min/max** + 25% margin | RectificationBox **AABB** (center ± scaledVec) |
| Clamp 逻辑 | `clamp(history, min-margin, max+margin)` | `clamp(history, max(aabbMin, center-vec), min(aabbMax, center+vec))` |
| Box 构建 | 3×3 邻域 RGB | 2×2 Lanczos 加权邻域 (center + variance) |
| 累积保护 | `HistoryContribution = max(lumaInstability, lock) * accumulation * (1-disocclusion)` | `HistoryContribution = max(lumaInstability, lock) * reactive * accumulation` |
| 代码位置 | usf:180-195, 266 | `ffxm_fsr2_accumulate.h:104-140` |

### 5. 深度置信度 / Depth Clip

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 方法 | **深度差连续权重**: `1 - saturate((Δ-100)/500)` | **Depth Clip**: `1 - (RequiredSep/Δ)^power` + 平面评估 |
| 深度来源 | 当前像素 vs history 重投影位置 | 当前像素 vs Reconstructed Prev Depth (bilinear scatter) |
| Disocclusion 辅助 | 无 (仅 depth) | Reconstructed Prev Depth (当前 depth 反投影到上一帧) |
| Motion divergence | 无 | **移除**（FSR2 原版有，ASR 删了） |
| Depth divergence | 无 | **移除** |
| 代码位置 | usf:256-259 | `ffxm_fsr2_depth_clip.h:39-78` |

### 6. 运动自适应 / Motion Adaptive

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 方式 | 速度标量 → 连续降权: `1 - saturate((speed-min)/(max-min))` | Velocity 影响 accumulation 上限 + Lock 衰减 |
| 参数 | MinSpeed=0.5, MaxSpeed=8.0 (pixels/frame, output 分辨率) | `fHrVelocity / 20` 调制 `fBaseAccumulation` |
| Lock 机制 | **无** | Lock lifetime 衰减 + ThinFeatureConfidence |
| 代码位置 | usf:137-147 | `ffxm_fsr2_accumulate.h:161-171` |

### 7. History 混合

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 权重公式 | `HistoryWeight * DepthConfidence * MotionConfidence` | `fBaseAccumulation (受 velocity/reactive/depth-clip 三重调制)` |
| 最终混合 | `lerp(CurrentColor, PrevColor, EffectiveHistoryWeight)` | `lerp(HistoryColor, UpsampledColor, UpsampledWeight/HistoryWeight)` |
| 新样本处理 | `OutputColor = CurrentColor` (无history时) | `fHistoryColor = YCoCgToRGB(UpsampledColor)` 直接赋值为当前帧 |
| 累积上限 | `HistoryWeight` (固定 0.85) | `fMaxAccumulationLanczosWeight * existingSample * (1-reactive) * (1-depthClip)` |

### 8. Lock 机制

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| Lock 存在 | **无** | 有 — Lock lifetime + lock temporal luma |
| Lock 作用 | N/A | 保护稳定像素不被过早平均；控制 `fLockContributionThisFrame` |
| Lock 衰减 | N/A | 按 Lanczos weight 比例减少 lifetime |
| Thin feature | N/A | **ASR 新增**: 3×3 luma 一致性 → 细线/薄物体 lock 保护 |
| 代码位置 | N/A | `ffxm_fsr2_lock.h:26-118`, `ffxm_fsr2_accumulate.h:142-158` |

### 9. Luma Instability（亮度闪烁检测）

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 存在 | **无** | Quality 档有；Balanced/Perf/Ultra 关闭 |
| 方式 | N/A | 4 帧亮度历史 ± 邻域 box 大小因子 |
| 应用 | N/A | `fHistoryContribution *= reactive * lumaInstability` |
| 代码位置 | N/A | `ffxm_fsr2_accumulate.h:173-232` |

### 10. 锐化

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 锐化 | **无** | RCAS (Robust Contrast-Adaptive Sharpening) 独立 pass |
| 代码位置 | N/A | `ffxm_fsr2_rcas.h` + `RCASPass.usf` |

### 11. Reactive Mask

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 来源 | **无** | **新增** CreateReactiveMask pass (从 UE GBuffer 生成) |
| 检测内容 | N/A | 半透明物体、高粗糙度、反射、Lumen 镜面 |
| 应用 | N/A | 缩小 clamp box、降低 accumulation |
| 代码位置 | N/A | `CreateReactiveMask.usf:68-183` |

### 12. 额外 Pass

| | SimpleTemporalUpscaler | ASR |
|--|----------------------|-----|
| 曝光金字塔 | 无 | ComputeLuminancePyramid |
| 曝光拷贝 | 无 | CopyExposure |
| Velocity 格式转换 | 无 (inline) | ConvertVelocity (独立 pass) |
| Lock 预处理 | 无 | LockPass + PostProcessLockStatus |
| Depth 重构 | 无 | ReconstructPrevDepth |

---

## 三、SimpleTemporalUpscaler 缺少的关键技术（按影响排序）

| # | 技术 | 来源 | 影响 |
|---|------|------|------|
| 1 | **Lanczos 多 tap 上采样** | ASR/FSR | 当前 bilinear 边缘模糊，Lanczos 5-tap 可显著提升上采样锐度 |
| 2 | **Lock 机制** | ASR | 稳定像素被过早平均 → 画面柔化，Lock 可保护静态区域的时间累积 |
| 3 | **RCAS 锐化** | ASR/FSR | 独立锐化 pass，弥补时间累积的柔化 |
| 4 | **YCoCg 颜色空间** | ASR/FSR | RGB clamp 有跨通道色偏风险，YCoCg 解耦亮度和色度 |
| 5 | **Reactive Mask** | ASR | 半透明/高粗糙度物体标记，各自优化历史权重 |
| 6 | **Luma Instability** | ASR/FSR | 检测闪烁像素，在闪烁时降低历史贡献 |
| 7 | **Depth Clip (Reconstruct Prev Depth)** | ASR/FSR | 当前深度反投影到上一帧再做深度比较，比简单的 history-depth 采样更精确 |

### 四、SimpleTemporalUpscaler 独有的优势

| 优势 | 说明 |
|------|------|
| **单 pass 运行** | 全部逻辑在一个 CS 中，无中间纹理开销（对比 ASR 的 8+ pass） |
| **DepthConfidence 可选** | 已调整为 100/500 阈值后是有效的反遮挡保护 |
| **Motion Adaptive History Weight** | 比 ASR 的 velocity 调制更直观可调 |
| **Camera MV 回退** | `ClipToPrevClip` 直接计算，不另起 pass |
| **极简部署** | 1 个 USF + 1 个 C++ 文件，200 行 shader |

---

## 五、下一步建议（最小成本的提升）

```
优先级 1: Lanczos 5-tap 替代 bilinear       ← 改动：新增采样循环 + 权重计算
优先级 2: Lock 机制                         ← 改动：新增 lock 纹理 + lock lifetime 逻辑
优先级 3: RCAS 锐化                         ← 改动：新增 RCAS pass (从 FSR2 库移植)
优先级 4: YCoCg 颜色 clamp                  ← 改动：替换 RGB clamp 为 YCoCg clamp
```
