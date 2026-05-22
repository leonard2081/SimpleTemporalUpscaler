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
  -> CopyExposure               [未请求 ASR AutoExposure，或 Ultra]
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
| `LumenReflections` | `ArmASRInfo.LumenReflections` | Lumen reflection history；只有 history 有效且当前 view 使用 Lumen Reflections 时才会被 Create Reactive Mask 绑定，否则使用 black dummy。 |

### 历史输入

| 历史资源 | 来源 | 本帧用途 |
|---|---|---|
| `PrevUpscaledColour` | 上一帧 Accumulate RT0 | 颜色历史重投影。 |
| `PrevInternalReactive` | 上一帧非 Quality 路径提取的 history；在 Balanced/Performance 中对应 Accumulate RT1 temporal reactive。Ultra Performance 当前 C++ 也会提取 RT1 到该成员，但其 shader 输出语义与 LockStatus 在 RT1 上重合，属于实现细节，不能简单理解为独立的 reactive history。 | 本帧 Accumulate 的 temporal reactive 输入。 |
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

1. 读取并解码 GBuffer：
   - `GBufferB` / `GBufferD` 由 `PostInputs.SceneTextures` 提供。
   - `GBufferB` 主要用于解出 `Roughness` 和 `ShadingModelID`；在 UE 常规 deferred GBuffer 布局中，roughness 通常在 `GBufferB.b`，shading model ID 通常打包在 `GBufferB.a`。
   - `GBufferD` 主要用于解出 `CustomData`，常用 `CustomData.x/y`。
   - 实际代码不直接按通道硬读，而是通过 `DecodeGBufferData()` 得到 `GBuffer.Roughness`、`GBuffer.ShadingModelID`、`GBuffer.CustomData`。

2. 处理材质 custom data：
   - `CustomData.x` 不是任意材质都能自由写入的通用用户槽位，它的语义由当前像素的 `ShadingModelID` 决定。
   - 例如 `SHADINGMODELID_CLEAR_COAT` 下，`CustomData.x/y` 会被当作 clear coat / clear coat roughness 使用，shader 会用它修正 roughness。
   - 只有当 `GBuffer.ShadingModelID == r.ArmASR.ReactiveMaskReactiveShadingModelID` 时，ASR 才把 `GBuffer.CustomData.x` 解释为强制 reactive 值。
   - 如果 `r.ArmASR.ReactiveMaskForceReactiveMaterialValue > 0`，则使用该 CVar 值覆盖材质 `CustomData.x`。

3. 比较 `SceneColor` 与 `SceneColorPreAlpha` 估计透明贡献：
   - `SceneColorPreAlpha` 来自 opaque 后、alpha/translucency 合成前的场景颜色快照；如果没有该纹理，则回退为 `SceneColor`。
   - shader 计算 `Delta = abs(SceneColor - SceneColorPreAlpha)`，用颜色差异估计透明 / alpha 合成对当前像素的贡献。
   - 这个透明贡献会分别进入 reactive mask 和 composite mask 的候选值。

4. 根据深度反推距离，并对 roughness / translucency 做距离衰减：
   - shader 从 `InputDepth` 读取当前像素深度，用 `SvPositionToTranslatedWorld()` 得到当前像素对应表面点的 `TranslatedWorldPosition`。
   - `TranslatedWorldPosition` 是 UE 渲染中平移后的世界空间位置，通常可近似理解为 `WorldPosition - CameraWorldPosition`；它的原点在相机附近，但坐标轴仍是世界轴，不等同于 view space。
   - `length(TranslatedWorldPosition)` 用于估算像素表面到相机的世界空间距离。
   - roughness 路径会根据距离把 `Roughness` 逐渐推向 `1.0`，从而削弱远处低 roughness 表面对 reactive 的贡献。注意当前 C++ 参数绑定实际始终传入 `r.ArmASR.ReactiveMaskRoughnessMaxDistance`，没有使用 `View.FurthestReflectionCaptureDistance`。
   - translucency 路径会根据 `r.ArmASR.ReactiveMaskTranslucencyMaxDistance` 将远处透明贡献逐渐压到 `0`，避免天空盒、远处背景板或后期合成远景大面积写入 reactive。

5. 使用 `ReflectionTexture` 或 `LumenSpecular` 估计反射贡献：
   - `ReflectionTexture` 来自 UE reflection denoiser 的 `Outputs.Color`，可以理解为当前帧反射管线输出的屏幕空间反射结果。通常 `Reflection.rgb` 表示反射颜色，`Reflection.a` / `.w` 表示反射有效性、强度或权重。
   - 对于粗糙或没有有效反射结果的像素，`ReflectionTexture` 通常接近 `float4(0,0,0,0)`，或者至少 `Reflection.w == 0`。
   - 当 `Reflection.w > 0` 时，shader 优先用 `Reflection.w` 和 `Luminance(Reflection.rgb)` 计算反射贡献；反射越强、越亮，贡献越高。
   - 如果 `ReflectionTexture` 无效，则尝试使用 `LumenSpecular`。`LumenSpecular` 只有在当前 view 使用 Lumen Reflections、Lumen reflection history 有效且 ASR history 有效时才会绑定真实纹理，否则是 black dummy。
   - 如果 `ReflectionTexture` 和 `LumenSpecular` 都无效，则使用 roughness fallback：`(1 - Roughness) * r.ArmASR.ReactiveMaskRoughnessScale`，即越光滑越可能给一点反射相关 mask，越粗糙贡献越低。

6. 合成输出 mask：
   - 反射贡献主要进入 `CompositeMaskTexture`，即 `CompositeMask = saturate(TranslucencyContribution.x + ReflectionContribution)`。
   - `ReactiveMaskTexture` 主要来自透明 history 贡献和特殊 reactive shading model 的强制值，即 `ReactiveMask = max(ForceReactive, TranslucencyContribution.y)`。
   - 后续 Depth Clip 会读取这两张 mask，做邻域扩张、颜色相似性加权，并合并 motion divergence / temporal motion difference，最终形成 `DilatedReactiveMaskTexture.xy` 供 Accumulate 使用。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `GBufferB` | `PostInputs.SceneTextures->GBufferBTexture` | roughness / shading model 解码。 |
| `GBufferD` | `PostInputs.SceneTextures->GBufferDTexture` | custom data / shading model 解码。 |
| `ReflectionTexture` | `ArmASRInfo.ReflectionTexture` 或 black dummy | SSR/反射贡献。 |
| `InputDepth` | `SceneDepth` | 当前深度。 |
| `SceneColor` | `SceneColor` | 当前最终场景色。 |
| `SceneColorPreAlpha` | `ArmASRInfo.SceneColorPreAlpha` 或 `SceneColor` | alpha 前颜色，用于估计透明贡献。 |
| `LumenSpecular` | `ArmASRInfo.LumenReflections` 或 black dummy | 只有在 history 有效且当前 view 确认使用 Lumen Reflections 时才会绑定。 |
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
- 运行条件：未请求 ASR AutoExposure，或处于 Ultra。等价于 `!(r.ArmASR.AutoExposure != 0 && !UltraPerformance)`。

### 核心逻辑

从 UE `EyeAdaptationBuffer` 读取曝光，写入 1x1 `ExposureTexture`。当 `r.ArmASR.AutoExposure=1` 且非 Ultra 时，本步骤会被前面的 Compute Luminance Pyramid 产物替代；其他情况都走这里。

### 输入

| Shader 输入 | 来源 | 说明 |
|---|---|---|
| `EyeAdaptationBuffer` | `GetEyeAdaptationBuffer(GraphBuilder, View)` | UE eye adaptation / exposure。 |

### 输出

| 输出 | 格式 / 尺寸 | 去向 | 说明 |
|---|---|---|---|
| `ExposureTexture` | `PF_A32B32G32R32F`，1x1 | `RPD`、`Depth Clip`、`Accumulate`、`RCAS` | 当前帧曝光。 |

### 关键信息

#### `EyeAdaptationBuffer` 与 `ExposureTexture.x`

`EyeAdaptationBuffer` 在 shader 侧是 `StructuredBuffer<float4>`，不是单个裸标量。`CopyExposure.usf` 通过 UE 的 `EyeAdaptationLookupBuffer(EyeAdaptationBuffer)` 读取其中的 eye adaptation / auto exposure 参数，并写入 1x1 `ExposureTexture`。

后续 ASR 主要读取 `ExposureTexture[0,0].x`：

```text
Exposure() = r_input_exposure[uint2(0, 0)].x
```

这个 `.x` 可以理解为当前帧 eye adaptation / auto exposure 得到的曝光倍率，用来把线性 HDR 场景颜色转换到当前视觉曝光空间。若该值为 `0`，shader 会回退为 `1.0`。

#### 与 Compute Luminance Pyramid 的关系

ASR 有两种曝光来源：

| 条件 | 曝光来源 |
|---|---|
| `r.ArmASR.AutoExposure=1` 且非 Ultra | Compute Luminance Pyramid 输出的 `AutoExposureTexture`。 |
| 未请求 ASR AutoExposure，或 Ultra | CopyExposure 从 UE `EyeAdaptationBuffer` 拷贝到 `ExposureTexture`。 |

因此可以理解为：如果不使用 UE 的曝光值，就需要由前面的亮度金字塔根据当前 `SceneColor` 自己计算 `AutoExposureTexture`。Ultra Performance 跳过 Compute Luminance Pyramid，因此只能使用 UE 的曝光值。

#### `Exposure` 与 `PreExposure` 的区别

`ExposureTexture.x` 和 `ViewInfo.PreExposure` 都与曝光有关，但职责不同：

| 名称 | 来源 | 含义 | 是否作为 history 存储 |
|---|---|---|---|
| `ExposureTexture.x` | UE `EyeAdaptationBuffer` 或 ASR `AutoExposureTexture` | 当前帧相机 / eye adaptation 的曝光倍率，用于 ASR 内部视觉曝光空间。 | 不作为单独 history 存储。 |
| `PreExposure()` | `ViewInfo.PreExposure` | UE 写入 / 保存 `SceneColor` 时提前乘上的预曝光倍率，用于保持 HDR 数值稳定。 | 当前帧会存为 `NewHistory->PreExposure`。 |
| `PreviousFramePreExposure()` | `PrevHistory->PreExposure` | 上一帧保存 history color 时使用的 pre-exposure。 | 从上一帧 history 读取。 |

可以把颜色空间简化为：

```text
StoredSceneColor = PhysicalSceneColor * PreExposure
ExposedColor     = PhysicalSceneColor * Exposure
DisplayColor     = Tonemap(PhysicalSceneColor * Exposure)
```

`PhysicalSceneColor` 是场景物理 / 线性 HDR 光照结果，本身不随相机曝光变化；`Exposure` 决定它进入 tonemapper 前的亮度尺度；`PreExposure` 是 UE 内部为了数值稳定而使用的 scene color 存储尺度。

#### 后续如何使用曝光值

非 Ultra 路径中，ASR 会用 `PrepareRgb()` / `UnprepareRgb()` 在不同颜色空间之间转换：

```text
PrepareRgb(color, Exposure, PreExposure):
  color / PreExposure * Exposure

UnprepareRgb(color, Exposure):
  color / Exposure * CurrentPreExposure
```

含义是：

1. 当前帧输入 `SceneColor` 通常已经是 `PhysicalSceneColor * CurrentPreExposure`，ASR 先除以 `CurrentPreExposure`，再乘当前 `Exposure`，得到 `PhysicalSceneColor * CurrentExposure`。
2. 历史颜色通常保存为 `PhysicalSceneColor * PreviousFramePreExposure`，下一帧重投影后会除以 `PreviousFramePreExposure`，再乘当前 `Exposure`，也转换到 `PhysicalSceneColor * CurrentExposure`。
3. Accumulate 内部的历史重投影、颜色邻域 clipping box、rectification、luma instability、lock、shading change、RCAS 等判断都在这个当前视觉曝光空间中进行。
4. 最终输出 / history 写回前再除以当前 `Exposure`，乘回当前 `PreExposure`，回到 UE 后续后处理期望的 pre-exposed scene color 空间。

#### history 中保存的颜色不是最终送显颜色

非 Ultra 路径下，ASR history / output RGB 大致保存为：

```text
HistoryRGB ≈ PhysicalSceneColor * CurrentPreExposure
```

它不是：

```text
PhysicalSceneColor * Exposure
```

也不是 tone mapping 后的最终显示颜色。最终送显颜色通常是：

```text
DisplayColor = Tonemap(PhysicalSceneColor * Exposure)
```

ASR 保存 pre-exposed HDR scene color，是为了下一帧还能在 HDR / 线性空间进行时域重建，并让 UE 后续 tonemapper 继续按正常流程应用当前曝光和色彩变换。

#### 为什么最终会除回 `Exposure` 还要拷贝它

虽然 ASR 最终会通过 `UnprepareRgb()` 把颜色从 `PhysicalSceneColor * Exposure` 转回 `PhysicalSceneColor * PreExposure`，看起来 `Exposure` 被抵消了，但它仍然影响中间算法行为。

`Exposure` 的意义不是改变 ASR 最终输出空间，而是让 ASR 的内部比较和判断更接近当前帧最终视觉结果：

- 当前帧和历史帧颜色都被转换到同一个 `PhysicalSceneColor * CurrentExposure` 空间后再比较。
- rectification / clipping box 基于曝光后的颜色范围，避免在纯物理 HDR 数值空间中被极亮高光过度主导。
- luma instability、shading change、lock 相关亮度判断更接近当前眼适应后的视觉亮度。
- RCAS 锐化也需要在合理的曝光空间中处理 HDR 颜色。

因此 CopyExposure 的核心价值是：把 UE 已经计算好的当前帧曝光倍率提供给 ASR，让内部时域累积、历史裁剪和锐化等判断发生在一致的当前视觉曝光空间中，而最终输出仍回到 UE 期望的 pre-exposed scene color 空间。

---

## 5. Reconstruct Previous Depth Pass

- Wrapper：`Source/ArmASR/Private/Shaders/ArmASRReconstructPrevDepth.h`
- Shader：`Shaders/Private/ReconstructPrevDepthPass.usf`
- Core：`Shaders/Private/fsr2/ffxm_fsr2_reconstruct_previous_depth_pass_fs.hlsl`

### 核心逻辑

调用 `ReconstructAndDilate(uPixelCoord)`：用当前深度和 motion vector 重建前一帧最近深度，并在邻域中扩张 depth / motion vector / luma，提升运动边界和薄几何体处的稳定性。Ultra 模式将 depth、motion vector、luma 合并为一个 RT。

主要步骤：

1. 在当前低分辨率像素的 3x3 邻域中查找最近深度：
   - 读取当前像素周围 9 个 `r_input_depth` 样本。
   - inverted depth 下 device depth 越大表示越近，因此选择最大 depth；非 inverted depth 则选择最小 depth。
   - 输出 `fDilatedDepth` 和最近深度所在坐标 `iNearestDepthCoord`。
   - 目的：执行 depth dilation，让运动边界、薄几何体、前景轮廓附近的像素更倾向使用最近前景深度，减少背景深度污染前景边缘。

2. 用最近深度坐标确定 motion vector 采样点：
   - 当前 ASR 使用 low resolution motion vectors，因此 `iMotionVectorPos = iNearestDepthCoord`。
   - 然后从 `r_input_motion_vectors` 读取 `fDilatedMotionVector`。
   - 目的：让扩张后的 depth 和 motion vector 来自同一个最近前景样本，避免出现“depth 是前景、motion vector 却是背景”的错配。

3. 使用扩张后的 depth 和 motion vector 重建上一帧最近深度：
   - 当前像素 UV 为 `(iPxPos + 0.5) / RenderSize()`。
   - 根据 motion vector 计算上一帧位置：`fReprojectedUv = fUv + fDilatedMotionVector`。
   - 对重投影位置做 bilinear footprint 计算，将当前 `fDilatedDepth` 写入覆盖到的最多 4 个上一帧 texel。
   - 写入时会根据 depth 规则做原子比较：inverted depth 用 `InterlockedMax` 保留最近深度，非 inverted depth 用 `InterlockedMin`。
   - 目的：生成 `ReconstructedPreviousNearestDepthTexture`，表示当前帧几何按 motion vector 投影回上一帧后，在上一帧各位置可见的最近深度。后续 Depth Clip 用它判断历史样本是否来自同一表面，以及是否发生遮挡 / 反遮挡。

4. 计算 Lock pass 使用的当前像素 luma：
   - 对当前像素 `iPxLrPos` 读取 `SceneColor`，并执行 `max(rgb, 0)` 避免负颜色影响亮度。
   - 使用 `rgb / PreExposure() * Exposure()` 转到当前视觉曝光空间。
   - HDR 输入下再执行 `Tonemap(rgb)`，压缩高亮，避免极亮值主导 lock 检测。
   - 然后计算 `RGBToPerceivedLuma(rgb)`，并取 `pow(luma, 1/6)`。
   - 因此 `LockLumaTexture` 可以理解为“当前像素在当前曝光空间下、经过 tonemap 和感知亮度变换后的 lock 专用 luma”，不是简单的线性 `dot(rgb, float3(...))`。
   - 目的：给 Lock Pass 检测细线、高频纹理、亚像素边缘和亮度 ridge，减少 temporal jitter 下的闪烁。

5. 输出扩张后的 depth / motion vector / luma：
   - 非 Ultra：分别写入 `DilatedDepthTexture`、`DilatedVelocityTexture`、`LockLumaTexture`。
   - Ultra：写入合并纹理 `DilatedDepthVelocityLumaTexture`，其中 `.x=depth`，`.yz=motion vector`，`.w=luma`。
   - 目的：这些结果分别供 Depth Clip 做遮挡判断、供 Accumulate 做历史重投影、供 Lock Pass 做细节稳定性检测，并作为下一帧 history 的一部分继续使用。

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

### Lock 细节

Lock 机制用于稳定细线、高频纹理、亚像素边缘等容易随 temporal jitter 闪烁的细节。它分成两部分：

1. `Lock Pass` 根据当前帧亮度生成 `NewLock`。
2. `Accumulate Pass` 结合 `NewLock` 和上一帧 `PrevLockStatus` 维护完整 lock 状态，并让 lock 影响历史颜色保留。

#### `r_lock_input_luma` 存储内容

非 Ultra 下，`r_lock_input_luma` 来自 `Reconstruct Previous Depth Pass` 的 `LockLumaTexture`，格式为 `PF_R16F`。它存储当前帧 per-pixel luma，用于 Lock pass 做局部亮度结构检测。

Ultra Performance 下没有单独的 `LockLumaTexture`，luma 被打包在 `DilatedDepthMotionVectorsInputLumaTexture.w` 中：

```text
DilatedDepthMotionVectorsInputLumaTexture.x  = dilated depth
DilatedDepthMotionVectorsInputLumaTexture.yz = dilated motion vector
DilatedDepthMotionVectorsInputLumaTexture.w  = luma
```

因此 Lock pass 的细线检测主要看亮度结构，而不是直接看完整 RGB 颜色。颜色只会通过转换后的 luma 间接影响 lock 检测。

#### `NewLock` 存储内容

`NewLock` 是当前帧新检测出的 thin feature / 细线候选 mask。当前实现基本是二值：

| 值 | 含义 |
|---|---|
| `0.0` | 当前像素未检测到新 lock。 |
| `1.0` | 当前像素检测到 thin feature，需要建立 new lock。 |

本帧开始时 `NewLock` 会被 clear，然后 Lock pass 只在检测成功的位置写入 `1.0`。它不是完整 lock 状态，只是当前帧的 new lock 候选。

#### Lock 检测依据

Lock pass 的核心检测函数是 `ComputeThinFeatureConfidence()`，主要步骤如下：

1. 读取当前像素亮度 `fNucleus = LoadLockInputLuma(pos)`。
2. 读取 3x3 邻域亮度样本。
3. 用亮度比值判断邻居是否与中心相似，阈值约为 `1.05`，也就是亮度差异约 5% 内认为相似。
4. 判断中心像素是否是局部 ridge：中心亮度要么比所有不相似邻居更亮，要么更暗。
5. 使用 4 个 2x2 rejection masks 排除大块相似区域，避免把普通面片误判为细线。
6. 如果通过检测，则写入 `NewLock = 1.0`。

因此 Lock 更容易检测黑白细线、高亮度对比边缘、栅栏、发丝等亮度高频结构；对于色相变化明显但亮度接近的细节不敏感。

#### `PrevLockStatus` 存储内容

完整 lock 状态不是 `NewLock`，而是 Accumulate 输出的 `LockStatusOutputTexture`，下一帧作为 `PrevLockStatus` 输入。其格式是 `PF_G16R16F`，两个通道语义为：

| 通道 | 含义 |
|---|---|
| `.x` / `LOCK_LIFETIME_REMAINING` | lock 剩余生命周期。 |
| `.y` / `LOCK_TEMPORAL_LUMA` | lock 关联的 temporal luma。 |

`PrevLockStatus` 会在下一帧 Accumulate 中按重投影 UV 采样：

```text
current pixel -> motion vector -> previous frame UV -> sample PrevLockStatus
```

这样可以判断该像素对应的历史位置上一帧是否已经处于 locked 状态。

#### 为什么 `NewLock` 和 `PrevLockStatus` 分开

| 资源 | 空间 / 语义 | 用途 |
|---|---|---|
| `NewLock` | 当前帧 high-res 像素位置 | 当前帧 Lock pass 检测出的新 lock 候选。 |
| `PrevLockStatus` | 上一帧历史状态，按重投影 UV 采样 | 保存已有 lock 的生命周期和 temporal luma。 |

二者不能简单合并，因为：

- `NewLock` 是当前帧检测结果，`PrevLockStatus` 是历史状态。
- `NewLock` 按当前像素位置读取，`PrevLockStatus` 按重投影 UV 读取。
- `NewLock` 是单通道 mask，`PrevLockStatus` 是双通道状态。
- Accumulate 需要同时知道“当前是否新检测到 lock”和“上一帧是否已经 locked”。

#### Lock 状态如何维护

Accumulate 中会读取：

```text
NewLock          -> state.NewLock
PrevLockStatus   -> state.WasLockedPrevFrame + fLockStatus
```

如果当前帧检测到 `NewLock`：

```text
LOCK_TEMPORAL_LUMA      = 当前 shading change luma
LOCK_LIFETIME_REMAINING = 1.0 或 2.0
```

如果没有新的 lock，但旧 lock 生命周期还较短，则会逐渐更新 temporal luma；如果亮度变化过大，则 kill lock。

lock 生命周期还会被以下因素削弱：

| 因素 | 对 lock 的影响 |
|---|---|
| reactive factor 高 | `LOCK_LIFETIME_REMAINING *= (1 - reactive)`，减少 lock。 |
| accumulation mask 高 | `LOCK_LIFETIME_REMAINING *= (1 - accumulationMask)`，减少 lock。 |
| depth clip 高 | depth clip 不稳定时 kill / 禁用 lock。 |
| 估计下一帧 UV 出屏幕 | kill lock，避免屏幕边界锁住错误历史。 |
| jitter 累积进度 | lock lifetime 随当前帧 upsample weight / jitter sequence 逐步衰减。 |

#### Lock 如何影响后续 Accumulate 混色

Lock 不直接输出最终颜色，也不直接改变当前帧 `SceneColor`。它通过 `fLockContributionThisFrame` 影响 Accumulate 的 history rectification。

当历史颜色落在当前帧颜色邻域 clipping box 外时，Accumulate 会倾向于把历史颜色 clamp 到当前邻域范围，以减少 ghosting。但有效 lock 会允许保留更多历史细节：

```text
lock contribution 高 -> 更多保留历史颜色 -> 减少细线 / 高频细节闪烁
```

同时，如果 reactive、depth clip 或 accumulation mask 较高，lock 会被削弱或杀掉：

```text
不稳定区域 -> 减少 lock -> 避免锁住错误历史 -> 降低拖影
```

所以 lock 的作用是平衡：

| 场景 | Lock 行为 |
|---|---|
| 静止或稳定的细线 / 高频结构 | 保留更多历史，减少 shimmer。 |
| 遮挡变化、透明、反射、快速运动区域 | 削弱或取消 lock，避免 ghosting。 |

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
| `r_internal_temporal_reactive` | `PrevHistory->InternalReactive`，Balanced/Performance | temporal reactive 历史；Ultra shader 路径不绑定独立 temporal reactive 输入。 |
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
| `ArmASROutputSceneColor` | full-res output | `Outputs.FullRes.Texture` | 最终锐化后的全分辨率颜色；当前 wrapper 通过 pixel shader `RenderTargets[0]` 写入，虽然参数结构中保留了 `rw_upscaled_output` UAV 声明。 |

---

## History Extraction / 下一帧流向

| 本帧资源 | 存入 history | 下一帧读者 |
|---|---|---|
| `NewLock` | `NewHistory->NewLock` | 下一帧 Lock / Accumulate 路径。 |
| `Accumulate RT0 InternalUpscaledColor` | `NewHistory->UpscaledColour` | 下一帧 Accumulate。 |
| `Accumulate LockStatus RT` | `NewHistory->LockStatus` | 下一帧 Accumulate。 |
| Quality：`Accumulate RT2 LumaHistory` | `NewHistory->LumaHistory` | 下一帧 Accumulate。 |
| 非 Quality：`NewHistory->InternalReactive` | Balanced/Performance 中来自 `Accumulate RT1 InternalReactive`；Ultra Performance 当前 C++ 也会提取 Accumulate RT1 到该成员，但该 RT1 在 shader 输出语义上是 LockStatus，属于实现细节/潜在问题。 | Balanced/Performance 下一帧 Accumulate 的 temporal reactive 输入；Ultra 不绑定独立 temporal reactive 输入。 |
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
        |-- CopyExposure [when not ASR auto exposure, or Ultra]
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
        |               LumaHistory or InternalReactive depending on quality path,
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
   - Ultra Performance：跳过 reactive mask 和 luminance pyramid，并合并 depth/mv/luma；shader 不输出独立 temporal reactive RT，但当前 C++ history 仍会填充 `InternalReactive` 成员，且与 RT1 LockStatus 输出存在重合这一实现细节。
6. 最终输出：不开锐化由 `Accumulate` 写；开启锐化由 `RCAS` 写。
