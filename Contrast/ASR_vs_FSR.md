# ASR vs FSR 对比分析

> ASR: `C:\Work\Reference\accuracy-super-resolution-for-unreal-main\` (based on AMD FSR2 v2.2.2)
> FSR3: `C:\Work\Reference\FidelityFX-SDK-main\Kits\FidelityFX\upscalers\fsr3\`

---

## 一、基础信息

| | ASR | FSR2 (baseline) | FSR3 |
|--|-----|-----|------|
| 版本 | v25.06 | v2.2.2 | v3.1.5 |
| 目标平台 | 移动端 (Vulkan SM5 / GLES 3.2) | 桌面 (D3D12 / Vulkan) | 桌面 + 主机 |
| 代码量 | 37 个 shader 文件 (含 FSR2 库) | ~30 文件 | 33 个 GPU header + 19 个 HLSL |
| UE 集成方式 | `ITemporalUpscaler` 接口 | 独立 SDK | `ITemporalUpscaler` 接口 |
| 许可证 | MIT | MIT | MIT |

---

## 二、Pass 对比

| Pass | FSR2 原始 | FSR3 | ASR (改动) |
|------|:--:|:--:|:--:|
| Depth Clip | ✓ | ✓ (增强) | ✓ (简化) |
| Compute Luminance Pyramid | ✓ | ✓ (增强) | ✓ |
| Reconstruct Prev Depth | ✓ | 合并到 PrepareInputs | ✓ |
| Dilate Velocity | 独立 pass | 独立 pass | **合并**到 ReconstructPrevDepth |
| Lock | ✓ | ✓ | ✓ (增加 ThinFeatureConfidence) |
| Accumulate | ✓ | ✓ (椭圆 clamp) | ✓ (AABB clamp，更简单) |
| RCAS | ✓ | ✓ | ✓ |
| Reactive Mask | 应用侧提供 | 应用侧提供 | **新增** CreateReactiveMask pass |
| Velocity Conversion | N/A (SDK 自带格式) | N/A | **新增** ConvertVelocity pass |
| Copy Exposure | 有 | 有 | ✓ |

---

## 三、核心算法改动（移动端优化）

### 1. Ultra Performance 质量档位（ASR 新增）

**`ffxm_core_gpu_common.h:33-55`** — 四档质量体系：

| 档位 | 9-tap Lanczos | Deringing | YCoCg | Luma Instability | Separate Reactive | Depth Clip |
|------|:--:|:--:|:--:|:--:|:--:|:--:|
| Quality | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Balanced | ✗ → 5-tap | ✗ | ✓ | ✗ | ✗ | ✓ |
| Performance | ✗ → 5-tap | ✗ | 跳过 YCoCg，直用 Tonemapped RGB | ✗ | ✗ | ✓ |
| **Ultra Performance** | ✗ → 单采样点 | ✗ | ✗（跳过曝光处理） | ✗ | ✗ | ✗（复用 reactive mask 通道） |

**Ultra Performance 相比 FSR2 原始最低档的额外裁剪：**
- 深度 clip 完全跳过（`ffxm_fsr2_depth_clip.h:266-270`：`fDepthClipFactor` 从 dilated reactive mask 读 0）
- 跳过曝光处理（`PrepareRgb/UnprepareRgb` 直接返回原值）
- 输出结构最小化（`AccumulateOutputs` 仅 `fColorAndWeight + fLockStatus`）
- 不产生菱形 instability 分析

### 2. Lock 机制的移动端简化

**`ffxm_fsr2_lock.h:26-118`** — 新增 `ComputeThinFeatureConfidence`：

FSR2 原版: Lock 仅基于 temporal luma 变化
ASR: 增加 **3×3 邻域 luma 一致性检查**（thin feature confidence）
- 检测细线、薄物体：邻域内至少 1 个 2×2 quad 中 4 个像素 luma 均相似（差值比 < 1.05）→ 该像素适合 lock
- 避开细线边缘（lock 不稳定），聚焦 flat shading 区域

### 3. RectifyHistory（AABB clamp 替代 FSR3 的椭圆 clamp）

**`ffxm_fsr2_accumulate.h:104-140`** — ASR 的 clamp 方式：

```
ASR:  boxMin = max(aabbMin, center - scaledVec)
      boxMax = min(aabbMax, center + scaledVec)
      clamp(history, boxMin, boxMax)  ← 简单 AABB

FSR3: normalize(transformedHistory) * scaledVec + center  ← 椭圆球面 clamp
```

ASR 的 AABB clamp 计算量更小（1 次 clamp vs FSR3 的 `normalize + scale + offset`），适合移动端 ALU 敏感场景。

### 4. Depth Clip 简化

**`ffxm_fsr2_depth_clip.h:39-78`** — ASR 移除了 FSR3 中的：

| FSR3 有，ASR 移除了 | 说明 |
|---------------------|------|
| `ComputeMotionDivergence` | 3×3 邻域 velocity 方向一致性 |
| `ComputeDepthDivergence` | 3×3 邻域深度范围/发散 |
| `ComputeTemporalMotionDivergence` | 当前 vs 上一帧 velocity 差异 |
| `PreProcessReactiveMasks` 3×3 颜色加权膨胀 | ASR 改为更简单的直接采样 |

ASR 仅保留核心的 `ComputeDepthClip`（深度比较）+ 简化版 mask 膨胀。

### 5. Lanczos Kernel 降级

**`ffxm_fsr2_common.h:51-55`** 和 **`ffxm_fsr2_upsample.h:129-171`**：

```
Quality:      9-tap Lanczos (FSR2 标准)
Ultra Perf:   单采样点（saturate 后的单点 bilinear 等效）
其他:         5-tap Lanczos
```

### 6. YCoCg → Tonemapped RGB 直达（Performance 模式）

**`ffxm_fsr2_accumulate.h:63-77`** 和 **`ffxm_fsr2_common.h:364-369`**：

```
Quality/Balanced:  RGB → YCoCg → Tonemap → clamp → InverseTonemap → YCoCgToRGB
Performance:       Tonemap → clamp → InverseTonemap（跳过 YCoCg）
Ultra Perf:        完全不处理（直接线性 RGB 混合）
```

### 7. 新增 ConvertVelocity pass

**`ConvertVelocity.usf:46-67`** — UE velocity buffer 格式桥接：

```hlsl
if (EncodedVelocity.x > 0.0)
    Velocity = DecodeVelocity(EncodedVelocity).xy;
else
    Velocity = ComputeStaticVelocity(ScreenPos, Depth).xy; // ClipToPrevClip 回退

return Velocity * (-0.5, 0.5); // FSR2 要求的负值 + 缩放
```

FSR2 SDK 期望 velocity 为 `NDC_prev - NDC_cur` 且做了缩放。UE 的 velocity 格式不同，此 pass 做格式转换 + camera 回退填补。

### 8. 新增 CreateReactiveMask pass

**`CreateReactiveMask.usf:68-183`** — 从 UE GBuffer 生成 reactive mask：

```
输入: GBuffer B (roughness), GBuffer D (shading model), SceneColor, ReflectionTexture, LumenSpecular, InputDepth
检测:
  1. 半透明物体 (GBuffer ShadingModel → reactive)
  2. 高粗糙度表面 (roughness > threshold → reactive)
  3. 反射强度 (reflection luma > bias → reactive)
  4. Lumen 镜面 (gi_lumen_specular 存在时降低 reactive)
  5. 预 Alpha 差异 (SceneColor vs SceneColorPreAlpha → 半透明 mask)
输出: ReactiveMask + CompositeMask（双通道 R32F）
```

---

## 四、总结对比表

| 优化/裁剪维度 | FSR2 原始 | FSR3 | ASR |
|------------|:--:|:--:|:--:|
| **最高质量 Lanczos taps** | 9 | 5 (Catmull-Rom) | 9 (同 FSR2) |
| **最低质量采样** | 5-tap | 5 (CONFIG_SAMPLES_COUNT=5) | **单采样点** |
| **Deringing 去除** | 有 | 无独立模块 | Quality 档保留 |
| **YCoCg 颜色空间** | ✓ | N/A (GCS/SMCS) | Quality 档保留 |
| **Ultra Performance** | 无 | 无 | 新增 |
| **Elliptical clamp** | N/A | ✓ | 无（用简单 AABB） |
| **Motion/Depth Divergence** | ✓ (FSR2 版) | ✓ (增强) | **移除** |
| **Disocclusion 类型** | Depth Clip (连续值) | Parallax (二值) | Depth Clip (连续值, 简化) |
| **Hole filling** | 无 | ✓ (18-bit 极坐标) | 无 |
| **Reprojection field** | 无 | ✓ (3 slices) | 无 |
| **History Resurrection** | 无 | ✓ | 无 |
| **Lock 机制** | Luma stability | N/A (accumulated validity) | Luma + ThinFeature + new lock logic |
| **FP16 倾向** | 部分 | ✓ (核心路径) | ✓（广泛使用 `FFXM_MIN16_*`）|
| **Lanczos 5-tap 宏** | N/A | N/A | `FFXM_SHADER_QUALITY_OPT_UPSCALING_LANCZOS_5TAP` |
| **Reactive Mask** | 应用侧提供 | 应用侧提供 | **新增 CreateReactiveMask pass** |
| **Velocity 桥接** | SDK 自己处理 | SDK 自己处理 | **新增 ConvertVelocity pass** |
| **GLES 3.2** | 不支持 | 不支持 | 支持（有限） |
| **RHI 支持** | Vulkan/D3D12 | Vulkan/D3D12 | Vulkan SM5/GLES 3.2 |

---

## 五、两类裁剪总结

### A 类：算法级裁剪（永远关闭）

| 裁剪项 | 原因 |
|--------|------|
| Motion/Depth Divergence | 3×3 邻域 9 次采样，移动端 ALU 太重 |
| Elli 试 clamp → AABB clamp | 省掉 `normalize + scale + offset` |
| 移除 TSR 类功能（hole filling / resurrection / reprojection field） | 桌面级复杂度，移动端不需要 |

### B 类：质量档位裁剪（按需关闭）

| 裁剪项 | 关闭条件 |
|--------|---------|
| 9-tap → 5-tap → 单采样 Lanczos | Balanced / Performance / Ultra |
| YCoCg → Tonemapped RGB → 无处理 | Performance / Ultra |
| Luma Instability 分析 | Balanced / Performance / Ultra |
| Deringing | Balanced / Performance / Ultra |
| Depth Clip | Ultra Performance |
| Separate Temporal Reactive | Balanced / Performance / Ultra |
