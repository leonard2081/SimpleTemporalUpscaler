# SGSR2（Snapdragon Game Super Resolution 2）完整技术分析文档

---

## 目录

1. [架构概览](#架构概览)
2. [变体一：2-Pass Compute Shader](#变体一2-pass-compute-shader)
3. [变体二：2-Pass Fragment Shader](#变体二2-pass-fragment-shader)
4. [变体三：3-Pass Compute Shader](#变体三3-pass-compute-shader)
5. [三变体核心差异对比](#三变体核心差异对比)
6. [性能数据](#性能数据)
7. [设计原理](#设计原理)

---

## 架构概览

SGSR2 是针对 Adreno GPU 设计的时域超分辨率方案，结合了三项核心机制：

1. **时域积累**：通过运动向量将前一帧颜色重投影到当前帧，累积多帧信息
2. **Lanczos 空间上采样**：用自适应核在 3×3 邻域做低分到高分的重建
3. **方差颜色包围盒**：在 YCoCg 空间统计局部颜色分布，裁剪出盒的历史色以防重影

所有变体共享的核心算法组件：

| 组件 | 作用 |
|------|------|
| **DepthClip** | 通过深度比较检测 disocclusion（新暴露表面），压低历史权重 |
| **MV 重投影** | 将当前像素映射回上一帧位置，读取历史颜色 |
| **Lanczos 上采样** | 用自适应核将低分输入重建为高输出 |
| **方差包围盒** | 在 YCoCg 空间计算 min/max/σ，限制历史颜色范围 |
| **时域混合** | 以可配置权重混合历史和当前颜色 |
| **YCoCg ↔ RGB** | 在去相关的色彩空间工作，改善色度/亮度分离 |
| **逆 Tone-map** | 从 LDR [0,1] 恢复 HDR 输出 |

### 三种变体的定位

```
速度 ◀──────────────────────────────────────────────−▶ 质量

  2-pass FS           2-pass CS              3-pass CS
  （最简）            （均衡）                （完整）
  - RGB 直通         - YCoCg 管线            - YCoCg 管线
  - 5 tap 固定       - 5/9 tap 自适应        - 9 tap 固定
  - 无 Wfactor        - Wfactor 透传          - Wfactor 闭环
  - 无 HDR 还原       - 逐像素 HDR            - Luma 追踪
  - 无透明处理        - 无透明处理            - 透明感知
```

---

## 变体一：2-Pass Compute Shader

**文件路径**：`glsl_2_pass_cs/`

### 管线概览

```
Pass 1: Convert (低分, 每线程1像素)    →    Pass 2: Upscale (高分, 每线程1输出像素)

InputColor  ──┐                             MotionDepthClipAlphaBuffer ◀── Convert
InputDepth  ──┤                             YCoCgColor                ◀── Convert
InputVelocity─┤                             PrevHistoryOutput         ◀── 上一帧 Upscale
              ├──▶ MotionDepthClipAlphaBuffer
              ├──▶ YCoCgColor               ──▶ SceneColorOutput (最终 RGBA16F)
              └──▶                          ──▶ HistoryOutput (下一帧的 PrevHistory)
```

每帧 2 次 dispatch：**Convert → Upscale**。

---

### Pass 1: Convert (`sgsr2_convert.comp`)

#### 输入纹理

| 纹理名称 | 分辨率 | 格式 | 用途 |
|----------|--------|------|------|
| `InputColor` | Render | RGBA (HDR) | 当前帧颜色（已乘 preExposure） |
| `InputDepth` | Render | D24S8 | 当前帧深度 |
| `InputVelocity` | Render | RGBA | 编码的动态 MV（静物为 0） |

#### 输出纹理

| 纹理名称 | 分辨率 | 格式 | 编码 |
|----------|--------|------|------|
| `YCoCgColor` | Render | R32UI | `[Y:11\|Co:11\|Cg:10]` 打包 uint32 |
| `MotionDepthClipAlphaBuffer` | Render | RGBA16F | `.xy=motion`, `.z=depthclip`, `.w=ColorMax` |

#### 核心算法逻辑

**阶段 1：4×4 邻域最近深度查找**（第 56-71 行）

用 4 个不重叠的 `textureGather` 覆盖 16 个 texel，提取内部 3×3 的 9 个深度值，计算最近深度 `topLeftMax9`。

```
Gather 中心 2×2 (maxC) + 4 个角块 → 9 个深度 → 取最近值
```

**阶段 2：4-Quad DepthClip**（第 73-96 行）

当前帧内的深度比较：中心深度 vs 4 个象限的最近深度。

自适应深度容限：

```
Ksep = 1.37e-5
Ksep_Kfov_diagonal = Ksep × Kfov × diagonal_length
Depthsep = Ksep_Kfov_diagonal × (1.0 - maxC)

Wdepth = Σ clamp(Depthsep / |maxC - quadMin|)    (4 个象限, 各权重 0.25)
depthclip = 1.0 - Wdepth × 0.25
```

- `depthclip ≈ 0`：无 disocclusion，历史可靠
- `depthclip ≈ 1`：disocclusion，压低历史

**阶段 3：运动向量推导**（第 98-119 行）

- 动态 MV（`EncodedVelocity.x > 0`）：直接解码（**无膨胀**——MV 不重定向到最近深度像素）
- 静态几何（相机运动）：用 `clipToPrevClip` 矩阵重投影，最近深度 `topLeftMax9` 作为 Z 坐标

```
解码公式: dv = ev × inv_div − 32767/65535 × inv_div, inv_div = 1/(0.499×0.5)
静态: ScreenPos = 2×UV−1, Position = (ScreenPos, topLeftMax9)
      PreClip = clipToPrevClip × Position, motion = ScreenPos − PreClip.xy/PreClip.w
```

**阶段 4：RGB → YCoCg + Tone-map**（第 122-133 行）

```
ColorMax = max(R, G, B) + preExposure       ← 保存! 供 Upscale 逐像素 HDR 还原
Colorrgb /= ColorMax                         ← HDR → [0,1] LDR

Y  = 0.25 × (R + 2G + B)
Co = 0.5R + 0.5 − 0.5B   (clamp [0,1])
Cg = Y + Co − R           (clamp [0,1])

量化: x11 = uint(Y×2047.5), y11 = uint(Co×2047.5), z10 = uint(Cg×1023.5)
打包: (x11 << 21) | (y11 << 10) | z10   → R32UI
```

**阶段 5：写回**

```
MotionDepthClipAlphaBuffer = (motion, depthclip, ColorMax)  ← .w 存 ColorMax
YCoCgColor = packed_uint32
```

---

### Pass 2: Upscale (`sgsr2_upscale.comp`)

#### 输入纹理

| 纹理名称 | 分辨率 | 格式 | 用途 |
|----------|--------|------|------|
| `PrevHistoryOutput` | Display | RGBA16F | 上一帧混合后的 YCoCg + Wfactor |
| `MotionDepthClipAlphaBuffer` | Render | RGBA16F | Convert pass 产出 |
| `YCoCgColor` | Render | R32UI | Convert pass 产出 |

#### 输出纹理

| 纹理名称 | 分辨率 | 格式 | 内容 |
|----------|--------|------|------|
| `SceneColorOutput` | Display | RGBA16F | 最终 HDR 颜色 |
| `HistoryOutput` | Display | RGBA16F | `.xyz=混合YCoCg`, `.w=Wfactor`（透传） |

#### 核心算法逻辑

**阶段 1：自适应 Lanczos 核参数**（第 57-108 行）

```
Biasmax_viewportXScale = min(displayW/renderW, 1.99)
scalefactor = min(20, (s_x × s_y)³) ≈ s⁶
Wfactor = clamp(|History.w|, 0, 1)    ← 2-Pass 中透传常量，不更新

kernelfactor = clamp(Wfactor + reset, 0, 1)
biasmax = viewportScale × (1 − kernelfactor)
biasmin = max(1.0, 0.3 + 0.3 × biasmax)
biasfactor = max(0.25 × depthfactor, kernelfactor)
kernelbias = mix(biasmax, biasmin, biasfactor) × 0.5

curvebias = mix(−2, −3, clamp(motion_px × 0.02, 0, 1))  ← 运动越大越负，方差盒越紧
```

**阶段 2：坐标映射 + 重投影 UV**（第 66-88 行）

```
Hruv = (threadID+0.5) / displaySize             ← 输出像素中心 UV
Jitteruv = Hruv + jitter/renderSize              ← 应用 Halton jitter
PrevUV = Hruv - 0.5 × Motion                     ← NDC→UV 重投影
InputPos = ivec2(Jitteruv × renderSize)          ← 对应低分纹理坐标
```

**阶段 3：YCoCg 采样 & 双分支策略**（第 122-151 行）

- **相机静止**（`sameCameraFrmNum ≠ 0`）：3×3 全网格 = 9 采样点（4 次 `textureGather` + 1 次 `texelFetch`）
- **相机运动**：5 点十字（中心 + 4 邻），4 个角丢弃

```
解码 YCoCg (DecodeColor):
Y  = x11 × 1/2047.5
Co = y11 × 4.7695e-7 − 0.5
Cg = z10 × 1/1023.5 − 0.5
```

**阶段 4：Lanczos 加权上采样 + 方差盒统计**（第 155-292 行）

对每个采样点：

```
baseoffset = 采样位置 − 输出亚像素位置
base = clamp(|baseoffset|² × kernelbias², 0, 1)
weight = FastLanczos(base) = (0.75(b−1) + (b−1)²)(b−1)²

// 上采样路径
Upsampledcw.xyz += color × weight,  .w += weight

// 方差盒路径
boxweight = exp(|baseoffset|² × curvebias)       ← 负 curvebias × 距离² = e^(−kd²)
rectboxcenter += color × boxweight               ← 一阶矩
rectboxvar    += color² × boxweight              ← 二阶矩
rectboxmin = min, rectboxmax = max               ← AABB 硬边界
```

**阶段 5：归一化**（第 295-307 行）

```
rectboxcenter = Σ(color×w) / Σ(w)               ← 指数加权均值
rectboxvar    = sqrt(Σ(color²×w)/Σ(w) − center²) ← 指数加权标准差
Upsampledcw.xyz = clamp(avg, AABB_min−0.05, AABB_max+0.05)
Upsampledcw.w  /= 3.0                            ← Lanczos 权重 → 时域信任尺度校准
```

**阶段 6：baseupdate — 历史累积权重**（第 307-317 行）

```
2-Pass: Wfactor 只透传 → 实际是初始化常量
baseupdate = (1−Wfactor) × (1−depthfactor)

// 两段运动压制
baseupdate = min(baseupdate, mix(原始值, weight×10, clamp(10×motion_px)))   ≥0.1px
baseupdate = min(baseupdate, mix(原始值, weight×1, clamp(0.05×motion_px)))  ≥20px
```

- `baseupdate` 大 → alpha 小 → 偏历史（静止区高质量抗锯齿）
- `baseupdate` 小 → alpha 大 → 偏当前帧（运动区快速响应）

**阶段 7：方差包围盒融合**（第 320-328 行）

```
boxscale = max(depthfactor, clamp(motion_px×0.05))
boxsize  = mix(scalefactor, 1.0, boxscale)     ← 稳定区 20σ, disocclusion/运动 1σ
最终盒 = intersect(AABB, 均值±boxsize×σ)        ← 取两个盒的交集（更紧者）
```

**阶段 8：历史裁剪 + 时域混合**（第 330-347 行）

```
startLerpValue = (|motion|>0) ? 0.0 : MinLerpContribution
lerpcontribution = (历史出盒) ? startLerpValue : 1.0

HistoryColor = mix(clamped, unclamped, lerpcontribution)
basealpha    = mix(min(basealpha,0.1), basealpha, lerpcontribution)
alpha        = Upsampledcw.w / (basealpha + Upsampledcw.w) + reset
final_ycocg  = mix(HistoryColor, Upsampledcw.xyz, alpha)
```

**阶段 9：YCoCg → RGB**（第 353-362 行）

```
R = Y − Cg + Co
G = Y + Cg
B = Y − Cg − Co
```

**阶段 10：逐像素 HDR 逆 Tone-map**（第 363-369 行）**← 2-Pass 独有**

```
compMax = max(R, G, B)
scale = preExposure / (1.009 − compMax)         ← 1.009 = 1 + 600/65504 (防除零)
if ColorMax > 4000: scale = ColorMax            ← 极亮像素回退原始 divisor
最终 HDR 输出: blended_RGB × scale
```

---

## 变体二：2-Pass Fragment Shader

**文件路径**：`glsl_2_pass_fs/`

### 管线概览

同为 2-Pass 架构，但用 Fragment Shader 替代 Compute Shader。顶点 shader (`sgsr2_vertex.vs`) 只做全屏三角形透视。

### Pass 1: Convert (`sgsr2_convert.fs`)

#### 输入纹理

| 纹理名称 | 分辨率 | 格式 | 用途 |
|----------|--------|------|------|
| `InputDepth` | Render | D24S8 | 当前帧深度 |
| `InputVelocity` | Render | RGBA | 编码的动态 MV |

**注意：Convert 不读 InputColor（与 2-Pass CS 不同）**

#### 输出

| 名称 | 格式 | 内容 |
|------|------|------|
| `MotionDepthClipAlphaBuffer` | RGBA16F | `.xy=motion`, `.z=depthclip`, `.w=0.0`（**无 ColorMax**） |

#### 与 2-Pass CS Convert 的核心差异

1. **无颜色处理**：不读 `InputColor`，不做 RGB→YCoCg，不量化。`.w` 硬编码为 0.0
2. **深度 Gather 布局翻转**：FS 坐标系 Y 轴朝上 vs CS 相反，命名不同但 4-quad 逻辑等价
3. **Fragment Shader 输入**：用 `texCoord`（光栅化插值），非 dispatch ID。`InputPos = texCoord × renderSize`
4. **DepthClip 公式、MV 推导**完全相同
5. **无 `YCoCgColor` 输出**：颜色上采样由 Upscale 直接从 `InputColor` 完成

---

### Pass 2: Upscale (`sgsr2_upscale.fs`)

#### 输入纹理

| 纹理名称 | 分辨率 | 格式 | 用途 |
|----------|--------|------|------|
| `PrevOutput` | Display | RGBA | 上一帧输出颜色 |
| `MotionDepthClipAlphaBuffer` | Render | RGBA16F | Convert pass 产出 |
| `InputColor` | Render | RGBA | 当前帧原始颜色（**直接读取**） |

#### 输出

| 名称 | 格式 | 内容 |
|------|------|------|
| `Output` | RGBA16F | 最终颜色（`.w = 0.0`） |

#### 与 2-Pass CS Upscale 的核心差异

| 差异 | 2-Pass CS | 2-Pass FS |
|------|-----------|-----------|
| 颜色来源 | YCoCg R32UI（解码）| **InputColor 直接 texelFetch** |
| 颜色空间 | YCoCg（内部处理）| **RGB（全程）** |
| 采样模式 | 5/9 tap 自适应 | **5-tap 十字固定** |
| 4 个角采样 | 相机静止时启用 | **`if(false)` 永远禁用** |
| Wfactor | 透传常量 | **无**（`.xyz` 只读，baseupdate = 1−depthclip） |
| HDR 还原 | 逐像素 ColorMax | **无**（LDR 输出） |
| 历史输出 | 独立 `HistoryOutput` + `SceneColorOutput` | **单 `Output`**（同纹 Ping-Pong） |
| clamp 容限 | ±0.05 | **±0.075** |
| scale 参数 | 实时计算 | **UBO 预计算 `scaleRatio`** |

---

## 变体三：3-Pass Compute Shader

**文件路径**：`glsl_3_pass_cs/`

### 管线概览

```
Pass 1: Convert (低分)  →  Pass 2: Activate (低分)  →  Pass 3: Upscale (高分)

InputOpaqueColor─┐         PrevLumaHistory (上帧)       MotionDepthClipAlphaBuffer ◀ Activate
InputColor  ─────┤         MotionDepthAlphaBuffer        YCoCgColor               ◀ Convert
InputDepth  ─────┤         YCoCgColor                    PrevHistoryOutput         ◀ 上帧
InputVelocity───┤                                                                    Upscale
                ├─ YCoCgColor                          ├─ MotionDepthClipAlphaBuffer
                └─ MotionDepthAlphaBuffer              ├─ LumaHistory
                                                       ── SceneColorOutput
                                                       ── HistoryOutput
```

每帧 3 次 dispatch：**Convert → Activate → Upscale**。

---

### Pass 1: Convert (`sgsr2_convert.comp`)

#### 输入纹理

| 纹理名称 | 分辨率 | 格式 | 用途 |
|----------|--------|------|------|
| `InputOpaqueColor` | Render | RGBA (HDR) | **渲染透明前的不透明背景色** |
| `InputColor` | Render | RGBA (HDR) | 透明后的最终颜色 |
| `InputDepth` | Render | D24S8 | 当前帧深度 |
| `InputVelocity` | Render | RGBA | 编码的动态 MV |

#### 输出纹理

| 纹理名称 | 分辨率 | 格式 | 编码 |
|----------|--------|------|------|
| `YCoCgColor` | Render | R32UI | `[Y:11\|Co:11\|Cg:10]` 打包 |
| `MotionDepthAlphaBuffer` | Render | RGBA16F | `.xy=motion`, `.z=NearestZ`, `.w=alpha_mask` |

#### 与 2-Pass CS Convert 的核心差异

1. **3×3（非 4×4）深度邻域**：1 texelFetch + 1 gather + 2 半 gather = 9 深度，取最近值 `NearestZ` → `.z`
2. **不在此阶段计算 depthclip**（推迟到 Activate pass 做双线性精确计算）
3. **Alpha Mask 检测**（第 126-139 行）：

```
Colorprergb = texelFetch(InputOpaqueColor, ...)     ← 透明前
Colorrgb    = texelFetch(InputColor, ...)            ← 透明后
delta       = |tone(Colorrgb) − tone(Colorprergb)|  ← 逐通道差
alpha_mask  = max(delta.r, delta.g, delta.b) × 350  ← 最大差异 × 放大
```

4. **不保存 ColorMax**：`.w = alpha_mask`，`.w = 0` 留给透明掩码。**无逐像素 HDR 还原**。

---

### Pass 2: Activate (`sgsr2_activate.comp`) ← 3-Pass 独有

#### 输入纹理

| 纹理名称 | 分辨率 | 格式 | 用途 |
|----------|--------|------|------|
| `PrevLumaHistory` | Render | R32UI | 上一帧的 [luma \| sign_coherent_diff] |
| `MotionDepthAlphaBuffer` | Render | RGBA16F | Convert 产出（.z=NearestZ, .w=alpha_mask） |
| `YCoCgColor` | Render | R32UI | Convert 产出（打包 YCoCg） |

#### 输出纹理

| 纹理名称 | 分辨率 | 格式 | 内容 |
|----------|--------|------|------|
| `MotionDepthClipAlphaBuffer` | Render | RGBA16F | `.xy=motion`, `.z=depthclip`, `.w=alpha+luma标志` |
| `LumaHistory` | Render | R32UI | `[当前luma \| sign_coherent_diff]` |

#### 核心算法逻辑

**阶段 1：读取当前帧亮度**

```
luma_reference = DecodeColorY(gather(YCoCgColor, gatherCoord).w)  ← 只解码 Y，丢弃 Co/Cg
```

**阶段 2：读取 Convert 中间结果**

```
motion = mda.xy,  depth = mda.z (NearestZ),  alphamask = mda.w (alpha_mask)
PrevUV = −0.5 × motion + ViewportUV                          ← MV 重投影
```

**阶段 3：双线性精确 DepthClip**（第 82-143 行）

**这是三种变体中精度最高的 depthclip 计算。**

- 在 PrevUV 处算亚像素相位
- 4 个双线性权重对应 4 个采样点
- 每个采样点用 `textureGatherOffset` 抓 2×2 窗口的最小深度，与当前深度比较

```
Prevf_pixel = PrevUV × renderSize − 0.5           ← 上一帧浮点坐标
Prevf_frac  = frac(Prevf_pixel)                   ← 亚像素相位

Bilinweights = [(1−fx)(1−fy), fx(1−fy), (1−fx)fy, fx·fy]   ← 四点权重

对每个采样点:
  gPrevDepth = textureGatherOffset(MotionDepthAlphaBuffer, PrevUV, offset, channel 2)
  fPrevDepth = min(.x, .y, .z, .w)               ← 该点 2×2 最浅深度
  Depthsep = Ksep × Kfov × diagonal × (1.0 − min(fPrevDepth, depth))
  Wdepth += clamp(Depthsep / |fPrevDepth − depth|, 0,1) × bilinear_weight

depthclip = 1.0 − Wdepth
```

比 2-Pass 的固定 4-quad 比较精确得多：双线性权重 + 亚像素相位 + 跨帧（MV 导向）位置比较。

**阶段 4：Luma Sign-Coherent 差异追踪**（第 146-183 行）

追踪多帧亮度变化的最小持续方向：

```
luma_diff = 当前亮度 − 上一帧历史亮度

如果历史不可靠 (depthclip+reset ≥ 0.1 或 PrevUV 出屏):
    current_luma_diff = (0, 0)                    ← 放弃追踪
否则:
    current_luma_diff.x = luma_reference          ← 存当前亮度
    current_luma_diff.y =
        首帧?     → luma_diff                    初始化
        同向?     → sign × min(|prev|, |diff|)  收缩到更小值
        异向?     → prev                          冻结（噪声过滤）
```

- **同向持续收缩**：亮度连续同向变化 → 追踪值降到最小 → 真场景特征
- **异向冻结**：方向翻转 → 保留上次 → 视为噪声

**阶段 5：编码 Luma Highlight 位 + 写回**

```
luma_highlight = (追踪成功) && (|追踪值| ≠ |真实差|)   ← 有不规律额度外亮度跳变
alphamask_encoded = floor(alpha_mask) + 0.5 × luma_highlight   ← 整数+小数共存

LumaHistory = [packHalf2x16(luma) << 16] | packHalf2x16(sign_diff)
MotionDepthClipAlphaBuffer = (motion 透传, 双线性精确 depthclip, 编码后的 alphamask)
```

---

### Pass 3: Upscale (`sgsr2_upscale.comp`)

#### 输入 / 输出

与 2-Pass CS Upscale 基本相同的纹理布局，但：

- `MotionDepthClipAlphaBuffer.w` 的含义不同（3-Pass 存 alpha_mask + luma 标志，2-Pass 存 ColorMax）
- 多了一个回路的 luma/alpha 数据解码

#### 与 2-Pass CS Upscale 的核心差异

**1. Wfactor 闭环更新**（第 113 行）

```
Wfactor = max(|History.w|, alphamask)               ← 单调不降!
```

2-Pass 中 Wfactor 是透传常量；3-Pass 每帧取 max(上一帧置信, 当前帧透明掩码)。一旦半透明触发，Wfactor 永久推高——防止不稳定像素积累过多历史。

**2. 动态 lerp 宽容度（tcontribute）**（第 305-334 行）

```
history_value = fract(alphab) × 2.0                 ← 提取 Luma Highlight (0 or 1)
alphamask     = trunc(alphab) × 0.001               ← 提取透明掩码 [0, 0.35]

tcontribute = history_value × clamp(std×10, 0, 1) × (1−Wfactor)
tcontribute 需额外扣减 sqrt(alphamask)               ← 透明度收紧
```

- `history_value = 0`（无特征）：tcontribute = 0 → 历史出盒硬裁
- `history_value = 1`（有特征、高方差）：tcontribute 接近 1 → 宽让
- 半透明区域：tcontribute 进一步降低

**3. 固定 9-tap Lanczos**（第 139-293 行）

无 `sameCameraFrmNum` 分支，9 个采样点始终计算。

**4. 无 ColorMax 逐像素 HDR 还原**（第 356-361 行）

```
scale = preExposure / (1.000015 − compMax)          ← 紧缩 eps (1+1/65504)
output = blended_ycocg_rgb × scale
```

无 `ColorMax > 4000` 回退路径——极亮像素比 2-Pass 略暗。

---

## 三变体核心差异对比

| 特性 | 2-Pass CS | 2-Pass FS | 3-Pass CS |
|------|-----------|-----------|-----------|
| **Pass 数** | 2 | 2 | 3 |
| **Shader 类型** | Compute | Fragment | Compute |
| **DepthClip 方法** | 4-quad 帧内比较 | 4-quad 帧内比较 | **双线性跨帧重投影** |
| **DepthClip 位置** | Convert | Convert | **Activate（推迟）** |
| **颜色工作空间** | YCoCg（量化） | RGB（原始） | YCoCg（量化） |
| **上采样 Tap 数** | 5 或 9（自适应） | **5（永远）** | **9（永远）** |
| **角采样** | 相机静止时启用 | **`if(false)` 永禁** | 始终启用 |
| **Wfactor** | 透传常量 | 无（baseupdate=1−depthclip） | **闭环更新（max 单调）** |
| **透明处理** | 无 | 无 | **Alpha_mask + Wfactor 持久** |
| **Luma 追踪** | 无 | 无 | **Sign-coherent 跨帧追踪** |
| **逐像素 HDR 还原** | **是（ColorMax）** | 无 | 无（全局 preExposure） |
| **额外输入** | — | — | InputOpaqueColor |
| **历史缓冲** | RGBA16F(.w=Wfactor) | RGBA16F(.w=0) | RGBA16F(.w=Wfactor) |
| **clamp 容限** | ±0.05 | ±0.075 | ±0.05 |

### 画质影响对比

| 效果 | 2-Pass CS | 2-Pass FS | 3-Pass CS |
|------|-----------|-----------|-----------|
| 颜色精度 | YCoCg 去相关 | RGB 可能跨通道误 clamp | YCoCg 去相关 |
| 深度精度 | 4-quad 近似 | 4-quad 近似 | 双线性精确 |
| 运动画质 | 相机运动时退 5-tap | 永远 5-tap | 全 9-tap + Luma 追踪 |
| 半透明 | 易重影 | 易重影 | 检测+持久压制 |
| HDR 高亮 | 逐像素精确 | 不支持 | 全局近似 |
| Disocclusion | 帧内邻域推测 | 帧内邻域推测 | 跨帧精确检测 |

---

## 性能数据

Snapdragon 8 Gen 3 最大 GPU 频率（来自 README）：

| 变体 | 1.5× upscale | 1.7× upscale | 2.0× upscale |
|------|:---:|:---:|:---:|
| **2-pass FS** | 1.107 ms | 1.024 ms | **0.905 ms** |
| **2-pass CS** | 1.998 ms | 1.910 ms | 1.801 ms |
| **3-pass CS** | 2.397 ms | 2.199 ms | 2.015 ms |

### 分析

- **2-pass FS 比 2-pass CS 快 ~2×**：得益 Adreno tile-based 架构的片上 GMEM，FS 避免 compute→L2→system 带宽开销。5-tap 固定 + 无 YCoCg 打包/解包 + 无独立 HistoryOutput
- **2-pass CS 比 3-pass CS 快 ~10–17%**：缺 Activate pass。高倍率时 Upscale（高线程数）主导，差距缩小
- **倍率越高耗时越长**：Upscale pass 在 display 分辨率执行
- **2-pass FS 适合 VR/移动端**：2× 超分只需 <1ms → >1000 FPS 理论吞吐

---

## 设计原理

### 为什么三种变体

对应三个使用场景的性能-画质权衡：

- **2-pass FS**：VR / 移动低延迟场景。砍 YCoCg、砍 HDR、砍角点、砍 Wfactor → 换取 <1ms
- **2-pass CS**：均衡场景。保留 YCoCg + HDR + 自适应 tap → 中间性价比
- **3-pass CS**：主机 / 高端移动场景。追加 Activate → 双线性 depthclip + Luma 追踪 + 闭环 Ffactor

### 为什么 YCoCg

1. **通道去相关**：RGB 高度相关，独立 clamp 产生色偏；YCoCg 亮/色分离，clamp 在独立分量
2. **打包效率**：11+11+10=32 位一个 R32UI，不用三个独立通道
3. **int↔float 转换廉价**：移位 + 缩放，Adreno ALU 成本低

### 为什么 Adreno Tile-Based 架构下 FS 能更快

Fragment Shader 在 render pass 内执行，中间缓冲可留在片上 GMEM，省去 compute→system memory 的带宽。2-pass FS 正是利用这点：Convert pass 在深度/速度已经驻留在 GMEM 的 G-buffer pass 之后接续执行。

### 为什么 3-Pass 的 depthclip 精度更高

Activate 以双线性亚像素采样代替 2-Pass 的固定 4-quad block 比较——重投影位置由 MV 精确引导，每像素带独立 sub-pixel 权重。2-Pass 在当前帧固定网格盲查，3-Pass 跨帧精确指向。

### 为什么 Luma Sign-Coherent 追踪

简单阈值容易振荡：一个特征交替换亮变暗，每帧 flip。Sign-coherent 方法：
- **同向收缩**：持续同向 → 追踪值稳定到最小 → 可信
- **异向冻结**：方向翻转 → 保留上次 → 过滤噪声 → 可控

产生的 Luma Highlight 信号稳定控制历史裁剪的宽容度。

---

*文档基于 Snapdragon GSR v2 源代码分析 (Copyright 2024 Qualcomm Innovation Center, Inc.)*
