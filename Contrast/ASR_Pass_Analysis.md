# Arm ASR Pass 分析梳理

本文档梳理当前 `ArmASR` 插件中 ASR 算法的 pass、核心算法逻辑，以及 shader 输入来源与输出去向。

入口：`FArmASRTemporalUpscaler::AddPasses()`  
主要调度文件：`Source/ArmASR/Private/ArmASR.cpp`  
Shader wrapper：`Source/ArmASR/Private/Shaders/*.h`  
核心算法：`Shaders/Private/fsr2/*.hlsl`

## 总体流程

```text
SceneColor / SceneDepth / SceneVelocity / View / PrevHistory
  -> Create Reactive Mask       [可选，非 Ultra]
  -> ConvertVelocity
  -> Compute Luminance Pyramid  [非 Ultra]
  -> CopyExposure               [不用 ASR AutoExposure 或 Ultra]
  -> Reconstruct Previous Depth
  -> Depth Clip
  -> Lock
  -> Accumulate
  -> RCAS                       [Sharpness > 0]
  -> Outputs.FullRes.Texture
```

## 公共输入与历史资源

### 当前帧输入

| 资源 | 来源 | 说明 |
|---|---|---|
| `SceneColor` | `Inputs.SceneColor.Texture` | 当前帧低分辨率、带 jitter 的颜色。 |
| `SceneDepth` | `Inputs.SceneDepth.Texture` | 当前帧深度，ASR 按 inverted depth 使用。 |
| `VelocityTexture` | `Inputs.SceneVelocity.Texture` | UE 输出的 scene velocity。 |
| `View` | `FSceneView` / `ViewUniformBuffer` | 矩阵、viewport、曝光、jitter 等。 |
| `SceneTextures` | `ArmASRInfo.PostInputs.SceneTextures` | GBuffer 等，用于 Reactive Mask。 |
| `LumenReflections` | `ArmASRInfo.LumenReflections` | Lumen reflection history。 |

### 历史输入

| 历史资源 | 来源 | 本帧用途 |
|---|---|---|
| `PrevUpscaledColour` | 上一帧 Accumulate RT0 | 颜色历史重投影。 |
| `PrevInternalReactive` | 上一帧 Balanced/Performance Accumulate RT1 | temporal reactive history。 |
| `PrevLumaHistory` | 上一帧 Quality Accumulate RT2 | luma / shading change history。 |
| `PrevDilatedMotionVectors` | 上一帧 RPD RT1 | Depth Clip 比较上一帧 dilated MV。 |
| `PrevDilatedDepthMotionVectorsInputLuma` | 上一帧 Ultra RPD RT0 | Ultra Performance 合并格式历史。 |
| `PrevLockStatus` | 上一帧 Accumulate lock status RT | 延续 lock 状态。 |
| `PrevPreExposure` | 上一帧 `fPreExposure` | 曝光连续性修正。 |

如果 camera cut 或历史无效，上述历史资源会替换为 black dummy，并重置 `FRAME_INDEX`。

### 当前帧复用 / 中间资源

| 资源 | 来源 | 本帧用途 |
|---|---|---|
| `NewLock` | 优先复用 `PrevHistory->NewLock` 的纹理对象；本帧开始会 clear | 由当前帧 Lock pass 写入 new lock mask，随后 Accumulate 作为 `r_new_locks` 读取；它不是算法意义上的历史输入。 |

---

## 1. Create Reactive Mask Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRCreateReactiveMask.h`
- Shader：`Shaders/Private/CreateReactiveMask.usf`
- 运行条件：非 Ultra，`r.ArmASR.CreateReactiveMask != 0`，且 `SceneTextures` 有效。

### 核心逻辑

根据当前帧 GBuffer、透明贡献、反射/Lumen specular、roughness 和材质 custom data 生成 reactive mask / composite mask，用于降低透明、反射或特殊材质区域对历史颜色的依赖。

主要步骤：

1. 解码 `GBufferB/GBufferD` 得到 roughness、shading model、custom data。
2. 比较 `SceneColor` 与 `SceneColorPreAlpha` 估计 translucency。
3. 根据深度计算世界空间距离，对 roughness / translucency 做距离衰减。
4. 使用 reflection texture 或 Lumen specular 计算反射贡献。
5. 特殊 reactive shading model 可强制写入 reactive 值。
6. 输出 `CompositeMask = Output.x`，`ReactiveMask = max(ForceReactive, Output.y)`。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `GBufferB` | `PostInputs.SceneTextures->GBufferBTexture` | roughness / shading model 解码。 |
| `GBufferD` | `PostInputs.SceneTextures->GBufferDTexture` | custom data / shading model 解码。 |
| `ReflectionTexture` | `ArmASRInfo.ReflectionTexture` 或 black dummy | SSR/反射贡献。 |
| `InputDepth` | `SceneDepth` | 当前深度。 |
| `SceneColor` | `SceneColor` | 当前最终场景色。 |
| `SceneColorPreAlpha` | `ArmASRInfo.SceneColorPreAlpha` 或 `SceneColor` | alpha 前颜色，用于估计透明贡献。 |
| `LumenSpecular` | `ArmASRInfo.LumenReflections` 或 black dummy | Lumen specular 历史。 |
| `InputVelocity` | 原始 `VelocityTexture` | Lumen specular 重投影。 |
| `View` | `View.ViewUniformBuffer` | 矩阵、viewport、深度转换。 |
| Reactive CVars | `r.ArmASR.ReactiveMask*` | 控制反射、透明、roughness 权重。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `ReactiveMaskTexture` | `PF_R8`，GLES 为 `PF_R32_FLOAT`，input res | `Depth Clip.r_reactive_mask` | 标记需要减少历史依赖的区域。 |
| `CompositeMaskTexture` | `PF_R8`，GLES 为 `PF_R32_FLOAT`，input res | `Depth Clip.r_transparency_and_composition_mask` | 组合透明、反射、roughness 等贡献。 |

---

## 2. ConvertVelocity Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRConvertVelocity.h`
- Shader：`Shaders/Private/ConvertVelocity.usf`

### 核心逻辑

将 UE velocity 转换为 ASR/FSR2 需要的 motion vector 格式。若 velocity texture 没有有效速度，则用深度和 `View.ClipToPrevClip` 为静态物体重建 velocity。最终乘以 `(-0.5, 0.5)` 适配 FSR2 坐标约定。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `InputDepth` | `SceneDepth` | 静态 velocity 重建。 |
| `InputVelocity` | `VelocityTexture` | UE 原始 motion vector。 |
| `View` | `View.ViewUniformBuffer` | `ClipToPrevClip` 和 viewport 信息。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `MotionVectorTextureNew` | `PF_G16R16F`，quantized input res | `RPD`、`Depth Clip`、`Accumulate` | ASR 标准 motion vector，后续时域重投影的基础。 |

---

## 3. Compute Luminance Pyramid Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRComputeLuminancePyramid.h`
- Shader：`Shaders/Private/ComputeLuminancePyramidPass.usf`
- Core：`Shaders/Private/fsr2/ffxm_fsr2_compute_luminance_pyramid_pass.hlsl`
- 运行条件：非 Ultra。

### 核心逻辑

调用 FSR2 `ComputeAutoExposure()`，基于当前颜色生成亮度金字塔，同时可计算 ASR 自动曝光。该 pass 使用 SPD 风格 downsample 和 `rw_spd_global_atomic` 做跨 workgroup 归约。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `r_input_color_jittered` | `SceneColor` | 当前低分辨率颜色。 |
| `cbArmASR` | `SetCommonParameters()` | 尺寸、jitter、曝光等。 |
| `cbArmASRSPD` | `SpdConfig` | workgroup / mip 参数。 |
| `rw_spd_global_atomic` | `ArmASRInfo.Atomic`，首次创建后复用 | SPD 归约辅助。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `MipShadingChangeTexture` | `PF_R16F`，GLES 为 `PF_R32_FLOAT`，mipped | `Accumulate.r_imgMips` | 亮度 mip，用于 shading change detection。 |
| `rw_img_mip_5` | 同一 mip texture 的 mip 5 | 算法内部 | 参与 luminance pyramid / exposure 计算。 |
| `AutoExposureTexture` | `PF_G32R32F`，GLES 为 `PF_FloatRGBA`，1x1 | 后续 `ExposureTexture` | 当 `r.ArmASR.AutoExposure=1` 时作为曝光输入。 |
| `rw_spd_global_atomic` | `PF_R32_UINT`，1x1 | 后续帧复用 | 同步/计数辅助，不是画面数据。 |

---

## 4. CopyExposure Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRCopyExposure.h`
- Shader：`Shaders/Private/CopyExposure.usf`
- 运行条件：`r.ArmASR.AutoExposure=0` 或 Ultra。

### 核心逻辑

从 UE `EyeAdaptationBuffer` 读取曝光，写入 1x1 `ExposureTexture`。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `EyeAdaptationBuffer` | `GetEyeAdaptationBuffer(GraphBuilder, View)` | UE eye adaptation / exposure。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `ExposureTexture` | `PF_A32B32G32R32F`，1x1 | `RPD`、`Depth Clip`、`Accumulate`、`RCAS` | 当前帧曝光。 |

---

## 5. Reconstruct Previous Depth Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRReconstructPrevDepth.h`
- Shader：`Shaders/Private/ReconstructPrevDepthPass.usf`
- Core：`Shaders/Private/fsr2/ffxm_fsr2_reconstruct_previous_depth_pass_fs.hlsl`

### 核心逻辑

调用 `ReconstructAndDilate(uPixelCoord)`：用当前深度和 motion vector 重建前一帧最近深度，并在邻域中扩张 depth / motion vector / luma，提升运动边界和薄几何体处的稳定性。Ultra 模式将 depth、motion vector、luma 合并为一个 RT。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `r_input_motion_vectors` | `MotionVectorTextureNew` | ConvertVelocity 输出。 |
| `r_input_depth` | `SceneDepth` | 当前深度。 |
| `r_input_color_jittered` | `SceneColor` | 当前颜色，用于 luma。 |
| `r_input_exposure` | `ExposureTexture` | 曝光校正，Ultra permutation 不绑定。 |
| `cbArmASR` | common params | 尺寸、jitter、depth transform 等。 |

### 输出

#### 非 Ultra

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `ReconstructedPreviousNearestDepthTexture` | `PF_R32_UINT`，input res | `Depth Clip` | 重建的前帧最近深度，用于遮挡判断。 |
| `DilatedDepthTexture` | `PF_R32_FLOAT`，input res | `Depth Clip` | 扩张深度，减少边界不稳定。 |
| `DilatedVelocityTexture` | `PF_G16R16F`，input res | `Depth Clip`、`Accumulate`、下一帧 history | 扩张 motion vector，时域重投影主输入。 |
| `LockLumaTexture` | `PF_R16F`，input res | `Lock` | 当前帧 lock 检测亮度。 |

#### Ultra

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `ReconstructedPreviousNearestDepthTexture` | `PF_R32_UINT`，input res | `Depth Clip` | 同上。 |
| `DilatedDepthVelocityLumaTexture` | `PF_FloatRGBA`，input res | `Depth Clip`、`Lock`、`Accumulate`、下一帧 history | `.x=depth`，`.yz=motion vector`，`.w=luma`，用于减少带宽与 RT 数量。 |

---

## 6. Depth Clip Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRDepthClip.h`
- Shader：`Shaders/Private/DepthClipPass.usf`
- Core：`Shaders/Private/fsr2/ffxm_fsr2_depth_clip_pass_fs.hlsl`

### 核心逻辑

调用 `DepthClip(uPixelCoord)`：比较 reconstructed previous nearest depth、当前扩张深度、当前/历史 motion vector，并结合 reactive mask 判断历史是否可信。非 Ultra 下还会输出 prepared / tonemapped input color。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `r_reconstructed_previous_nearest_depth` | RPD UAV | 前帧最近深度重建结果。 |
| `r_dilated_motion_vectors` | RPD RT1，非 Ultra | 当前帧扩张 MV。 |
| `r_dilatedDepth` | RPD RT0，非 Ultra | 当前帧扩张深度。 |
| `r_previous_dilated_motion_vectors` | `PrevHistory->DilatedMotionVectors` | 上一帧扩张 MV。 |
| `r_dilated_depth_motion_vectors_input_luma` | RPD RT0，Ultra | Ultra 合并数据。 |
| `r_prev_dilated_depth_motion_vectors_input_luma` | `PrevHistory->DilatedDepthMotionVectorsInputLuma` | 上一帧 Ultra 合并数据。 |
| `r_reactive_mask` | Create Reactive Mask 或 black dummy | reactive mask。 |
| `r_transparency_and_composition_mask` | Create Reactive Mask 或 black dummy | composite mask。 |
| `r_input_motion_vectors` | `MotionVectorTextureNew` | 标准 motion vector。 |
| `r_input_color_jittered` | `SceneColor` | 当前颜色。 |
| `r_input_depth` | `SceneDepth` | 当前深度。 |
| `r_input_exposure` | `ExposureTexture` | 非 Ultra 下用于 prepared color。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `DilatedReactiveMaskTexture` | `PF_R8G8`，input res | `Accumulate.r_dilated_reactive_masks` | 两通道 reactive 信息，调节历史累积强度。 |
| `PreparedInputColorTexture` | `PF_FloatRGBA`，input res，非 Ultra | `Accumulate.r_prepared_input_color` | 预处理后的当前帧颜色。 |
| Ultra 无 prepared color | - | `Accumulate` 直接读 `SceneColor` | Ultra 省带宽路径。 |

### `DilatedReactiveMaskTexture` 细节

`DilatedReactiveMaskTexture` 不是简单把输入的 reactive mask / composite mask 透传成两个通道。Depth Clip 会对输入 mask 做邻域扩张、颜色相似性加权，并合并 motion divergence / temporal motion difference 后再输出。

非 Ultra 路径中，输入是两张单通道 mask：

| 输入 | 来源 | 说明 |
|---|---|---|
| `r_reactive_mask` | `ReactiveMaskTexture` | 原始 reactive mask。 |
| `r_transparency_and_composition_mask` | `CompositeMaskTexture` | transparency / composition / reflection 相关 mask。 |

`PreProcessReactiveMasks()` 的处理逻辑可以概括为：

1. 初始化输出因子：`fReactiveFactor = float2(0, fMotionDivergence)`，因此 `.y` 从一开始就包含 motion divergence。
2. 对 `r_reactive_mask` 做 3x3 邻域 gather，得到 9 个 reactive samples。
3. 对 `r_transparency_and_composition_mask` 做 3x3 邻域 gather，得到 9 个 composition samples。
4. 如果邻域 mask 有非零值，再读取 3x3 input color，用中心颜色和邻域颜色计算相似性。
5. 根据颜色相似性调整邻域 mask 强度：颜色越不相似，mask 样本会被更强地压低，避免跨明显颜色边界过度扩张。
6. 对加权后的邻域 reactive / composition mask 取最大值，得到输出双通道。

因此非 Ultra 下更接近：

```text
DilatedReactiveMaskTexture.x = max_3x3(weighted reactive mask)
DilatedReactiveMaskTexture.y = max(motion divergence / temporal motion difference,
                                   max_3x3(weighted composition mask))
```

后续 Accumulate 读取后拆成：

| 通道 | Accumulate 变量 | 作用 |
|---|---|---|
| `.x` | `params.fDilatedReactiveFactor` | 当前帧 reactive factor，越高越减少历史颜色权重。 |
| `.y` | `params.fAccumulationMask` | accumulation / composition mask，影响 rectification、luma history 采样和 lock 生命周期。 |

另外，Depth Clip 本身会计算 `fDepthClip`。非 Ultra 下它不写入 `DilatedReactiveMaskTexture.x`，而是写入 `PreparedInputColorTexture.a`，供 Accumulate 的 `SampleDepthClip()` 读取。Ultra Performance 路径不同，`DilatedReactiveMaskTexture.x` 会用于保存 `fDepthClip`，`.y` 主要来自 motion divergence / temporal motion difference。

---

## 7. Lock Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRLock.h`
- Shader：`Shaders/Private/LockPass.usf`
- Core：`Shaders/Private/fsr2/ffxm_fsr2_lock_pass.hlsl`

### 核心逻辑

调用 `ComputeLock(uDispatchThreadId)`。根据 luma 检测并生成细节锁定 mask，用于减少高频细节、细线和边缘在 temporal jitter 下的闪烁。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `r_lock_input_luma` | RPD RT2，非 Ultra | lock 检测亮度。 |
| `r_dilated_depth_motion_vectors_input_luma` | RPD RT0，Ultra | Ultra 合并纹理中的 luma。 |
| `rw_new_locks` | `NewLock` | 当前帧清空后作为 UAV 写入。 |
| `cbArmASR` | common params | 输出尺寸、jitter、frame index 等。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `NewLock` | `PF_R8`，GLES 为 `PF_R32_FLOAT`，output res | `Accumulate.r_new_locks`，并提取到 `NewHistory->NewLock` | 当前帧新 lock mask，用于更新 lock status。 |

---

## 8. Accumulate Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRAccumulate.h`
- Shader：`Shaders/Private/AccumulatePass.usf`
- Core：`Shaders/Private/fsr2/ffxm_fsr2_accumulate_pass_fs.hlsl`

### 核心逻辑

调用 `Accumulate(uPixelCoord)`。这是主超分 pass，完成 upsample、历史重投影、历史可信度判断、当前帧与历史帧混合、lock status 更新，并输出新的历史资源和最终颜色。

主要依据：motion vector / dilated motion vector、depth clip 结果、reactive masks、lock status / new locks、luma history 或 temporal reactive history、shading change luminance mip、previous upscaled color。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `r_input_exposure` / `r_auto_exposure` | `ExposureTexture` | 当前曝光。 |
| `r_dilated_reactive_masks` | Depth Clip RT0 | dilated reactive mask。 |
| `r_dilated_motion_vectors` | RPD RT1，非 Ultra | 当前扩张 MV。 |
| `r_dilated_depth_motion_vectors_input_luma` | RPD RT0，Ultra | Ultra 合并数据。 |
| `r_input_motion_vectors` | `MotionVectorTextureNew` | 标准 MV。 |
| `r_internal_upscaled_color` | `PrevHistory->UpscaledColour` | 上一帧 upscaled color。 |
| `r_input_color_jittered` | `SceneColor`，Ultra | 当前颜色。 |
| `r_prepared_input_color` | Depth Clip RT1，非 Ultra | prepared current color。 |
| `r_lock_status` | `PrevHistory->LockStatus` | 上一帧 lock status。 |
| `r_imgMips` | Compute Luminance 输出，非 Ultra | shading change 检测。 |
| `r_luma_history` | `PrevHistory->LumaHistory`，Quality | 亮度历史。 |
| `r_internal_temporal_reactive` | `PrevHistory->InternalReactive`，Balanced/Performance | temporal reactive 历史。 |
| `r_new_locks` | Lock 输出 `NewLock` | 当前帧 lock mask。 |

### 输出

#### Quality

| RT | 输出 | 格式 | 去向 | 说明 |
|---|---|---|---|---|
| RT0 | `InternalUpscaledColorOutputTexture` | `PF_FloatRGBA` | `NewHistory->UpscaledColour`，RCAS 输入 | 内部超分颜色 + 权重/附加信息。 |
| RT1 | `LockStatusOutputTexture` | `PF_G16R16F` | `NewHistory->LockStatus` | 更新后的 lock 状态。 |
| RT2 | `LumaHistoryOutputTexture` | `PF_R8G8B8A8` | `NewHistory->LumaHistory` | 下一帧亮度历史。 |
| RT3 | `ArmASROutputSceneColor` | output format | 最终输出，RCAS 关闭时写 | 全分辨率颜色。 |

#### Balanced / Performance

| RT | 输出 | 格式 | 去向 | 说明 |
|---|---|---|---|---|
| RT0 | `InternalUpscaledColorOutputTexture` | `PF_FloatR11G11B10` | `NewHistory->UpscaledColour`，RCAS 输入 | 省带宽内部超分颜色。 |
| RT1 | `InternalReactiveOutput` | `PF_R16F` | `NewHistory->InternalReactive` | temporal reactive history。 |
| RT2 | `LockStatusOutputTexture` | `PF_G16R16F` | `NewHistory->LockStatus` | 更新后的 lock 状态。 |
| RT3 | `ArmASROutputSceneColor` | output format | 最终输出，RCAS 关闭时写 | 全分辨率颜色。 |

#### Ultra Performance

| RT | 输出 | 格式 | 去向 | 说明 |
|---|---|---|---|---|
| RT0 | `InternalUpscaledColorOutputTexture` | `PF_FloatR11G11B10` | `NewHistory->UpscaledColour`，RCAS 输入 | 内部超分颜色。 |
| RT1 | `LockStatusOutputTexture` | `PF_G16R16F` | `NewHistory->LockStatus` | 更新后的 lock 状态。 |
| RT2 | `ArmASROutputSceneColor` | output format | 最终输出，RCAS 关闭时写 | 全分辨率颜色。 |

### 关键输出说明

- `InternalUpscaledColorOutputTexture`：最重要的颜色历史。下一帧会被重投影并参与当前帧混合。
- `LockStatusOutputTexture`：跨帧细节锁定状态，用于减少 jitter 导致的闪烁。
- `LumaHistoryOutputTexture`：Quality 模式保存亮度历史，用于检测 shading change 和历史稳定性。
- `InternalReactiveOutput`：Balanced/Performance 模式使用的 reactive 历史，成本低于完整 luma history。
- `ArmASROutputSceneColor`：最终输出给 UE renderer 的 full-res color。若启用 RCAS，则由 RCAS 写入。

### 最终混色影响因素

`Accumulate` 的最终混色可以简化理解为：

```text
history = Reproject(PrevUpscaledColour, motionVector)
history = ExposureCorrect(history)
history = Rectify(history, currentNeighborhood, depthClip, reactive, lock, lumaInstability)

current = Upsample(CurrentFrameColor, jitter)

alpha = currentWeight / (historyWeight + currentWeight)
final = lerp(history, current, alpha)
```

其中 `alpha` 越大，当前帧颜色占比越高，响应更快、拖影更少，但可能更闪；`alpha` 越小，历史颜色占比越高，画面更稳，但更容易 ghosting / smearing。

| 因素 | 来源 | 如何影响混色 |
|---|---|---|
| 历史有效性 / motion vector | `GetMotionVector()`、`ComputeReprojectedUVs()` | motion vector 决定上一帧历史颜色采样位置。若重投影 UV 出屏幕、camera cut 或 reset frame，则视为 new sample，基本直接使用当前帧颜色。 |
| `PrevUpscaledColour` | 上一帧 Accumulate RT0 | 提供历史颜色 `fHistoryColor`。历史正确时提升稳定性；历史错误时会被 depth clip、reactive、rectification 等机制降权或修正。 |
| 当前帧 upsampled color / weight | `ComputeUpsampledColorAndWeight()` | `.xyz` 是当前帧重建颜色，`.w` 是当前帧样本权重。`.w` 越大，最终 `alpha` 越大，当前帧颜色占比越高。 |
| 当前 reactive factor | `DilatedReactiveMaskTexture.x` | 来自 `ReactiveMask / CompositeMask -> DepthClip`。值越大，说明当前区域越不适合依赖历史，会降低历史累积权重。 |
| 历史 temporal reactive | Quality 中来自 `PrevUpscaledColour.a`；Balanced/Performance 中来自 `PrevInternalReactive` | 按 motion vector 重投影采样后得到 `fTemporalReactiveFactor`，与当前 reactive 取 `max`。即使当前帧 reactive 变弱，上一帧的不稳定性也会延续，继续减少历史依赖。 |
| `fThisFrameReactiveFactor` | `max(params.fDilatedReactiveFactor, fTemporalReactiveFactor)` | 直接进入历史累积权重：`historyWeight *= (1 - fThisFrameReactiveFactor)`。值越大，历史越少，当前帧越多。 |
| depth clip factor | `DepthClip` 输出；非 Ultra 下存于 `PreparedInputColor.a` | 反映遮挡/反遮挡/深度不连续。值越大，历史越不可信，历史权重乘以 `(1 - depthClip)`，减少 ghosting。 |
| accumulation mask | `DilatedReactiveMaskTexture.y` | 来自 composition / transparency mask、motion divergence 等。它会影响 rectification、luma history 采样和 lock 生命周期，使透明、反射、运动发散区域更保守地使用历史。 |
| motion speed / `fHrVelocity` | motion vector 乘以输出尺寸 | 速度越高，基础历史累积越低；同时会影响 rectification box 和下一帧 temporal reactive，减少运动拖影。 |
| `bInMotionLastFrame` | temporal reactive 的符号，历史中负值表示上一帧高速运动 | 若上一帧处于高速运动，即使当前速度降低，也会暂时限制历史累积，减少运动后残留拖影。 |
| `NewLock` / `PrevLockStatus` | Lock pass 输出与上一帧 lock status | `NewLock` 标记当前帧细线/高频结构，`PrevLockStatus` 保存 lock lifetime 与 temporal luma。有效 lock 会提高历史细节保留，减少细线闪烁；reactive、depth clip、accumulation mask 高时会削弱或杀掉 lock，避免锁住错误历史。 |
| `PrevLumaHistory` / luma instability | Quality 模式 Accumulate RT2 | 保存最近 4 帧量化亮度历史。它不直接作为混色权重，而是影响 history rectification，判断历史颜色是否应被当前邻域颜色范围裁剪。 |
| rectification clipping box | 当前帧邻域颜色，由 `ComputeUpsampledColorAndWeight()` 构造 | 若历史颜色落在当前邻域颜色范围外，会 clamp 到当前颜色范围，降低 ghosting；lock / luma instability 可允许保留更多历史以减少闪烁；reactive 高会削弱历史保留。 |
| 曝光 / PreExposure | 当前 `ViewInfo.PreExposure` 与上一帧 `PrevPreExposure` | 不直接改变权重，但会把历史颜色转换到当前曝光空间后再混合，避免曝光变化导致历史颜色过亮或过暗。 |
| jitter / jitter sequence | `ViewInfo.TemporalJitterPixels`、`JitterSequenceLength()` | jitter 决定当前帧子像素采样位置，多帧积累提升细节；jitter sequence 也影响 lock lifetime 衰减。 |
| RCAS | `r.ArmASR.Sharpness > 0` | 不参与 Accumulate 内部历史/当前帧混色，只在 Accumulate 之后对 internal upscaled color 做锐化并写最终输出。 |

关键方向总结：

| 因素增大 | 混色趋势 |
|---|---|
| reactive factor 增大 | 减少历史，多用当前帧。 |
| depth clip 增大 | 减少历史，防止遮挡 ghosting。 |
| motion velocity 增大 | 减少历史，防止运动拖影。 |
| accumulation mask 增大 | 削弱 lock / 历史稳定性。 |
| lock contribution 增大 | 更保留历史细节，减少细线闪烁。 |
| current upsample weight 增大 | 当前帧颜色占比提高。 |
| history invalid / reset | 直接使用当前帧初始化历史。 |

---

## 9. RCAS Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRRCAS.h`
- Shader：`Shaders/Private/RCASPass.usf`
- Core：`Shaders/Private/fsr2/ffxm_fsr2_rcas_pass_fs.hlsl`
- 运行条件：`r.ArmASR.Sharpness > 0`

### 核心逻辑

RCAS，即 Robust Contrast Adaptive Sharpening，对 Accumulate 输出的内部 full-res color 做对比度自适应锐化，然后写入最终输出。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `r_input_exposure` | `ExposureTexture` | 当前曝光。 |
| `r_rcas_input` | Accumulate RT0 | 内部超分颜色。 |
| `cbArmASRRCAS` | `Sharpness` remap | 锐化强度配置。 |
| `cbArmASR` | common params | 输出尺寸等。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `ArmASROutputSceneColor` | full-res output | `Outputs.FullRes.Texture` | 最终锐化后的全分辨率颜色。 |

---

## History Extraction / 下一帧流向

| 本帧资源 | 存入 history | 下一帧读者 |
|---|---|---|
| `NewLock` | `NewHistory->NewLock` | 下一帧 Lock / Accumulate 路径。 |
| `Accumulate RT0 InternalUpscaledColor` | `NewHistory->UpscaledColour` | 下一帧 Accumulate。 |
| `Accumulate LockStatus RT` | `NewHistory->LockStatus` | 下一帧 Accumulate。 |
| Quality：`Accumulate RT2 LumaHistory` | `NewHistory->LumaHistory` | 下一帧 Accumulate。 |
| Balanced/Performance：`Accumulate RT1 InternalReactive` | `NewHistory->InternalReactive` | 下一帧 Accumulate。 |
| 非 Ultra：`RPD RT1 DilatedMotionVector` | `NewHistory->DilatedMotionVectors` | 下一帧 Depth Clip。 |
| Ultra：`RPD RT0 DilatedDepthVelocityLuma` | `NewHistory->DilatedDepthMotionVectorsInputLuma` | 下一帧 Depth Clip / Accumulate。 |
| `fPreExposure` | `NewHistory->PreExposure` | 下一帧 common params。 |

## 资源依赖概览

```text
UE Inputs:
  SceneColor
  SceneDepth
  SceneVelocity
  View / EyeAdaptation
  GBuffer / Reflections / Lumen history
  PrevHistory
        |
        |-- Create Reactive Mask
        |      outputs: ReactiveMask, CompositeMask
        |
        |-- ConvertVelocity
        |      outputs: MotionVectorTextureNew
        |
        |-- Compute Luminance Pyramid [non-Ultra]
        |      outputs: MipShadingChangeTexture, AutoExposureTexture
        |
        |-- CopyExposure [when not ASR auto exposure or Ultra]
        |      outputs: ExposureTexture
        |
        |-- Reconstruct Previous Depth
        |      outputs: reconstructed previous nearest depth,
        |               dilated depth / motion vector / luma
        |
        |-- Depth Clip
        |      outputs: DilatedReactiveMasks, PreparedInputColor
        |
        |-- Lock
        |      outputs: NewLock
        |
        |-- Accumulate
        |      outputs: InternalUpscaledColor, LockStatus,
        |               LumaHistory or InternalReactive,
        |               FinalColor if RCAS disabled
        |
        |-- RCAS [optional]
               output: FinalColor
```

## 结论

1. 当前 ASR 实现基本沿用 FSR2 pass 结构，UE 侧额外实现 `CreateReactiveMask` 与 `ConvertVelocity` 做数据适配。
2. `Accumulate` 是主算法 pass，负责 temporal upsampling、历史重投影、历史权重控制和最终颜色生成。
3. `Reconstruct Previous Depth + Depth Clip` 是历史可靠性判断的核心前置流程。
4. `Lock` 负责高频细节稳定性，减少细线、边缘等区域闪烁。
5. 质量档位主要影响中间 RT 数量、格式和历史信息保存方式：
   - Quality：保留 `LumaHistory`，颜色历史为 `PF_FloatRGBA`。
   - Balanced/Performance：使用 `InternalReactive`，颜色历史为 `PF_FloatR11G11B10`。
   - Ultra Performance：跳过 reactive mask 和 luminance pyramid，并合并 depth/mv/luma。
6. 最终输出：不开锐化由 `Accumulate` 写；开启锐化由 `RCAS` 写。
