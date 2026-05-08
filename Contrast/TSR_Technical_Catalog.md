# TSR (Temporal Super Resolution) — 关键技术完整目录

> Source: `C:\Work\Repo\GUSD\UE_553\Engine\Shaders\Private\TemporalSuperResolution\` + `TemporalSuperResolution.cpp`
> Total: 82 features, 21 categories

---

## Pipeline 执行顺序

```
C++: TemporalSuperResolution.cpp

1. ClearPrevTextures        (cpp:1874)
2. DilateVelocity           (cpp:2027)
3. DecimateHistory          (cpp:2147)
4. RejectShading            (cpp:2380)
5. SpatialAntiAliasing      (cpp:2423)
6. UpdateHistory            (cpp:2613)
7. ResolveHistory           (cpp:2700, upscale only)
8. Visualize                (cpp:2910, debug only)
```

---

## 1. MOTION VECTOR DILATION

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 1.1 | `FetchDepthVelocity3x3` | `TSRDepthVelocityAnalysis.ush:91` | 3x3 深度+velocity 邻域并行加载 |
| 1.2 | `FindClosestDepthOffset` | `TSRDepthVelocityAnalysis.ush:269` | 3x3 中找最近深度像素及其偏移，驱动 MV 膨胀 |
| 1.3 | `FetchAndComputeScreenVelocity` | `TSRDilateVelocity.usf:58` | 在最近遮挡物位置采样 velocity 作为膨胀结果 |
| 1.4 | `EncodeReprojectionVector` / `DecodeReprojectionVector` | `TSRReprojectionField.ush:79-97` | 膨胀后 velocity 的 32-bit 编解码 |
| 1.5 | `ComputeReprojectionEdge` | `TSRDilateVelocity.usf:239,304` | 膨胀前/后 velocity 差异，检测跨边界像素 |
| 1.6 | Velocity Flatten Tile | `TSRDilateVelocity.usf:355` | 逐 tile 极坐标 velocity 归约（MotionBlur 用）|

---

## 2. DISOCCLUSION DETECTION (Parallax Rejection)

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 2.1 | `ScatterClosestOccluder` | `TSRClosestOccluder.ush:101` | InterlockedMax 反向散射最近深度+孔填充 velocity |
| 2.2 | `LoadPrevAtomicTexturesSamples` | `TSRClosestOccluder.ush:160` | 加载前一帧 PrevAtomic 纹理的 4 个 bilinear 样本 |
| 2.3 | `ProcessPrevAtomicTexturesSamples` | `TSRClosestOccluder.ush:190` | 比较世界空间深度，生成 parallax rejection mask |
| 2.4 | Parallax Rejection Threshold | `TSRClosestOccluder.ush:330` | `bIsParallaxDisocclusion = !bIsOffScreen && ParallaxRejectionMask < 0.5` |
| 2.5 | `IsOffScreenOrDisoccluded` | `TSRCommon.ush:429` | 组合 camera-cut + off-screen + parallax → 二值 flag |
| 2.6 | `ComputePixelDeviceZError` | `TSRDepthVelocityAnalysis.ush:41` | 像素级 device-Z 容差（最小可分辨深度差）|
| 2.7 | `EncodeDeviceZError` / `DecodeDeviceZError` | `TSRDepthVelocityAnalysis.ush:51-67` | 深度误差 8-bit 编解码，传入 DecimateHistory |

---

## 3. HISTORY REPROJECTION & SAMPLING

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 3.1 | `ReprojectAllPrevTextures` | `TSRDecimateHistory.usf:328` | 批量重投影：guide + moire + resurrection + depth atomics |
| 3.2 | `GetBicubic2DCatmullRomSamples_Stubbe` | `TSRKernels.ush:355` | 5-sample 2D Catmull-Rom 采样模式 |
| 3.3 | `FCatmullRomFetches` | `TSRKernels.ush:398` | 泛型模板：bicubic 采样 + 加权累加 |
| 3.4 | History Catmull-Rom Sampling | `TSRUpdateHistory.usf:941-971` | 每个 DPV lane 2 像素 × 5 tap Catmull-Rom |
| 3.5 | `BilinearSampleColorHistory` | `TSRUpdateHistory.usf:403` | 手动 bilinear + HDR 加权补偿 |
| 3.6 | `ScreenPosToPrevHistoryBufferUV` | `TSRDecimateHistory.usf:49` | 当前 screen pos → 前一帧 history buffer UV |
| 3.7 | Pre-Exposure Correction in Samples | `TSRUpdateHistory.usf:975-978` | Catmull-Rom 累加时乘 PreExposureCorrection |
| 3.8 | Negative/NaN Fallback | `TSRUpdateHistory.usf:1011-1021` | Catmull-Rom 累加出现负数/NaN → 回退中心样本 |

---

## 4. COLOR CLAMPING / HISTORY RECTIFICATION

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 4.1 | Clamp Box Construction | `TSRUpdateHistory.usf:1185-1213` | 当前帧空间样本构建 AABB clamp box |
| 4.2 | History Clamp | `TSRUpdateHistory.usf:1235` | `fastClamp(history, InputMin, InputMax)` |
| 4.3 | `DisableHistoryClamp` / HDR-Weighted Lerp | `TSRUpdateHistory.usf:1236-1237` | HDR 加权夹在 clamped/unclamped history 之间 |
| 4.4 | `WeightedLerpFactors` | `TSRUpdateHistory.usf:1236` | HDR 色调加权 lerp 因子，防亮区亮度失真 |
| 4.5 | History Motion Validity Clamp | `TSRUpdateHistory.usf:1243` | 按 `OutputPixelVelocity` 衰减 validity |

---

## 5. DEPTH ANALYSIS

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 5.1 | `FetchDepth3x3` | `TSRDepthVelocityAnalysis.ush:75` | 无 reprojection field 时的 3x3 深度加载 |
| 5.2 | `FetchDepthVelocity3x3` | `TSRDepthVelocityAnalysis.ush:91` | 有 reprojection field 时的 3x3 深度+velocity |
| 5.3 | `ComputePixelVelocityNeighborhood` | `TSRDepthVelocityAnalysis.ush:241` | 将编码 velocity 转为像素空间，计算邻域深度-velocity 差 |
| 5.4 | `ComputeDepthError` | `TSRDepthVelocityAnalysis.ush:409` | 从深度邻域梯度计算 parallax 深度误差 |
| 5.5 | `ComputeReprojectionJacobian` | `TSRDepthVelocityAnalysis.ush:463` | 2x2 velocity Jacobian 估计 |
| 5.6 | `ComputeVelocityJacobian` | `TSRDepthVelocityAnalysis.ush:429` | 逐方向 Jacobian |
| 5.7 | `ComputeReprojectionUpscaleFactorFromJacobian` | `TSRReprojectionField.ush:155` | 从 Jacobian 推导重投影 upscale 修正因子 |

---

## 6. SPATIAL ANTI-ALIASING

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 6.1 | `SpatialAntiAliasingCS` | `TSRSpatialAntiAliasing.usf:1-222` | 检测高频 aliasing 模式，输出 subpixel offset |
| 6.2 | Noise Filtering Detection | `TSRSpatialAntiAliasing.ush:70-94` | 水平/垂直 total variation 分析 |
| 6.3 | Edge-Aware Subpixel Offset | `TSRSpatialAntiAliasing.ush:496-631` | 深度边缘方向分析 → `ReprojectionBoundary` |
| 6.4 | `EncodeSpatialAntiAliasingOffset` / `Decode` | `TSRSpatialAntiAliasing.ush:641-678` | 16-bit 编解码 |
| 6.5 | `ShouldSpatialAntiAlias` | `TSRShadingAnalysis.ush:186-188` | aliasing 可见 + (rejection 差 OR disoccluded) |
| 6.6 | Spatial AA Offset Application | `TSRUpdateHistory.usf:1072-1083` | 解码的 offset 应用到 InputPPCo |

---

## 7. HISTORY VALIDITY / CONFIDENCE

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 7.1 | `PrevHistoryValidity` | `TSRUpdateHistory.usf:916` | 从 metadata 读取的累积 validity |
| 7.2 | `ClampedPrevHistoryValidity` | `TSRUpdateHistory.usf:1104-1109` | `MaxValidity = 1.0 - WeightClampingRejection * DecreaseValidityMultiplier` |
| 7.3 | `DecreaseValidityMultiplier` | `TSRUpdateHistory.usf:700,843,849` | 从 RejectShading 传入的强制降权信号 |
| 7.4 | `ComputePredictionCompleteness` | `TSRCommon.ush:423-425` | `saturate(Validity * MaxSampleCount - 0.2)` |
| 7.5 | `FinalHistoryValidity` | `TSRUpdateHistory.usf:1276` | 量化 validity = `CurrentWeight + PrevWeight` |
| 7.6 | `HistorySampleCount` | `TSRUpdateHistory.usf:141` | C++ 传入的最大样本数 (8-32) |
| 7.7 | Motion-Based Validity Clamp | `TSRUpdateHistory.usf:1243-1249` | 按 `OutputPixelVelocity` 缩放 validity |

---

## 8. COLOR SPACE SYSTEM

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 8.1 | GCS (Guide Color Space) | `TSRColorSpace.ush:59-84` | `Linear / (Linear + 0.17)` 类 Gamma 色调映射 |
| 8.2 | SMCS (Shading Measurement) | `TSRColorSpace.ush:102-106` | `GCS^2`，用于 shading rejection 测量 |
| 8.3 | `LinearToSMCS` / `SMCSToLinear` | `TSRColorSpace.ush:131-152` | Linear ↔ SMCS 直转 |
| 8.4 | `HdrWeight4` / `HdrWeightY` | `TSRColorSpace.ush:184-230` | `1/(Luma+4)` 的 Karis HDR 加权 |
| 8.5 | `HdrWeightInvY` | `TSRColorSpace.ush:198-201` | `4/(1-LDRLuma)` 逆 HDR 权重 |
| 8.6 | `kGCSPreceptionAdd` / `kHistoryAccumulationPreceptionAdd` | `TSRColorSpace.ush:49-50` | 感知偏移常量 (0.17 / 1.0) |
| 8.7 | `SafeRcp` | `TSRCommon.ush:374` | fp16 安全倒数 |
| 8.8 | `Luma4` | `TSRColorSpace.ush:160-181` | 近似亮度 `G*2+R+B` × 4 |

---

## 9. HISTORY RESURRECTION

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 9.1 | `IsResurrectedFrameCloserThanPreviousFrame` | `TSRRejectShading.usf:240` | 比较 resurrection frame vs 前一帧的颜色匹配度 |
| 9.2 | `IsWorthMeasuringRejectionOfResurrectedFrame` | `TSRRejectShading.usf:263` | 仅在前一帧 rejection > 0.5 时才检测 resurrection |
| 9.3 | `ShouldResurrectHistory` | `TSRRejectShading.usf:285` | 更近 + rejection 改善 > 0.1 |
| 9.4 | `FetchResurrectedHistoryGuide` | `TSRRejectShading.usf:225` | 取 resurrection history guide + uncertainty |
| 9.5 | `OverwriteReprojectionFieldWithResurrection` | `TSRRejectShading.usf:311` | 覆写 reprojection 指向 resurrection frame |
| 9.6 | Resurrection Clip-to-Clip Transform | `TSRRejectShading.usf:328-331` | `ClipToResurrectionClip` 矩阵计算 |
| 9.7 | Resurrection Frame Slice Indexing | `TSRDecimateHistory.usf:220-225` | Texture2DArray 切片选择 |
| 9.8 | Resurrection Rolling Index (C++) | `TemporalSuperResolution.cpp:1734-1816` | 环形 buffer 管理 + camera-cut 重置 |

---

## 10. EXPOSURE HANDLING

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 10.1 | `PreExposureCorrectGCS` | `TSRColorSpace.ush:87-89` | GCS → Linear × Correction → GCS |
| 10.2 | `HistoryPreExposureCorrection` / `ResurrectionPreExposureCorrection` | `TSRCommon.ush:190-191` | C++ 侧计算 `CurPreExposure / PrevPreExposure` |
| 10.3 | `CorrectGuideColorExposure` | `TSRDecimateHistory.usf:83` | Guide 曝光矫正 |
| 10.4 | `CorrectMoireExposure` | `TSRDecimateHistory.usf:104` | Moire history 曝光矫正 |

---

## 11. REPROJECTION FIELD

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 11.1 | Reprojection Output Indices | `TSRReprojectionField.ush:10-12` | 3 slices: Vector(0), Jacobian(1), Boundary(2) |
| 11.2 | `EncodeReprojectionBoundary` / `Decode` | `TSRReprojectionField.ush:21-53` | 26-bit 编码 offset + boundary |
| 11.3 | `ComputeReprojectionBoundary` | `TSRSpatialAntiAliasing.ush:496` | 深度边缘分析 |
| 11.4 | `IsHistoryPixelWithinOffsetBoundary` | `TSRReprojectionField.ush:63` | 判断 history 像素是否在 boundary 内 |
| 11.5 | `ComputeReprojectionBoundaryCoverage` | `TSRReprojectionField.ush:56` | boundary 覆盖估计 |
| 11.6 | `EncodeReprojectionJacobian` / `Decode` | `TSRReprojectionField.ush:108-152` | 2x2 Jacobian 32-bit 编解码 |
| 11.7 | Boundary Usage in UpdateHistory | `TSRUpdateHistory.usf:743-752` | Boundary 偏移 Catmull-Rom 核 |
| 11.8 | `FetchVelocityDilateBoundary` | `TSRUpdateHistory.usf:580` | 加载 reprojection boundary |

---

## 12. FLICKERING / MOIRE DETECTION

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 12.1 | `FTSRMeasureFlickeringLumaCS` | `TSRMeasureFlickeringLuma.usf:1-65` | GCS 亮度预测量 pass |
| 12.2 | `ComputeMoireError` | `TSRShadingAnalysis.ush:196` | 多帧亮度变化追踪 + TotalVariation 计数 |
| 12.3 | Moire Error in MeasureRejection | `TSRShadingAnalysis.ush:481-505` | Moire 区域扩大 clamp box |
| 12.4 | `bIsRefining` | `TSRUpdateHistory.usf:1130-1134` | refining/rejecting 决策 |
| 12.5 | `KernelInputToHistoryLerp` | `TSRUpdateHistory.usf:1139` | refining 时从 soft → sharp kernel |
| 12.6 | `RefiningHisteresis` | `TSRUpdateHistory.usf:1116-1119` | `Ideal / (PrevValidity + Ideal)` |
| 12.7 | `bIsMovingMask` | `TSRDilateVelocity.usf:316-349` | 世界位置比较 + velocity 幅值检测运动像素 |
| 12.8 | Input Luma Modification Check | `TSRShadingAnalysis.ush:216-233` | 半透明/半透明分通道导致的亮度变化检测 |
| 12.9 | Flickering Frame Period | `TSRShadingAnalysis.ush:211` | `1/(1-pow(1-BlendFinal, Period))` 自适应阈值 |
| 12.10 | `bEnableFlickeringHeuristic` | `TSRRejectShading.usf:45` | CVar 控制 |
| 12.11 | `NoiseFiltering` | `TSRUpdateHistory.usf:1137` | 噪声区域软化 upscaling kernel |

---

## 13. DECIMATION (HISTORY DOWNSCALE)

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 13.1 | `FTSRDecimateHistoryCS` | `TSRDecimateHistory.usf:1-500` | 单 pass 重投影全部 history → 降采样 |
| 13.2 | Decimate Bitmask | `TSRDecimateHistory.usf:431-436` | 5-bit: OffScreen, ParallaxDisocclusion, PixelAnimation, HoleFill, ResurrectionOffScreen |
| 13.3 | `FetchDecimateMask` | `TSRRejectShading.usf:119` | 解码 bitmask |
| 13.4 | Velocity Edge in Decimate Mask | `TSRDecimateHistory.usf:299-301` | 预膨胀 edge 优先 |
| 13.5 | LDS Spill | `TSRDecimateHistory.usf:74-77,288-290` | 手动 LDS 保存 VGPR |

---

## 14. HISTORY UPDATE / BLEND WEIGHTS

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 14.1 | `PrevWeight` / `CurrentWeight` | `TSRUpdateHistory.usf:1057-1058` | 逐像素 blend 权重 |
| 14.2 | `ComputeSampleWeigth` | `TSRKernels.ush:438` | `saturate((0.9x2-1.9)x2+1.0+Min)` 基于 UpscaleFactor |
| 14.3 | `KernelInputToHistoryFactor` | `TSRUpdateHistory.usf:1089` | 动态 kernel 大小 |
| 14.4 | `MinRejectionBlendFactor` | `TSRUpdateHistory.usf:1094` | `min(LowFrequencyRejection, ReprojectionUpscaleCorrection)` → blend |
| 14.5 | `ComputePrevWeightMultiplier` | `TSRCommon.ush:438-441` | `(1-Blend)/Blend` |
| 14.6 | `InputToHistoryFactor` | `TSRUpdateHistory.usf:146` | C++ 传入的分辨率比例平方 |
| 14.7 | Spatial Weight Accumulation | `TSRUpdateHistory.usf:1150-1178` | 空间邻域 + HDR 加权过滤当前帧 |

---

## 15. VELOCITY HOLE FILLING

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 15.1 | `EncodeHoleFillingVelocity` | `TSRClosestOccluder.ush:56` | 18-bit: 5-bit angle + 13-bit length |
| 15.2 | `DecodeHoleFillingVelocity` | `TSRClosestOccluder.ush:73` | 解码 hole-filling velocity |
| 15.3 | `PolarCoordinateHoleFillingVelocity` / `Cartesian` | `TSRClosestOccluder.ush:27-38` | 极/直角坐标转换 |
| 15.4 | Hole Fill Adoption | `TSRDecimateHistory.usf:419-426` | disocclusion + hole fill → 替换 reprojection vector |
| 15.5 | `VELOCITY_HOLE_FILLING_BITS` | `TSRClosestOccluder.ush:12-15` | 编码常量 |
| 15.6 | Hole Fill Validation | `TSRClosestOccluder.ush:300-327` | velocity 匹配时减弱 parallax rejection |

---

## 16. SHADING REJECTION ANALYSIS

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 16.1 | `MeasureRejection` | `TSRShadingAnalysis.ush:446` | 核心 rejection：filtered input vs history |
| 16.2 | Rejection Box Size | `TSRShadingAnalysis.ush:462-466` | `max(QuantizationError, Blur3x3(TV), BoxSize*0.25*FilteringWeight)` |
| 16.3 | `AnnihilateToGuide3x3` | `TSRConvolutionNetwork.ush:887` | 交叉双边 guide 湮灭 |
| 16.4 | Median+MaxRGB/MinA 3x3 | `TSRShadingAnalysis.ush:545-546` | rejection clamp blend 后处理 |
| 16.5 | `RejectionBlendFinal` / `RejectionClampBlend` | `TSRRejectShading.usf:589-590` | 输出信号 |
| 16.6 | `FilteringWeight` | `TSRShadingAnalysis.ush:466` | 逐通道 0.25 权重 |
| 16.7 | `UpdateGuide` | `TSRRejectShading.usf:363` | Guide 更新公式 |
| 16.8 | `IncreaseValidityMultiplier` / `DisableHistoryClamp` | `TSRRejectShading.usf:780-781` | 传入 UpdateHistory 的 metadata |
| 16.9 | Convolution Network | `TSRConvolutionNetwork.ush:514-577` | 1x3+3x1 可分离卷积 + LDS/VGPR 共享 |
| 16.10 | `TotalVariation3x3` | `TSRConvolutionNetwork.ush` (via Blur3x3/MinMax3x3) | 3x3 总变差 |

---

## 17. HISTORY ATOMIC / METADATA

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 17.1 | `PrevAtomic` (InterlockedMax) | `TSRClosestOccluder.ush:134-137` | R32_UINT 原子散射 |
| 17.2 | `PrevAtomicTextureArray` (C++) | `TemporalSuperResolution.cpp:1856-1881` | 创建/清除 |
| 17.3 | Metadata Format | `TSRUpdateHistory.usf:161,164` | R8 UNORM 存储 validity |
| 17.4 | `AccumulateMetadata` | `TSRUpdateHistory.usf:993-1004` | Catmull-Rom 加权累加 metadata |
| 17.5 | Quantized Validity | `TSRUpdateHistory.usf:1276` | `ceil(255*Validity)/255` |
| 17.6 | `HistoryMetadataOutput` | `TSRUpdateHistory.usf:1339-1346` | 写入 History.MetadataArray |

---

## 18. TEMPORAL JITTER HANDLING

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 18.1 | `InputJitter` | `TSRCommon.ush:158` | `View.TemporalJitterPixels` |
| 18.2 | `ViewportUVToInputPPCo` | `TSRUpdateHistory.usf:137` | viewport UV → jitter 后的输入像素坐标 |
| 18.3 | Jitter Input PPCo | `TSRUpdateHistory.usf:671` | 应用到 history reprojection |
| 18.4 | Spatial AA Jitter-Derived Dilate | `TSRSpatialAntiAliasing.ush:577` | `DilateAmount = 0.5` |

---

## 19. LENS DISTORTION SUPPORT

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 19.1 | `UndistortingDisplacementTexture` | `TSRUpdateHistory.usf:659` | 逆畸变 |
| 19.2 | `ApplyDisplacementTextureOnScreenPos` | `TSRUpdateHistory.usf:572` | 位移纹理应用 |
| 19.3 | Forward Distort Textures | `TSRUpdateHistory.usf:130-132` | 前向畸变对齐 |
| 19.4 | Lens Distortion Enable (C++) | `TemporalSuperResolution.cpp:1535-1545` | LUT 启用条件 |

---

## 20. ANTI-FLICKERING / REFINING

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 20.1 | Refining vs. Rejecting | `TSRUpdateHistory.usf:1130-1134` | 逐像素决定 refining 或 rejecting |
| 20.2 | `bIsHistoryResurrection` | `TSRUpdateHistory.usf:706,836` | 当前 history 是否来自 resurrection |
| 20.3 | Composite Rejection Multiplexing | `TSRUpdateHistory.usf:830-849` | 聚合多种 rejection 信号 |

---

## 21. MISCELLANEOUS / UTILITY

| # | Feature | File:Line | Description |
|---|---------|-----------|-------------|
| 21.1 | High/Low Frequency Split | `TSRUpdateHistory.usf:158` | color detail (Catmull-Rom) + guide (prediction) |
| 21.2 | `ReprojectionUpscaleCorrection` | `TSRUpdateHistory.usf:805` | Jacobian upscale 修正因子 |
| 21.3 | ConvolutionNetwork Pass Infra | `TSRConvolutionNetworkPass.ush` | 原子 Lane 占用优化 |
| 21.4 | Stocastic Quantization | `TSRDecimateHistory.usf:446-451` | Hammersley 序列抖动量化 |
| 21.5 | History Color Output Quantization | `TSRUpdateHistory.usf:1315-1322` | FR10+10+10+2 / FP16 |
| 21.6 | Mitchell-Netravali Downsampling | `TSRResolveHistory.usf:340` | 4x4 核 + HDR 加权 |
| 21.7 | `TakeOnlyOneSamplePair` | `TSRCommon.ush:418` | 像素对足够近时共享样本 |
| 21.8 | 16-bit VALU (FP16 ALU) | `TSRCommon.ush:88,120` | `as_tsr_half` 双编译路径 |
| 21.9 | Quality Dim Levels | `TSRUpdateHistory.usf:75-81` | 4 级: High=9, Medium=6, Low=5, Epic=1+TAAU |
| 21.10 | Debug Visualize | `TSRVisualize.usf:1-556` | 14 种可视化模式 |
| 21.11 | Separate Translucency | C++:1614-1618 | 独立半透明 compositing |
