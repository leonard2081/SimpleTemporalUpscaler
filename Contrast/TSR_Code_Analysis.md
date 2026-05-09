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
