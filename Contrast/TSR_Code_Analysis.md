# TSR (Temporal Super Resolution) 代码深度分析

> 基于 UE 5.5.3 源码

---

## 1. Pass: ClearPrevTextures

**源文件**: `Engine/Shaders/Private/TemporalSuperResolution/TSRClearPrevTextures.usf`
**C++ 调度**: `Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp:1869`

### 1.1 概述

该 pass 初始化一个名为 `TSR.PrevAtomics` 的 `RWTexture2DArray<uint>` (PF_R32_UINT)，将所有元素清零。该纹理随后在 `DilateVelocity` pass 中用作原子累加器（InterlockedMax），存储"最近遮挡物深度"。

### 1.2 清理的纹理

| 属性 | 值 |
|------|-----|
| 名称 | `TSR.PrevAtomics` |
| 格式 | `PF_R32_UINT` (32-bit uint) |
| 类型 | `Texture2DArray` (perspective: 1 slice, orthographic: 2 slices) |
| 标记 | `TexCreate_UAV | TexCreate_ShaderResource | TexCreate_AtomicCompatible` |
| 尺寸 | `InputExtent`（低分辨率输入纹理尺寸） |

```cpp
FRDGTextureDesc Desc = FRDGTextureDesc::Create2DArray(
    InputExtent, PF_R32_UINT, FClearValueBinding::None,
    TexCreate_ShaderResource | TexCreate_UAV | TexCreate_AtomicCompatible,
    bIsOrthoProjection ? 2 : 1);
PrevAtomicTextureArray = GraphBuilder.CreateTexture(Desc, TEXT("TSR.PrevAtomics"));
```

### 1.3 线程与 Tile 布局

```
numthreads: 64 × 1 × 1  (即 8×8 = 64 线程)
每 Group 覆盖: 16 × 16 像素 (通过每线程处理 2×2 = 4 像素实现)
```

```hlsl
#define TILE_SIZE 8
#define TILE_ROWS 2
[numthreads(TILE_SIZE * TILE_SIZE, 1, 1)]  // 64 threads
```

### 1.4 坐标计算

```hlsl
uint2 GroupThreadId   = (GroupThreadIndex % 8, GroupThreadIndex / 8);
uint2 OutputPixelOrigin = InputInfo_ViewportMin + GroupId * 16 + GroupThreadId;
```

- `InputInfo_ViewportMin`: 输入视口的左上角像素坐标（来自 `View.ViewRect.Min`）
- `GroupId * 16`: 当前 group 的 tile 左上角像素坐标
- `GroupThreadId`: 线程在 8×8 内的位置

### 1.5 双层循环覆盖 4 像素

```hlsl
for (uint y = 0; y < 2; y++)
    for (uint x = 0; x < 2; x++)
        PixelOffset = (x, y) * 8;  // 乘 8 产生间隔
```

一个线程负责的 4 个像素分散在 tile 的 4 个 8×8 象限：

```
质心坐标: (0,0), (8,0), (0,8), (8,8)
```

**设计原因**: 只用于纯写零，无实际性能收益。但沿用与其他 TSR pass 一致的 tile 布局，保持工程一致性。

### 1.6 越界保护

```hlsl
InputPixelPos.x = select(all(InputPixelPos < InputInfo_ViewportMax), 
                         InputPixelPos.x, uint(~0));
```

超出视口范围的像素地址 X 设为 `~0` (UINT_MAX)，GPU 写到无效地址，硬件丢弃该写操作。

### 1.7 异步计算

当 `r.TSR.AsyncCompute >= 1` 时，此 pass 可在异步计算管线执行（不依赖其他帧资源）。

### 1.8 为什么不清理会导致错误

后续 `DilateVelocity` 使用 `InterlockedMax` 写入：

```hlsl
InterlockedMax(PrevAtomicOutput[PixelPos], PrevClosestDepth);
```

`InterlockedMax` 仅当新值 > 已有值时才替换。若不清理：
- 上一帧残留值可能大于当前值 → **原子写入被静默丢弃**
- 旧深度值污染 disocclusion 判断 → **漏判或误判**
- 旧 hole-filling velocity 用于当前帧 → **补洞速度错误**

---

## 2. Pass: DilateVelocity

**源文件**: `Engine/Shaders/Private/TemporalSuperResolution/TSRDilateVelocity.usf`
**C++ 调度**: `TemporalSuperResolution.cpp:2028`

### 2.1 概述

DilateVelocity 是 TSR 最核心的 pass 之一，负责：
1. 加载 3×3 深度/MV 邻域
2. 找到最近深度像素（depth dilation）
3. 计算膨胀后的运动矢量
4. 计算重投影边界（Reprojection Boundary）
5. 输出多种元数据纹理给后续 pass 使用

### 2.2 坐标映射：Z 序曲线 vs 线性映射

```hlsl
uint2 DispatchThreadId = ZOrder2D(GroupThreadIndex, log2(8))   // Morton 码映射
                       + GroupId * uint2(8, 8);
tsr_short2 InputPixelPos = InputInfo_ViewportMin + DispatchThreadId;
```

`ZOrder2D` 将一维线程索引按 Morton 码映射到 2D 坐标：

| ThreadIndex | 二进制 | ZOrder2D | 坐标 |
|-------------|--------|----------|------|
| 0 | 000000 | → | (0, 0) |
| 1 | 000001 | → | (1, 0) |
| 2 | 000010 | → | (0, 1) |
| 3 | 000011 | → | (1, 1) |

与 `ClearPrevTextures` 的线性 `(i%8, i/8)` 不同，Z 序给相邻线程更好的 2D 空间局部性，对原子散射写入更友好。

### 2.3 像素坐标转类型：tsr_short2

```hlsl
tsr_short2 InputPixelPos = tsr_short2(InputInfo_ViewportMin + DispatchThreadId);
```

`tsr_short2` 是跨平台适配的类型别名：

```hlsl
// 硬件支持 FP16 时：16 位有符号整数
#define tsr_short2 int16_t2

// 不支持时：32 位回退
#define tsr_short2 int2
```

像素坐标用 16 位存储足以覆盖 ±32768 范围，减少 VGPR 寄存器压力。

### 2.4 Butterfly 邻域采样

#### 2.4.1 核心机制

仅加载 4 个像素（而非 9 个），但 4 个相邻 lane 联合覆盖 3×3=9 个不同像素。

```hlsl
tsr_short2 GetLaneOffsetSign()
{
    return tsr_short(-1) + tsr_short2(
        tsr_ushort2(tsr_ushort(LaneIndex) << 1, LaneIndex) & tsr_ushort(0x2));
}

tsr_short2 Offset = Offsets2x2[ArrayIndex] * LaneOffsetSign;
```

| Lane | LaneOffsetSign | 加载的 4 个偏移 |
|------|---------------|----------------|
| 0 | (-1,-1) | (0,0)中心, (-1,0)左, (0,-1)上, (-1,-1)左上 |
| 1 | (+1,-1) | (0,0)中心, (+1,0)右, (0,-1)上, (+1,-1)右上 |
| 2 | (-1,+1) | (0,0)中心, (-1,0)左, (0,+1)下, (-1,+1)左下 |
| 3 | (+1,+1) | (0,0)中心, (+1,0)右, (0,+1)下, (+1,+1)右下 |

4 lane × 4 pixels = 16 loads, 去重后覆盖 9 个唯一像素 ✓

#### 2.4.2 数据交换：Wave Broadcast (无 LDS)

lane 间数据交换用 Wave 级别 DPP 指令，不走 LDS：

```hlsl
const FWaveBroadcastSettings Broadcast = InitWaveXorButterfly(XorValue);
return WaveBroadcast(Broadcast, Array[IndexInArray]);
```

底层是 GCN 的 `ds_swizzle` DPP 指令，单周期 lane 间 VGPR 寄存器交换。不需要 barrier 同步——wave 内 64 lane 本身是 lockstep 执行。

- `Xor=1`: 水平邻居 lane 交换
- `Xor=2`: 垂直邻居 lane 交换
- `Xor=3`: 对角邻居 lane 交换

不支持 DPP 的平台退化为 `CONFIG_BUTTERFLY_SAMPLES = 9`（每线程直接加载全部 9 个像素）。

### 2.5 屏幕坐标转换：ApplyScreenTransform

```hlsl
float2 ScreenPos = ApplyScreenTransform(float2(int2(InputPixelPos)), InputPixelPosToScreenPos);
```

本质是线性变换 `P * Scale + Bias`，将像素坐标转换为 NDC 空间 `[-1, 1]`：

```cpp
// C++ 侧构造
InputPixelPosToScreenPos = (Identity + 0.5f)    // 像素中心偏移
    * ChangeTextureBasisFromTo(TexelPosition → ScreenPosition);
```

`FViewScreenTransform` 本质上是 `float4(ScaleX, ScaleY, BiasX, BiasY)`，Scale/Bias 由视口尺寸和投影类型决定。

### 2.6 分支 1：bReprojectionField = false（简单路径）

对应 `r.TSR.ReprojectionField=0`（低质量/性能优先）。膨胀粒度为整像素。

```
FetchDepth3x3()          → 只加载 3×3 深度
FindClosestDepthOffset() → 找最近深度像素偏移
调用 FetchAndComputeScreenVelocity 两次:
  ① Offset=(0,0)      → 中心像素的屏幕速度
  ② Offset=最近邻居    → 膨胀后的屏幕速度
ComputeReprojectionEdge() → 比较两个速度差异 → 运动边界
EncodedReprojectionBoundary = 0   (不做空间反走样)
EncodedReprojectionJacobian = 0   (不做 Jacobian)
```

### 2.7 分支 2：bReprojectionField = true（高质量路径）

对应 `r.TSR.ReprojectionField=1`（High/Epic/Cinematic 画质自动启用）。膨胀粒度为亚像素。

```
FetchDepthVelocity3x3()            → 加载 3×3 深度 + MV
ComputePixelVelocityNeighborhood() → 转像素速度（省寄存器）
ComputeReprojectionJacobian()      → 算 2×2 Jacobian 矩阵
FindClosestDepthOffset()           → 找最近深度像素
AccessNeighborhoodCenter()         → 中心速度 (不用再采样)
FetchAndComputeScreenVelocity()    → 膨胀速度（仅一次采样）
ComputeReprojectionEdge()          → 运动边界
ComputeReprojectionBoundary()      → 深度空间反走样 → 亚像素边缘
```

### 2.8 FindClosestDepthOffset：找最近深度

#### 2.8.1 像素打包

```hlsl
uint PackDepthAndOffset(const int2 Offset, float DeviceZ)
{
    return ((asuint(DeviceZ) << 2) & ~0xF)     // 深度: 高 28 bits
         | (uint(1 + Offset.x) << 0)            // X 偏移: bit [1:0]
         | (uint(1 + Offset.y) << 2);           // Y 偏移: bit [3:2]
}
```

```
Bit:  31 30 ... 4 | 3  2 | 1  0
      ├ float深度 28bits ┤ ├ y+1 ┤ ├ x+1 ┤
```

用 `max()` 直接比较：深度在高位决定排序，偏移在低位仅在深度相等时影响。

#### 2.8.2 十字方向搜索

```hlsl
kPairOffsets[4] = {(1,0), (1,1), (0,1), (-1,1)};

for (uint i = 0; i < 4; i += 2)   // 只取 i=0 (左右) 和 i=2 (上下)
```

跳过 4 个对角方向是性能换质量的取舍——十字方向已足够判断最近表面。

#### 2.8.3 深度误差容差

```hlsl
float ComputePixelDeviceZError(float DeviceZ)
{
    float Depth = ConvertFromDeviceZ(DeviceZ);                    // deviceZ → 世界深度
    float PixelWorldRadius = GetDepthPixelRadiusForProjectionType(Depth); // 1像素世界半径
    float ErrorCorrectedDeviceZ = ConvertToDeviceZ(Depth + PixelWorldRadius * 2.0f);
    return abs(ErrorCorrectedDeviceZ - DeviceZ);  // deviceZ 的像素级精度极限
}
```

在比较邻域深度时做容差：

```hlsl
if (DepthVariation > max(DepthDiff * 0.25, DeviceZError))
```

小于 `DeviceZError` 的波动视为精度噪声忽略，避免把深度梯度误判为几何边缘。

### 2.9 ComputeReprojectionEdge：运动边界检测

```hlsl
tsr_half ComputeReprojectionEdge(float2 ScreenVelocity, float2 DilatedScreenVelocity)
{
    tsr_half2 Delta = tsr_half2((DilatedScreenVelocity - ScreenVelocity) * ScreenVelocityToInputPixelVelocity);
    tsr_half ReprojectionEdge = saturate(1.1 - dot(abs(Delta), 1.1));
    return ReprojectionEdge;
}
```

- `Delta`: 中心速度与膨胀速度之差（像素单位）
- `dot(abs(Delta), 1.1)` = `1.1 × (|Δx| + |Δy|)` → Manhattan 距离
- `1.1` 是缩放因子，阈值等效为 `|Δx| + |Δy| = 1`

| `|Δx| + |Δy|` | ReprojectionEdge | 含义 |
|--|--|--|
| 0 | 1.0 | 速度完全一致 → 安全复用历史 |
| 0.5 | 0.55 | 轻微差异 |
| >= 1.0 | 0.0 (saturate截断) | 运动边界 → 减少历史依赖 |

### 2.10 ComputeReprojectionBoundary：深度空间反走样

仅在 `bReprojectionField=true` 时计算。对深度边缘做空间反走样，输出 2D 向量 `[−1, 1]²` 描述像素相对于深度边缘的位置。

```
FindBrowsingDirection()       → 找到深度梯度的主要方向
BrowseNeighborhood()          → 沿边缘双向扫描，测 edge 长度
ComputeReprojectionBoundary() → 算像素到边缘的距离 + 方向
```

在后续 `TSRUpdateHistory` 中被解码用于**亚像素重投影修正**——如果像素在深度边缘附近（半覆盖），通过 boundary 知道该用中心 MV 还是膨胀 MV 来采样历史。

### 2.11 bOutputIsMovingTexture：防闪烁系统

由 `r.TSR.ShadingRejection.Flickering` 控制（默认开启）。额外计算 `IsMovingMask` 判断像素是否在运动：

- 检查世界空间位移是否超过 1 个像素的世界半径
- 检查 parallax velocity 是否超过阈值
- 有 pixel animation 的像素直接标记为运动

该标志写入 R8Output slice[2]，供 `TSRRejectShading` 做帧间亮度稳定性分析。

### 2.12 输出纹理

#### R8Output (TSR.DilateR8) — PF_R8_UINT, 2~3 slices

| Slice | 内容 | 编码 | 使用者 |
|-------|------|------|--------|
| 0 | DilateMask | bit[6:0]=ReproductionEdge, bit7=HasReprojectionOffset | TSRDecimateHistory |
| 1 | DeviceZError | 深度误差 (0~255) | TSRDecimateHistory |
| 2 | IsMovingMask | 闪烁检测 (仅 Flickering 开启) | TSRRejectShading |

#### ReprojectionFieldOutput (TSR.ReprojectionField) — PF_R32_UINT, 4 slices

| Slice | 内容 | 使用者 |
|-------|------|--------|
| 0 | 膨胀后的屏幕 MV | TSRDecimateHistory, TSRUpdateHistory |
| 1 | 2×2 Jacobian 矩阵 (仅 bReprojectionField) | TSRUpdateHistory |
| 2 | 亚像素边界 (仅 bReprojectionField) | TSRUpdateHistory |
| 3 | 降采样 MV (后续 pass 写入) | TSRUpdateHistory |

#### ClosestDepthOutput

| 通道 | 内容 |
|------|------|
| R | PrevClosestDeviceZ（最近深度减去膨胀深度的 Z 分量） |
| G | ClosestDeviceZ（最近深度） |

#### 输出写入：越界保护

```hlsl
uint2 OutputPixelPos = bValidOutputPixel ? InputPixelPos : uint(~0).xx;
```

`uint(~0).xx` = `(0xFFFFFFFF, 0xFFFFFFFF)`，无效像素写入 UINT_MAX 地址，GPU 丢弃该写。用分支预测替代 `if`，对 wave 执行更友好。

### 2.13 ScatterClosestOccluder：散射写入

```hlsl
ScatterClosestOccluder(PrevAtomicOutput, bValidOutputPixel, ScreenPos,
                        DilatedScreenVelocity.xy, PrevClosestDeviceZ);
```

内部将深度和 hole-filling velocity 打包后用 `InterlockedMax` 原子写入 `PrevAtomicOutput`：

| 写入内容 | 位置 | 说明 |
|----------|------|------|
| `f32tof16(PrevClosestDeviceZ)` | bit [33:18] | 最近遮挡物深度（半精度 float） |
| `EncodedHoleFillingVelocity` | bit [17:0] | 膨胀 MV 编码的补洞 velocity |

后续 `TSRDecimateHistory` 读回，在 disocclusion 区域用 hole-filling velocity 做补洞采样。

### 2.14 Wave 概念

Wave (AMD) = Warp (NVIDIA) = GPU 调度和执行的基本单位（64 或 32 线程）。

- **Lockstep 执行**: wave 内所有 lane 同时执行相同指令
- **私有寄存器**: 每个 lane 的 VGPR 是私有的，不共享
- **跨 lane 通信**: 通过 `WaveReadLaneAt` / DPP 指令显式按需拷贝，单周期完成

TSR 利用 wave intrinsics 做 lane 间数据交换（butterfly broadcast），避免使用 groupshared (LDS) 和显式 barrier，零延迟零同步。

### 2.15 越界写入保护

```hlsl
uint2 OutputPixelPos = bValidOutputPixel ? InputPixelPos : uint(~0).xx;
```

`uint(~0).xx` = `(0xFFFFFFFF, 0xFFFFFFFF)`。无效像素写入 UINT_MAX 地址，GPU 丢弃该写操作——用三元表达式替代显式 `if` 分支，对 wave 内分支发散更友好。

---

## 3. Pass: DecimateHistory

**源文件**: `Engine/Shaders/Private/TemporalSuperResolution/TSRDecimateHistory.usf`
**C++ 调度**: `TemporalSuperResolution.cpp:2082`

### 3.1 概述

DecimateHistory 是 TSR 重投影的核心 pass，负责：
1. 读取膨胀后的 MV 邻域，做更精确的运动边缘检测
2. 用膨胀 MV 重投影到上一帧，采样历史颜色和深度
3. 通过深度比对判断 disocclusion
4. 在 disocclusion 区域尝试补洞（hole-filling）
5. 输出重投影后的历史颜色、降采样 MV、像素状态标志位

### 3.2 输入纹理

| 纹理 | 来源 | 内容 |
|------|------|------|
| `DilatedReprojectionVectorTexture` | DilateVelocity ReprojectionFieldOutput slice[0] 或 slice[3] | 膨胀后的 MV |
| `DilateMaskTexture` | DilateVelocity R8Output slice[0] | ReprojectionEdge + HasReprojectionOffset |
| `DepthErrorTexture` | DilateVelocity R8Output slice[1] | DeviceZError |
| `ClosestDepthTexture` | DilateVelocity ClosestDepthOutput | `.r`=PrevClosestDeviceZ, `.g`=ClosestDeviceZ |
| `PrevAtomicTextureArray` | DilateVelocity 散射写入的 `TSR.PrevAtomics` | 散射的最近深度 + hole-filling velocity |
| `PrevHistoryGuide` | 上一帧 RejectShading 输出的 `TSR.History.Guide` | 历史帧引导颜色 |

### 3.3 坐标映射：Map8x8Tile2x2Lane

```hlsl
tsr_ushort2 InputPixelPos = Map8x8Tile2x2Lane(GroupThreadIndex)
    + tsr_ushort2(InputInfo_ViewportMin) + tsr_ushort2(GroupId) * tsr_ushort2(8, 8);
```

与 DilateVelocity 使用 `ZOrder2D` 不同，DecimateHistory 使用 `Map8x8Tile2x2Lane`（不同映射方式），但本质相同——将 1D 线程索引映射到 2D 像素坐标。

### 3.4 读取预处理数据

```hlsl
uint EncodedDilateMask = DilateMaskTexture[InputPixelPos];
uint EncodedDeviceZError = DepthErrorTexture[InputPixelPos];
float2 DeviceZAndPrevDeviceZ = ClosestDepthTexture[InputPixelPos];

bHasReprojectionOffset = (EncodedDilateMask & 0x80u) != 0;
PremilinaryReprojectionEdge = (EncodedDilateMask & 0x7Fu) / 127.0;
PrevDeviceZ = abs(DeviceZAndPrevDeviceZ.r);
DeviceZError = DecodeDeviceZError(PrevDeviceZ, EncodedDeviceZError);
```

将 DilateVelocity 编码在 R8 纹理中的元数据解码还原。

### 3.5 FetchReprojectionNeighborhood：读取膨胀 MV 邻域

```hlsl
FetchReprojectionNeighborhood(DilatedReprojectionVectorTexture, InputPixelPos,
    EncodedReprojectionVectorNeighborhood);
DilatedEncodedReprojectionVector = AccessNeighborhoodCenter(...);  // 中心膨胀 MV
```

读取 3×3 邻域的膨胀 MV（butterfly 采样），目的有两个：

1. **中心 MV 用于历史重投影**：`AccessNeighborhoodCenter` 取当前像素自己的膨胀 MV
2. **邻域 MV 用于精确边缘检测**：调用 9-samples 版 `ComputeReprojectionEdge`，对比中心与所有邻居的速度差异

```hlsl
ComputePixelVelocityNeighborhood(EncodedReprojectionVectorNeighborhood, PixelVelocityNeighborhood);
ReprojectionEdge = ComputeReprojectionEdge(PixelVelocityNeighborhood);  // 9-sample 版本

// 取两轮检测的保守值
ReprojectionEdge = select(PremilinaryReprojectionEdge > 0.9, 
    PremilinaryReprojectionEdge,     // DilateVelocity 的初步结果
    ReprojectionEdge);               // 本 pass 完整邻域分析

// 没有重投影偏移 → 不是边缘
ReprojectionEdge = select(bHasReprojectionOffset, ReprojectionEdge, 1.0);
```

### 3.6 LDS Spill：手动寄存器换 LDS

```hlsl
groupshared uint  SharedArray0[64];
groupshared float SharedArray1[64];

// 写入 LDS，释放 VGPR
SharedArray0[GroupThreadIndex] = DilatedEncodedReprojectionVector;
SharedArray1[GroupThreadIndex] = PrevDeviceZ;

// ========== ReprojectAllPrevTextures (消耗大量 VGPR) ==========

// 从 LDS 恢复
DilatedEncodedReprojectionVector = SharedArray0[GroupThreadIndex];
PrevDeviceZ = SharedArray1[GroupThreadIndex];
```

历史纹理采样（`ReprojectAllPrevTextures`）消耗大量 VGPR。先将变量暂存 LDS，编译器可回收 VGPR 供采样使用，采样完成后再恢复。LDS 延迟约 16~32 cycles，比 VGPR spill 到显存快。

### 3.7 ReprojectAllPrevTextures：历史重投影

#### 3.7.1 两步重投影

```hlsl
// ① 读 PrevAtomics 深度（双线性 4 点，低清 InputExtent 坐标系）
float2 ScreenPos = PixelsToScreen(InputPixelPos);
float2 PrevScreenPos = ScreenPos - DilatedMV;
LoadPrevAtomicTexturesSamples(PrevAtomicTextureArray, PrevScreenPos,
    HistoryClosestDeviceZSamples0[4]);  // 4 个角点深度

// ② 读 PrevHistoryGuide 颜色（CatmullRom 采样，低清 GuideArray 坐标系）
float2 OutputScreenPos = PixelsToReprojectScreen(InputPixelPos);
float2 PrevOutputScreenPos = OutputScreenPos - DilatedMV;
ReprojectedHistoryGuideSamples.FetchSamples(PrevHistoryGuide, PrevOutputScreenPos);
```

注意两个步骤使用**不同的 ScreenPos → 纹理坐标变换**——`PrevAtomics` 和 `PrevHistoryGuide` 虽然都在低清分辨率下，但纹理尺寸和视口参数可能因量化对齐不同，因此需要各自的 transform。Guide 颜色和 PrevAtomics 深度同属低清坐标系。

#### 3.7.2 LoadPrevAtomicTexturesSamples

将重投影后的 screen position 转为 PrevAtomics 缓冲的 UV，取**双线性采样的 4 个角点整像素**：

```hlsl
float2 PrevInputBufferUV = (ScreenPosToViewportScale * PrevScreenPos + Bias) / Extent;
FBilinearSampleInfos BilinearInter = GetBilinearSampleLevelInfos(PrevInputBufferUV, ...);

for (uint i = 0; i < 4; i++)
    HistoryClosestDeviceZSamples0[i] = PrevAtomicTextureArray[uint3(PixelPos, 0)];
```

每个采样点存打包的 `f16(深度) + hole-filling velocity`。

#### 3.7.3 CameraCut 处理

无历史时（首帧 / CameraCut）：
- C++ 侧：`PrevHistory.GuideArray = BlackArrayDummy`（全零纹理）
- Shader 侧：`bIsOffScreen = (bCameraCut != 0) || ...` → 标记为无效
- 采样结果全零，后续 pass 直接用当前帧颜色，不混入历史

### 3.8 颜色累积与曝光校正

```hlsl
// CatmullRom 多采样点合并
tsr_half4 RawGuide = ReprojectedHistoryGuideSamples.AccumulateSamples();
ReprojectedHistoryGuideColor = RawGuide.rgb;
ReprojectedHistoryGuideUncertainty = RawGuide.a;

// 还原曝光校正（历史帧存的是预曝光校正后的值）
ReprojectedHistoryGuideColor = CorrectGuideColorExposure(ReprojectedHistoryGuideColor, 
    HistoryPreExposureCorrection);
```

### 3.9 ProcessPrevAtomicTexturesSamples：Disocclusion 检测与补洞

#### 3.9.1 输入参数

```
ScreenPos ([-1,1])           = 当前像素 NDC 坐标
ScreenVelocity ([-1,1])      = 膨胀后的 MV（解码自 DilatedEncodedReprojectionVector）
PrevDeviceZ                  = ClosestDeviceZ - DilatedScreenVelocity.z（最近深度投射到上一帧）
HistoryClosestDeviceZSamples0[4] = 上一帧散射纹理双线性 4 采样点
DeviceZError                 = 深度误差容差
```

#### 3.9.2 Disocclusion 判断

```hlsl
// 4 个采样点逐一比对
for (uint i = 0; i < 4; i++)
{
    float HistoryClosestDeviceZ = f16tof32(SampleHistoryClosestDeviceZ >> 18);  // 解码深度
    float HistoryClosestWorldDepth = ConvertFromDeviceZ(HistoryClosestDeviceZ);
    
    float WorldDepthEpsilon = GetDepthPixelRadiusForProjectionType(HistoryClosestWorldDepth) * 3.0 * 2.0;
    float DeltaDepth = abs(HistoryClosestWorldDepth - WorldDepth);
    DepthRejection = saturate(2.0 - DeltaDepth / WorldDepthEpsilon);
    
    ParallaxRejectionMask += BilinearWeight * DepthRejection;  // 累加
}
```

比对 `PrevDeviceZ`（按 MV 推算的上一帧深度）和 `HistoryClosestDeviceZSamples0`（上一帧实际散射的最近深度）：
- 一致 → `DepthRejection ≈ 1.0` → 高 rejection mask → 正常重投影
- 不一致 → `DepthRejection ≈ 0.0` → 低 rejection mask → disocclusion

#### 3.9.3 补洞 MV 解码

```hlsl
// 4 个样本取 max（高位是深度，max 自然选中最近遮挡物对应的打包值）
EncodedHoleFillingVelocity = max(EncodedHoleFillingVelocity, SampleHistoryClosestDeviceZ);

// 解码 hole-filling velocity
DecodeHoleFillingVelocity(EncodedHoleFillingVelocity, angle, length);
HoleFillingPixelVelocity = CartesianHoleFillingVelocity(angle, length);  // 极坐标 → 像素坐标
```

补洞 MV 就是膨胀 MV 本身——在 `ScatterClosestOccluder` 中打包写入的相同值，只是经过极坐标编码/解码。

#### 3.9.4 补洞有效性判断

```hlsl
// 判断 1: 长度是否在可编码范围内
bool bIsEncodablePixelLength = HoleFillingPixelLength < GetMaxEncodableHoleFillingPixelLength();

// 判断 2: 至少一个采样点有效
bool bIsValidHoleFillingPixelVelocity;  // 至少一个 PixelPos 在视口内

bCanHoleFill = bIsValidHoleFillingPixelVelocity && bIsEncodablePixelLength;
```

编码使用固定 bit 位宽，长度超过可编码范围会被 `clamp` 截断。解码值是编码最大值 → 可能因溢出被截断，真实长度未知 → 不可靠，放弃补洞。

#### 3.9.5 补洞 MV 与当前 MV 交叉验证

```hlsl
// 如果补洞 MV 方向和大小 ≈ 当前膨胀 MV → 运动一致
if (bIsEncodablePixelLength)
{
    float PixelAngleDiff = abs(PixelVelocityAngle - HoleFillingPixelAngle);
    float PixelLengthDiff = abs(HoleFillingPixelLength - PixelVelocityLength) - 2.0;
    
    // 角度相似 → 提升 rejection mask
    ParallaxRejectionMask = max(ParallaxRejectionMask, CompareSimilarity(...));
}
```

两个速度相似意味着运动和周围一致，不是真正的遮挡边缘，提升 rejection mask 避免误判。

#### 3.9.6 补洞 MV 替换膨胀 MV

```hlsl
bReprojectionHollFill = bCanHoleFill && bIsParallaxDisocclusion;

if (bReprojectionHollFill)
{
    float2 HoleFillingReprojectionVector = HoleFillingPixelVelocity * InputPixelVelocityToScreenVelocity;
    DilatedEncodedReprojectionVector = EncodeReprojectionVector(HoleFillingReprojectionVector);
}
```

补洞原理：disocclusion 区域的像素在上帧被前景遮挡 → 用前景的 MV（即膨胀 MV/补洞 MV）去历史帧前景曾覆盖的位置采样 → 那里就是本应出现的背景颜色。

### 3.10 输出

#### 3.10.1 DecimateBitMask：像素状态标志位

```hlsl
tsr_ushort DecimateBitMask = 
    select(bIsOffScreen,             bit0) |
    select(bIsParallaxDisocclusion,  bit1) |
    select(bHasPixelAnimation,       bit2) |
    select(bReprojectionHollFill,    bit3) |
    select(bIsResurrectionOffScreen, bit4);
```

| Bit | 标志 | 含义 |
|-----|------|------|
| 0 | `bIsOffScreen` | 重投影越界/CameraCut |
| 1 | `bIsParallaxDisocclusion` | 视差遮挡 |
| 2 | `bHasPixelAnimation` | 像素动画 |
| 3 | `bReprojectionHollFill` | 补洞有效 |
| 4 | `bIsResurrectionOffScreen` | resurrection 越界 |

后续 `ResolveHistory` / `RejectShading` 根据这些标志决定如何混合历史。

#### 3.10.2 输出纹理汇总

| 输出 | 内容 | 使用者 |
|------|------|--------|
| `ReprojectedHistoryGuideOutput` | 重投影后的历史颜色 + uncertainty | ResolveHistory, RejectShading |
| `DecimateMaskOutput` | `.r`=DecimateBitMask/255, `.g`=ReprojectionEdge | ResolveHistory, RejectShading |
| `ReprojectionFieldOutput` | 降采样后的重投影 MV（补洞像素用补洞 MV 替换） | UpdateHistory |

#### 3.10.3 补洞像素对 ReprojectionFieldOutput 的特殊处理

```hlsl
// 始终写入 MV（补洞像素已替换为补洞 MV）
ReprojectionFieldOutput[kReprojectionVectorOutputIndex] = DilatedEncodedReprojectionVector;

// Jacobian 清零（补洞区域亚像素形变不可知，不做修正）
ReprojectionFieldOutput[kReprojectionJacobianOutputIndex] = EncodeReprojectionJacobian(0.0);
```

### 3.11 随机抖动量化（Stochastic Quantization）

```hlsl
uint2 Random = Rand3DPCG16(int3(InputPixelPos - ViewportMin, View.StateFrameIndexMod8)).xy;
tsr_half E = Hammersley16(0, 1, Random).x;

ReprojectedHistoryGuideColor = QuantizeForUNormRenderTarget(
    ReprojectedHistoryGuideColor, E, HistoryGuideQuantizationError);
```

原理：`Color = Color + QuantizationError * (E - 0.5)`，其中 `E ∈ [0,1]` 是伪随机数。

历史 guide 纹理存 HDR 颜色但格式精度有限（如 R11G11B10），直接截断产生条带噪声。抖动将量化误差转为每帧不同的高频噪声，TAA 多帧累积后平均收敛到正确值，等价于提升有效位深。

### 3.12 GuideColor 数据流

> **注意：Guide 颜色是低清分辨率的。** RejectShading 以 `InputRect.Size()` 派遣线程，写入 `History.GuideArray` 的视口区域。`History.GuideArray` 的物理 extent 可能因量化对齐稍大，但有效数据仅填充低清视口范围。

| 阶段 | Pass | 纹理 | 作用 |
|------|------|------|------|
| 当前帧使用 | **DecimateHistory** | `ReprojectedHistoryGuideOutput` | 从 PrevHistoryGuide 重投影得到中间结果（低清） |
| 下一帧使用 | **RejectShading** | `HistoryGuideOutput` → `History.GuideArray` | 混合当前帧与历史后写入低清视口区域，存为下帧 PrevHistoryGuide |

```
PrevHistoryGuide (上帧 RejectShading 存下的，低清)
  → DecimateHistory 重投影采样（低清坐标系）
  → ReprojectedHistoryGuide (中间结果，低清)
  → RejectShading: 当前帧 vs 历史判定混合（低清线程派遣）
  → History.GuideArray（低清有效区域） → QueueTextureExtraction
  → 下一帧的 PrevHistoryGuide（同样是低清）

注意：最终高清画面走的是 History.ColorArray（UpdateHistory pass 写入，输出分辨率），
Guide 只负责时序判定和亮度引导，始终运行在低清分辨率下。
```

### 3.13 历史帧复活（Resurrection）— DecimateHistory 阶段

#### C++ 侧：滚动帧管理与 ClipToResurrectionClip

`FTSRHistorySliceSequence`（`TemporalSuperResolution.cpp:1204`）管理多帧滚动缓冲区：

```cpp
struct FTSRHistorySliceSequence
{
    int32 FrameStorageCount;   // 槽位数（≥ 4，偶数）
    int32 FrameStoragePeriod;  // 持久帧存储间隔（奇数）
    int32 GetResurrectionFrameRollingIndex(AccumulatedFrameCount, LastFrameRollingIndex);
};
```

复活帧选择：环形缓冲中最老的持久帧 → `ceil((Last+Period)/Period)*Period % Total`。`FTSRHistory` 存储每帧的 `ViewMatrices` 用于重投影计算。

C++ 侧从复活帧的 `ViewMatrices` 构造 `ClipToResurrectionClip`：

```cpp
const FViewMatrices& PrevMatrices = InputHistory.ViewMatrices[ResurrectionFrameSliceIndex];
FVector DeltaTranslation = PrevPreViewTranslation - CurrentPreViewTranslation;
FMatrix InvViewProj = CurrentInvProjection * CurrentView.Transpose();
FMatrix PrevViewProj = DeltaTranslation * PrevView * PrevProjection;
ClipToResurrectionClip = InvViewProj * PrevViewProj;
// 包含完整相机平移，支持相机全自由度运动
```

#### Shader 侧：计算复活位置

```hlsl
// TSRDecimateHistory.usf:206-208
float4 ResurrectionClip = mul(ThisClip, ClipToResurrectionClip);
float2 ResurrectionScreen = ResurrectionClip.xy / ResurrectionClip.w;
// → 越界检测 → bIsResurrectionOffScreen
// → 采样复活帧 Guide 颜色 → 写入 ReprojectedHistoryGuideOutput 额外 slice
// → bIsResurrectionOffScreen → DecimateMaskOutput bit 4
```

---

## 4. Pass: RejectShading

**源文件**: `Engine/Shaders/Private/TemporalSuperResolution/TSRRejectShading.usf`
**C++ 调度**: `TemporalSuperResolution.cpp:2380`

### 4.1 概述

RejectShading 是 TSR 时序稳定性决策的最终关口，负责：
1. 读取当前帧颜色和历史重投影颜色
2. 通过卷积网络做时序分析（shading rejection）
3. 计算当前帧与历史帧的混合比例（RejectionBlendFinal）
4. 生成下一帧使用的 Guide 颜色

### 4.2 线程模型：Lane + SIMD + Tensor

```hlsl
[numthreads(LANE_COUNT * WAVE_COUNT, 1, 1)]  // 如 64×1 = 64 threads
```

与前几个 pass 不同，RejectShading 使用**卷积网络架构**的线程模型：

| 概念 | 说明 |
|------|------|
| **Lane** | 硬件执行槽位，1 thread = 1 lane |
| **LaneStride** | 每个 lane 覆盖的空间范围（如 2×2=4 像素） |
| **SIMD_SIZE** | 每个线程向量化处理的像素数（通常 4） |
| **tsr_tensor** | 打包 `SIMD_SIZE` 个向量的张量类型 |

```hlsl
// tsr_tensor 类型定义（本质是 TLaneVector2D）
tsr_tensor_halfC  = TLaneVector2D<tsr_half, C, LaneStrideX, LaneStrideY>  // C 通道 × 4 像素
tsr_tensor_bool   = TLaneVector2D<bool,    1, LaneStrideX, LaneStrideY>  // 4 个布尔
tsr_tensor_short2 = TLaneVector2D<tsr_short, 2, LaneStrideX, LaneStrideY> // 4 个 (x,y)
tsr_tensor_half   = TLaneVector2D<tsr_half, 1, LaneStrideX, LaneStrideY>  // 4 个标量
```

所有核心数据都以张量形式流转——每个变量包含 `SIMD_SIZE` 个像素的值，通过 `ElementIndex` 循环处理。

### 4.3 像素坐标批量计算

```hlsl
tsr_tensor_short2 ComputePixelPos()
{
    tsr_tensor_short2 PixelPos;
    for (uint i = 0; i < SIMD_SIZE; i++)
        PixelPos.SetElement(i, ComputeElementPixelPos(i));
    return PixelPos;  // 4 个相邻像素坐标
}
```

坐标计算使用 `TileOverscan`（tile 间重叠）和 `LaneStride`，每个 lane 负责 2×2 连续像素块，无遗漏无重复。输出坐标由 `ComputeElementOutputPixelPos` 钳位到 `InputInfo_ViewportMin ~ InputInfo_ViewportMax`。

### 4.4 输入纹理

| 纹理 | 来源 | 内容 |
|------|------|------|
| `InputTexture` | `PassInputs.SceneColor.Texture` | 当前帧低清不透明颜色 |
| `InputSceneTranslucencyTexture` | `PostDOFTranslucencyResources` | DOF 后的分离半透明 |
| `ReprojectedHistoryGuideTexture` | DecimateHistory 输出 | 重投影后的历史引导色 + uncertainty |
| `DecimateMaskTexture` | DecimateHistory 输出 | `.r`=BitMask, `.g`=ReprojectionEdge |
| `IsMovingMaskTexture` | DilateVelocity R8Output slice[2] | 闪烁检测标志 |
| `ClosestDepthTexture` | DilateVelocity ClosestDepthOutput | 最近深度 |

### 4.5 FetchDecimateMask：读取像素状态

```hlsl
void FetchDecimateMask(PixelPos, out VelocityEdge, out bIsDisoccluded, ...)
{
    DecimateMask = DecimateMaskTexture[PixelPos[i]];
    BitMask = DecimateMask.r * 255;
    bIsDisoccluded[i] = (BitMask & 0x3) != 0;       // bit 0: off-screen, bit 1: disocclusion
    bHasPixelAnimation[i] = (BitMask & 0x4) != 0;   // bit 2
    VelocityEdge[i] = DecimateMask.g;                // ReprojectionEdge
}
```

### 4.6 FetchSceneColorAndTranslucency：读取当前帧颜色

```hlsl
OriginalOpaqueInput[i] = InputTexture[PixelPos[i]];                 // 不透明颜色
OriginalTranslucencyInput[i] = InputSceneTranslucencyTexture[UV[i]]; // 半透明
```

`InputTexture` 是 Base Pass 的不透明渲染结果，`InputSceneTranslucencyTexture` 是 DOF 后单独渲染的半透明层——两者分离是因为半透明在 DOF 之后渲染，不能直接写回 SceneColor。

#### 两次合成：用途不同

**ComposeTranslucency**（输出用）：
```hlsl
CenterFinalSceneColor = Opaque * Alpha + Translucency;  // 标准合成
// → 写入 InputSceneColorOutput 和 LDR Luma
```

**ComposeTranslucencyForRejection**（时序判定用）：
```hlsl
if (bHasPixelAnimation) {
    SharpInput = 0;                               // 不透明清零（不可靠）
    BlurInput = 合成颜色;                         // 半透明用完整颜色替换
}
return ComposeTranslucency(SharpInput, Blur3x3(BlurInput)); // 半透明 3×3 模糊
```

**分离原因**：Pixel Animation 和半透明区域天生时序不稳定，直接用精确颜色比对会大量误判。`ComposeTranslucencyForRejection` 先把不稳定区域模糊化，让 rejection 更保守。

### 4.7 Pixel Animation

速度缓冲中的标志位，标记**像素着色自身在变化**而非物体移动（如 flipbook 序列帧、UV 滚动、程序化噪声动画）。历史重投影无法通过 MV 精确对齐这种变化，shading rejection 需特殊处理——检测到 PA 的像素，把不透明部分清零、半透明模糊化，降低时序比较灵敏度。

### 4.8 FetchReprojectedHistoryGuide：读取历史引导色

```hlsl
FetchHistoryGuide(ReprojectedHistoryGuideTexture, 
    ReprojectedHistoryGuideMetadataTexture, PixelPos,
    out History, out HistoryUncertainty);
```

两个纹理实际是同一纹理数组的不同 slice：
- 无 Alpha 路径：`ReprojectedHistoryGuideTexture.rgb`=颜色, `.a`=uncertainty
- 有 Alpha 路径：`ReprojectedHistoryGuideTexture.rgba`=颜色+Alpha, `MetadataTexture`=uncertainty

所有读取都是传入当前帧 `PixelPos`——历史已在 DecimateHistory 中重投影对齐，无需再做坐标变换。

### 4.9 ComputeSpatialAntiAliaserLumaLDR：生成 LDR 亮度

```hlsl
tsr_half ComputeSpatialAntiAliaserLumaLDR(SceneColor)
{
    PixelLuma = dot(SceneColor[i].rgb, (0.299, 0.587, 0.114));  // 亮度
    AALumaLDR[i] = PixelLuma / (0.5 + PixelLuma);                // HDR→LDR 压缩
}
```

写入 `InputSceneColorLdrLumaOutput`，供后续 `SpatialAntiAliasing` pass 做边缘检测。

### 4.10 Mutual Annihilation：双向互钳

将当前帧和历史帧的颜色互相钳位到对方的 3×3 邻域范围，消除单向偏差：

```hlsl
// 第一步：当前帧 3×3 min/max
MinMax3x3(ExposedInput, out InputMin, out InputMax);

// 第二步：历史被钳到当前帧的 3×3 邻域盒
ClampedHistory_initial = clamp(ExposedHistory, InputMin, InputMax);

// 第三步：当前帧被钳到"已被钳的历史"的 3×3 邻域盒
ClampedInput = Clamp3x3(ExposedInput, ClampedHistory_initial);

// 第四步：对称操作——历史被钳到"已被钳的当前帧"的 3×3 邻域盒
ClampedHistory = AnnihilateToGuide3x3(ExposedHistory, ExposedInput);
//              = Clamp3x3(History, Clamp3x3(Input, History))
```

核心函数：

| 函数 | 作用 |
|------|------|
| `MinMax3x3` | 可分离 1×3 水平 + 3×1 垂直 → 3×3 邻域各通道 AABB（轴对齐包围盒） |
| `Clamp3x3(ToClamp, BoundaryCenter)` | 以 BoundaryCenter 的 3×3 邻域 min/max 钳位 ToClamp |
| `AnnihilateToGuide3x3(ToClamp, Guide)` | 双层对称钳位，消除单向偏差 |

**MinMax3x3 原理**：可分离 min/max 滤波——先水平 1×3 取每列 min/max，再垂直 3×1 取全局 min/max。逐通道独立计算，输出的 min/max 向量可能来自不同邻居像素。

**邻近像素访问**：水平方向用 wave broadcast（Xor=1），垂直方向若跨 wave 则用 LDS 交换。每个 element 独立算自己的 3×3 邻域，tensor 的 lane 交错存储保证了正确空间对应。

### 4.11 MeasureRejection：时序一致性判定（核心算法）

```hlsl
MeasureRejection(
    InputC0,            // 当前帧原始色（未互钳）
    ClampedInput,       // 当前帧互钳色
    ClampedHistory,     // 历史帧互钳色
    MoireError,
    out RejectionBlendFinal,  // 1.0=信历史, 0.0=信当前
    out RejectionClampBlend); // clamp 力度
```

#### 算法 8 步

**① 模糊历史**：
```hlsl
FilteredHistory = Blur3x3(HistoryC2);
```

**② 计算 TotalVariation（区域不稳定性）**：
```hlsl
TotalVarInputDiffC0C2 = abs(TotalVariation3x3(|InputC0 - InputC2|));  // 互钳改变量
TotalVarInputC2 = abs(TotalVariation3x3(InputC2));                    // 空间复杂度
```

**③ 计算 ClampError（容差带）**：
```hlsl
ClampError = LDR量化误差;
ClampError = max(ClampError, Blur3x3(min(TotalVarInputDiffC0C2, TotalVarInputC2)));
ClampError = max(ClampError, InputC2BoxSize * FilteringWeight * 0.25);
ClampError = ClampError + LDR量化误差;
// 结果：局部越复杂、变化越大 → 容差越宽
```

**④ 构建钳位盒**：
```hlsl
FilteredInput = Blur3x3(InputC2);
MinMax3x3(FilteredInput, out BoxMin, out BoxMax);
BoxMin -= ClampError; BoxMax += ClampError;  // 安全盒
```

**⑤ 钳位历史到安全盒**：
```hlsl
ClampedFilteredHistory = clamp(FilteredHistory, BoxMin, BoxMax);
```

**⑥（可选）摩尔纹修正**：用 `MoireError` 进一步扩展钳位盒。

**⑦ 计算 Rejection**：
```hlsl
BoxSize = InputC2BoxSize * FilteringWeight + LDR量化误差 * 2 * FilteringWeight;
Delta = max(|FilteredInput - FilteredHistory|, BoxSize);

RawClampedEnergy = |ClampedFilteredHistory - FilteredHistory|;  // 历史被钳了多少
RawRejection = min_over_channels( saturate(1.0 - RawClampedEnergy / Delta) );
```

| RawClampedEnergy | RawRejection | 含义 |
|---|---|---|
| ≈ 0（不需要钳） | **1.0** | 历史和当前一致 → 信历史 |
| 很大（需要大力钳） | **0.0** | 历史越界严重 → 信当前 |

**⑧ 空间平滑**：
```hlsl
OutRejectionClampBlend = MaxRGBMinA3x3(Median3x3(RawRejection));
FilteredClampedEnergy = MaxRGBMinA3x3(Median3x3(RawClampedEnergy));
FilteredRejection = min_over_channels( saturate(1.0 - FilteredClampedEnergy / Delta) );
OutRejectionBlendFinal = FilteredRejection;
```

Median3x3 中值滤波去噪，MaxRGB 取保守值。核心思想：**钳得越多 → 历史越不可信 → 多用当前帧**。

### 4.12 UpdateGuide：生成下一帧 Guide 颜色

```hlsl
FinalGuide = lerp(ExposedHistory, ExposedInput, BlendFinal);
```

其中 `BlendFinal` 由 `RejectionBlendFinal` 推导：

```hlsl
BlendFinal = max(
    TheoricBlendFactor,                  // 理论最小 blend（保证历史至少占一点权重）
    1.0 - RejectionBlendFinal,           // rejection 越高 → blend 越低 → 越多历史
    select(bIsDisoccluded, 1.0, ...));   // disocclusion → 全信当前
```

管线：`Input → AddPerceptionAdd / History → GCSToLinear → UpdateGuide → LinearToGCS → RemovePerceptionAdd → FinalGuide → HistoryGuideOutput`

### 4.13 随机抖动量化

```hlsl
uint2 Random = Rand3DPCG16(int3(LaneSimdPixelOffset, View.StateFrameIndexMod8)).xy;
tsr_half E = Hammersley16(0, 1, Random).x;
FinalGuideColor.rgb = QuantizeForUNormRenderTarget(FinalGuideColor.rgb, E, HistoryGuideQuantizationError);
```

和 DecimateHistory 同样机制——将量化误差转为每帧不同的高频噪声，TAA 累积后收敛。

### 4.14 输出纹理汇总

| 输出 | 内容 | 使用者 |
|------|------|--------|
| `HistoryGuideOutput` | 下一帧用的 Guide 颜色 + uncertainty | DecimateHistory |
| `HistoryRejectionOutput` | 每个像素的 Rejection 值 | UpdateHistory |
| `InputSceneColorOutput` | 合成的完整当前帧颜色（不透明+半透明） | UpdateHistory |
| `InputSceneColorLdrLumaOutput` | LDR 亮度 | SpatialAntiAliasing |
| `ReprojectionFieldOutput` | 若有 resurrection，覆盖 MV 和 Jacobian | UpdateHistory |
| `AntiAliasMaskOutput` | 空间反走样遮罩 | SpatialAntiAliasing |

### 4.15 历史帧复活（Resurrection）— RejectShading 阶段

每像素比较普通历史（N-1 帧）和复活历史（N-K 帧）谁更接近当前帧：

```hlsl
PrevMatch         = |当前帧 - 普通历史|;
ResurrectionMatch = |当前帧 - 复活历史|;

bResurrectionIsCloser = (Sum3x3(PrevMatch - ResurrectionMatch) > 阈值)
                     && !bIsResurrectionOffScreen;

// 对复活历史独立跑 MeasureRejection
MeasureRejection(Input, ClampedResurrectedInput, ClampedResurrectedHistory, ...);
bResurrectHistory = ShouldResurrectHistory(RejectionBlendFinal, ResurrectionRejectionBlendFinal, bIsCloser);

// 胜出者替换历史
History = select(bResurrectHistory, ResurrectedHistory, History);
```

**每帧都跑，不是仅 CameraCut**——正常帧时普通历史颜色更接近自动胜出，CameraCut 时复活帧胜出。结果编码到 `HistoryRejection` bit 1，UpdateHistory 据此切换 `FrameIndex`。

### 4.16 关键设计点总结

| 概念 | 说明 |
|------|------|
| **张量化计算** | `tsr_tensor` 打包 4 像素并行处理，卷积网络减少指令 fetch |
| **Lane/LDS 双重邻居访问** | 水平邻域用 wave broadcast（无延迟），跨 wave 垂直邻域用 LDS |
| **互钳消除偏差** | 当前和历史双向钳位，任何只在一帧出现的颜色被"湮灭" |
| **容差随复杂度自适应** | 局部颜色变化大 → ClampError 大 → 更宽容，避免误判 |
| **Guide 低清存储** | 仅用于时序判定，低清精度足够，省带宽 |
| **两次合成分离** | 输出用精确合成，rejection 用模糊合成——判定保守但输出精确 |

---

## 5. Pass: SpatialAntiAliasing

**源文件**: `Engine/Shaders/Private/TemporalSuperResolution/TSRSpatialAntiAliasing.usf`
**C++ 调度**: `TemporalSuperResolution.cpp:2423`

### 5.1 概述

TSR 的时序抗锯齿在 disocclusion、高速运动、CameraCut 场景下失效。SpatialAntiAliasing 在这些区域做**空域边缘定向反走样兜底**——检测 LDR 亮度中的锯齿边缘，算出亚像素采样偏移，供 UpdateHistory 调整对当前帧低清颜色的采样位置。

### 5.2 输入

| 纹理 | 来源 | 内容 |
|------|------|------|
| `AntiAliasMaskTexture` | RejectShading 输出 | 标记需要反走样的像素 |
| `InputSceneColorLdrLumaTexture` | RejectShading 输出 | LDR 亮度（用于边缘检测） |

分辨率：低清（`GetGroupCount(InputRect.Size(), 8)`）。

### 5.3 算法

和 DilateVelocity 的 ComputeReprojectionBoundary 相同的三件套，输入从深度图换成 LDR 亮度图：

```hlsl
// ① 读取 3×3 LDR 亮度邻域
InputC, InputN, InputS, InputE, InputW, InputNE, InputNW, InputSE, InputSW;

// ② 找边缘方向
FindBrowsingDirection(InputC, N, S, E, W, NE, NW, SE, SW,
    out NoiseFiltering, out BrowseDirection, out EdgeSide, out EdgeLuma);

// ③ 沿边缘扫描
BrowseNeighborhoodBilinearOptimized(InputSceneColorLdrLumaTexture, InputC, EdgeLuma, ...);

// ④ 算亚像素偏移
ComputeReprojectionBoundary(BrowseDirection, EdgeLengthP, EdgeLengthN, ...);
// → AntiAliasingOutput[Pixel] = EncodedOffset ∈ [-1,1]²
```

### 5.4 UpdateHistory 中的使用

同一低清像素的偏移被多个高清输出像素共享，但效果不同——因为偏移的是 `InputPPCo`（浮点坐标）：

```hlsl
// 读取偏移
SpatialAntiAliasingOffset = DecodeSpatialAntiAliasingOffset(AntiAliasingTexture[...]);

// 仅在无历史时全量应用
SpatialAntiAliasingLerp = select(bIsOffScreen || bIsDisoccluded, 1.0, saturate(1.0 - Rejection * 4.0));

// 偏移 InputPPCo（浮点！）
InputPPCo += SpatialAntiAliasingOffset * SpatialAntiAliasingLerp;

// 重新计算最近整数像素
InputPPCk = floor(InputPPCo) + 0.5;
```

同一偏移作用于不同子像素位置时，有的跨越整数边界（改变了实际采样点），有的不变——自然产生差异化效果。

---

## 6. Pass: UpdateHistory

**源文件**: `Engine/Shaders/Private/TemporalSuperResolution/TSRUpdateHistory.usf`
**C++ 调度**: `TemporalSuperResolution.cpp:2613`

### 6.1 概述

UpdateHistory 是 TSR 的最终输出 pass，负责：
1. 用重投影 MV 采样高清历史帧颜色（Catmull-Rom 双三次滤波）
2. 对当前低清帧做多采样点空间滤波（上采样核）
3. 根据 rejection 数据加权混合历史与当前帧
4. 输出最终高清画面，同时存入 `History.ColorArray` 作为下一帧的历史

### 6.2 线程模型：高清 + 双像素向量化

```cpp
FComputeShaderUtils::GetGroupCount(HistorySize, 8);  // HistorySize = 输出分辨率
```

```hlsl
// DPV 模式（双像素向量化，FP16 硬件）
[numthreads(32, 1, 1)]  // TILE_SIZE * TILE_SIZE / 2 = 32 threads
// 1 thread = 2 个 history 像素

// 普通模式
[numthreads(64, 1, 1)]  // TILE_SIZE * TILE_SIZE = 64 threads
// 1 thread = 1 个 history 像素
```

DPV 模式下坐标用 `tsr_short2x2 HistoryPixelPos` 存 2 个像素，后续计算用 `dpv_*` 宏并行处理两个像素。

### 6.3 InputPPCo：输出像素映射到低清输入

```hlsl
ScreenPos = ApplyScreenTransform(HistoryPixelPos, HistoryPixelPosToScreenPos);
   // → NDC [-1,1]，用于几何计算

InputPPCo = ApplyScreenTransform(HistoryPixelPos, HistoryPixelPosToInputPPCo);
   // → 低清像素坐标（浮点），用于采样低清纹理
```

`InputPPCo` 就是高清输出像素对应到低清输入图像中的浮点坐标（如 `50.3, 25.7`），是 C++ 侧预合并的一次性变换。`InputPPCk = floor(InputPPCo) + 0.5` 找到最近的整数输入像素中心。

### 6.4 数据读取

每个输出像素读取：

```hlsl
// ① 反走样偏移（低清 AntiAliasingTexture，来自 SpatialAntiAliasing）
RawEncodedInputTexelOffset = AntiAliasingTexture[LocalInputPixelPos];

// ② MV（低清 ReprojectionVectorTexture，可能已被补洞 MV 替换）
RawEncodedReprojectionVector = ReprojectionVectorTexture[LocalInputPixelPosWithReprojectionAA];

// ③ Rejection 权重（低清 HistoryRejectionTexture，来自 RejectShading）
RawHistoryRejection = HistoryRejectionTexture[LocalInputPixelPos];
// .r = LowFrequencyRejection, .g = DisableHistoryClamp, .b = DecreaseValidityMultiplier
// .a bitmask: bit0 = bIsParallaxRejected, bit1 = bIsHistoryResurrection

// ④ Reprojection Boundary（低清，仅 bReprojectionField）
EncodedReprojectionBoundary = ReprojectionBoundaryTexture[LocalInputPixelPos];
```

### 6.5 Reprojection Boundary：亚像素深度边缘偏移

从深度空间反走样结果解码，判断历史像素是否在膨胀侧，决定 MV 的采样偏移方向：

```hlsl
BoundaryDilateOffset = select(
    IsHistoryPixelWithinOffsetBoundary(dInputKO, ReprojectionBoundary),
    +ReprojectionOffset,   // 在膨胀侧：用膨胀 MV → 偏移到最近深度像素读 MV
    -ReprojectionOffset);  // 在中心侧：用中心 MV
```

### 6.6 Jacobian：亚像素 MV 修正

Jacobian `J²ˣ²` 编码了输入像素内 MV 随子像素位置的变化率，修正输出像素的 MV：

```hlsl
JacobianCoordinate = dInputKO - BoundaryDilateOffset;
ReprojectionPixelPosCorrection = mul(JacobianCoordinate, ReprojectionJacobian);
LocalReprojectionVector = DecodedMV + ReprojectionScreenPosCorrection;
```

同一输入像素覆盖多个输出像素 → MV 应有微调 → Jacobian 修正。**修正的是 MV，不是像素坐标**。

### 6.7 高清历史采样：Catmull-Rom 双三次

```hlsl
PrevHistoryBufferUV = ApplyScreenTransform(PrevScreenPos, ScreenPosToPrevHistoryBufferUV);
for (uint i = 0; i < 16; i++)
{
    PrevHighFrequency += BilinearSampleColorHistory(PrevHistoryColorTexture, UV[i], FrameIndex)
                       * KernelWeight[i] * PreExposureCorrection;
}
```

从 `PrevHistoryColorTexture`（高清多帧纹理数组）的对应帧采样，双三次 Catmull-Rom 16 个采样点加权合并。若产生负值（HDR 采样常见问题），回退到最近采样点。

**复活帧索引切换**：若 `bIsHistoryResurrection` 为 true（由 RejectShading 判定），采样时使用 `ResurrectionFrameIndex` 而非普通 `PrevFrameIndex`：

```hlsl
bIsHistoryResurrection = (HistoryRejection.a * 255 & 0x2) != 0;
float FrameIndex = select(bIsHistoryResurrection, ResurrectionFrameIndex, PrevFrameIndex);
```

### 6.8 当前帧空间滤波：多采样上采样核

```hlsl
for (uint SampleId = 0; SampleId < CONFIG_SAMPLES_COUNT; SampleId++)
{
    ComputeInputKernelSamplePosition(InputPixelPos, dInputKO, SampleId, ...);
    InputColor = InputSceneColorTexture[SamplePixelPos[i]];
    ToneWeight = HdrWeight4(InputColor);
    FilteredInputColor += SampleSpatialWeight * ToneWeight * InputColor;
}
```

不是简单双线性，而是在低清帧的多个采样点上加权滤波。权重：
- **空间权重**：基于采样点到输出像素的距离
- **HDR Tone Weight**：亮度越高权重越大，防止 HDR 亮点在混合中丢失
- 采样点采集 min/max 构建 clamp 盒，用于后续钳位历史色

### 6.9 核宽度自适应

```hlsl
KernelInputToHistoryFactor = lerp(
    1.0 - 0.5 * NoiseFiltering,   // 不可靠 → 窄核
    InputToHistoryFactor,         // 可靠 → 宽核（上采样拉伸）
    LowFrequencyRejection > 阈值);
```

Rejection 高（历史可靠）→ 宽核 → 平滑上采样。Rejection 低 → 窄核 → 不依赖历史。

### 6.10 历史钳位

```hlsl
ClampedPrevHighFrequencyColor = clamp(PrevHighFrequencyColor, InputMinColor, InputMaxColor);
BlendedPrevHighFrequencyColor = HdrWeightLerp(
    ClampedPrevHighFrequencyColor, PrevHighFrequencyColor, DisableHistoryClamp);
```

ghosting → 钳回。`DisableHistoryClamp` 控制钳位力度。

### 6.11 运动压制 Validity

```hlsl
MaxValidity = 1.0 - WeightClampingPixelSpeedAmplitude * saturate(OutputPixelVelocity * InvWeightClampingPixelSpeed);
PrevWeight = min(PrevWeight, MaxValidity);
```

运动越快 → Validity 越低 → 历史权重降低 → 保持运动清晰度。

### 6.12 亮度对比稳定增强

```hlsl
MinValidityForStability = |FilteredLuma - PrevHistoryLuma| / max(FilteredLuma, PrevHistoryLuma);
MaxValidity = max(MaxValidity, MinValidityForStability);
```

亮差大 → 提升 min validity → 允许更多历史 → 防闪烁。

### 6.13 最终混合

```hlsl
FinalHighFrequencyColor = (
    BlendedPrevHighFrequencyColor * (PrevWeight * PrevHistoryToneWeight) +
    FilteredInputColor * (CurrentWeight * FilteredInputToneWeight)
) / CommonWeight;
```

HDR 感知的加权混合，双方各有独立 tone weight。

### 6.14 输出

| 输出 | 纹理 | 用途 |
|------|------|------|
| `HistoryColorOutput` | `History.ColorArray` | 高清历史颜色，下帧 PrevHistoryColorTexture |
| `HistoryMetadataOutput` | `History.MetadataArray` | 高清历史元数据 |
| `UpdateHistoryTextureSRV` | `History.ColorArray` SRV | 当前帧画面输出（或经 ResolveHistory 降采样） |

### 6.15 ResolveHistory

若 `HistorySize > OutputRect.Size()` → 额外降采样到输出分辨率。否则直接输出。

---

## 7. Pass: ResolveHistory

**源文件**: `Engine/Shaders/Private/TemporalSuperResolution/TSRResolveHistory.usf`
**C++ 调度**: `TemporalSuperResolution.cpp:2700`

### 7.1 概述

纯空域降采样 pass，仅在 `HistorySize > OutputRect.Size()` 时运行（如 `r.TSR.History.ScreenPercentage=200`）。将高清历史缓冲区降采样到输出分辨率，不涉及任何时序数据。

### 7.2 线程模型与分辨率

线程派送覆盖输出分辨率：

```hlsl
[numthreads(LANE_COUNT, 1, 1)]  // wave 大小，如 16/32
```

### 7.3 核心算法：Mitchell-Netravali 三次卷积降采样

不是简单邻域平均，而是 4×4 邻域 Mitchell-Netravali 滤波：

```
每个输出像素对应 4×4 个高清历史像素，分 4 个象限:

    c  b | b  c
    b  a | a  b
    ──── o ────     o = 输出像素中心
    b  a | a  b
    c  b | b  c
```

Mitchell-Netravali 权重（三次样条曲线，平衡锐度和平滑）：

```hlsl
WeightA = MitchellNetravali(0.5, B, C);  // 距离 0.5 像素
WeightB = MitchellNetravali(1.5, B, C);  // 距离 1.5 像素
// 归一化后得 FinalWeightA, FinalWeightB, FinalWeightC
```

4 个象限各自加权点积，再用 wave broadcast 合并：

```hlsl
Out0 = DownsampleDot2x2(LDRColors, {C, B, B, A});  // 左上
Out1 = DownsampleDot2x2(LDRColors, {B, C, A, B});  // 右上
Out2 = DownsampleDot2x2(LDRColors, {B, A, C, B});  // 左下
Out3 = DownsampleDot2x2(LDRColors, {A, B, B, C});  // 右下

Output = Out0 + WaveBroadcast(Out1, X+1) + WaveBroadcast(Out2, Y+1) + WaveBroadcast(Out3, XY+1);
```

### 7.4 附加处理

- **HDR Tone Weight**：采样前用 `HdrWeight4(color)` 对亮部加权，结果用 `HdrWeightInvY(luma)` 恢复——防止 bright spot 在降采样中消失
- **Min/Max Clamp**：对 2×2 块取 min/max 后再对邻居做 2×2 扩展，clip 最终结果防止 ghosting/overshoot

### 7.5 输出

`SceneColorOutputTexture`——直接输出到屏幕或后续后处理。

---

## 8. TSR 整体数据流总览

```
Pass                       输出纹理                     后续使用者 (R=只读, W=只写, RW=读写)
─────────────────────────────────────────────────────────────────────────────────────────────────
ClearPrevTextures          TSR.PrevAtomics             → DilateVelocity.PrevAtomicOutput        (RW InterlockedMax)
                                                       → DecimateHistory.PrevAtomicTextureArray  (R)


DilateVelocity             ClosestDepthOutput           → DecimateHistory.ClosestDepthTexture    (R)
                                                       → RejectShading.ClosestDepthTexture       (R)

                           R8Output
                             slice[0]: DilateMask       → DecimateHistory.DilateMaskTexture      (R)
                             slice[1]: DeviceZError     → DecimateHistory.DepthErrorTexture      (R)
                             slice[2]: IsMovingMask     → RejectShading.IsMovingMaskTexture      (R)

                           ReprojectionFieldOutput
                             slice[0]: 膨胀 MV          → DecimateHistory.DilatedReprojectionVecTex (R)
                                                        → UpdateHistory.ReprojectionVectorTex     (R)
                             slice[1]: Jacobian         → UpdateHistory.ReprojectionJacobianTex   (R)
                             slice[2]: 边界              → UpdateHistory.ReprojectionBoundaryTex    (R)
                             slice[3]: 降采样 MV          ← DecimateHistory  (W) → UpdateHistory (R)


DecimateHistory            ReprojectedHistoryGuideOutput → RejectShading.ReprojectedHistoryGuideTex (R)
                             slice[0]: GuideColor                                       (复活 slice: R)
                             slice[1]: Uncertainty

                           DecimateMaskOutput (.r=BitMask, .g=Edge)
                                                       → RejectShading.DecimateMaskTexture       (R)

                           ReprojectionFieldOutput       ← DecimateHistory (W, slice[0]或[3])
                             (降采样 MV, 补洞 MV 替换)   → UpdateHistory.ReprojectionVectorTex     (R)
                             (Jacobian 清零)             → UpdateHistory.ReprojectionJacobianTex   (R)


RejectShading              History.GuideArray            ← RejectShading (W) → 下帧 DecimateHistory.PrevHistoryGuide    (R)
                           HistoryRejectionOutput         ← RejectShading (W) → UpdateHistory.HistoryRejectionTex        (R)
                           InputSceneColorOutput          ← RejectShading (W) → UpdateHistory.InputSceneColorTex          (R)
                           InputSceneColorLdrLumaOutput   ← RejectShading (W) → SpatialAntiAliasing.InputLdrLumaTex       (R)
                           AntiAliasMaskOutput            ← RejectShading (W) → SpatialAntiAliasing.AntiAliasMaskTex      (R)


SpatialAntiAliasing        AntiAliasingOutput             ← SpatialAntiAliasing (W) → UpdateHistory.AntiAliasingTex     (R)


UpdateHistory              History.ColorArray             ← UpdateHistory (W) → 下帧 UpdateHistory.PrevHistoryColorTex (R)
                             (也作为当前帧画面输出)        → ResolveHistory.UpdateHistoryOutputTex       (R) [HistorySize>OutputRect时]
                            History.MetadataArray          ← UpdateHistory (W) → 下帧 UpdateHistory.PrevHistoryMetadataTx(R)


ResolveHistory             SceneColorOutputTexture        ← ResolveHistory (W)
                            (仅 HistorySize > OutputRect 时运行)  → 屏幕 / 后续后处理
                            输入: UpdateHistory.ColorArray SRV  (R)
                            算法: Mitchell-Netravali 4×4 降采样
                            分辨率: OutputRect (输出分辨率)
```

### 跨帧数据流转

```
帧 N:
  RejectShading  → History.GuideArray  → QueueExtract → 下帧 InputHistory.GuideArray
  UpdateHistory  → History.ColorArray  → QueueExtract → 下帧 InputHistory.ColorArray

帧 N+1:
  DecimateHistory ← InputHistory.GuideArray   (PrevHistoryGuide, 低清, 重投影采 reference)
  UpdateHistory   ← InputHistory.ColorArray   (PrevHistoryColorTexture, 高清, Catmull-Rom 采样)
```
