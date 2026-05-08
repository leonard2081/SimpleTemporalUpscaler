# SimpleTemporalUpscaler

一个基于虚幻引擎 `ITemporalUpscaler` 接口的自定义时域上采样插件，实现了完整的时域超分辨率（Temporal Super Resolution）渲染管线。

## 功能特性

- **时域上采样**：支持 0.5x ~ 1.0x 分辨率分数（2x 到 1x 缩放）
- **运动矢量重投影**：利用场景速度（Scene Velocity）将历史帧重投影到当前帧
- **速度膨胀（Velocity Dilate）**：对缺失速度的像素，从 3x3 邻域选取有效速度填充
- **相机运动回退**：对静态物体（无速度），通过 ClipToPrevClip 矩阵计算纯相机重投影
- **运动自适应历史权重**：根据像素速度动态调整时域混合权重，减少运动拖影
- **邻域颜色 Clamp**：对当前帧 3x3 邻域计算颜色范围，Clamp 历史帧颜色以抑制鬼影
- **YCoCg 颜色空间 Clamp**：在 YCoCg 空间中进行分通道 Clamp，质量更高
- **深度拒绝（Depth Rejection）**：对比当前帧与历史帧深度差异，差异大时降低混合权重
- **锁定机制（Lock）**：检测高频边界特征并放宽 Clamp 范围，保护细节
- **去抖动（Dejitter）**：补偿 TemporalAA 抖动偏移，正确采样当前帧颜色
- **调试可视化**：9 种 Debug 模式，支持查看中间过程数据

## 控制台变量

| CVar | 默认值 | 说明 |
|------|--------|------|
| `r.SimpleTemporalUpscaler.Enable` | 0 | 启用/禁用插件 |
| `r.SimpleTemporalUpscaler.HistoryWeight` | 0.85 | 基础时域累积权重 |
| `r.SimpleTemporalUpscaler.UseVelocity` | 1 | 启用场景速度进行重投影 |
| `r.SimpleTemporalUpscaler.CurrentFrameDejitter` | 1 | 当前帧采样时补偿抖动 |
| `r.SimpleTemporalUpscaler.DilateVelocity` | 1 | 启用速度膨胀 |
| `r.SimpleTemporalUpscaler.MotionAdaptiveHistory` | 1 | 根据速度动态调整历史权重 |
| `r.SimpleTemporalUpscaler.MotionHistoryMinSpeed` | 0.5 | 运动自适应速度下限 |
| `r.SimpleTemporalUpscaler.MotionHistoryMaxSpeed` | 8.0 | 运动自适应速度上限（超过则权重归零） |
| `r.SimpleTemporalUpscaler.DepthThreshold` | 100.0 | 深度拒绝阈值 |
| `r.SimpleTemporalUpscaler.Lock` | 1 | 启用锁定机制保护高频细节 |
| `r.SimpleTemporalUpscaler.YCoCgClamp` | 1 | 使用 YCoCg 颜色空间做 Clamp |
| `r.SimpleTemporalUpscaler.DebugMode` | 0 | 可视化调试模式（0-7） |
| `r.SimpleTemporalUpscaler.LogStats` | 0 | 打印输入/输出分辨率统计 |

## DebugMode 可视化

| 值 | 显示内容 |
|----|----------|
| 0 | 正常输出 |
| 1 | 有效历史权重（灰度） |
| 2 | 深度置信度（灰度） |
| 3 | 运动置信度（灰度） |
| 4 | 历史重投影路径分类（颜色编码） |
| 5 | 速度膨胀掩码 |
| 6 | 像素速度大小（归一化） |
| 7 | 锁定贡献（灰度） |

## 依赖

- **公共模块**：Core、Engine、Renderer、RenderCore、RHI
- **私有模块**：Projects

## 兼容性

- 虚幻引擎 5.5
- 插件类型：Runtime
- 加载阶段：PostConfigInit

## 在 UE 源码中集成

### 1. 放置插件

将 `SimpleTemporalUpscaler` 目录复制到 UE 源码的 `Engine/Plugins/Runtime/` 目录下：

```
Engine/Plugins/Runtime/SimpleTemporalUpscaler/
├── SimpleTemporalUpscaler.uplugin
├── Shaders/
├── Source/
└── Binaries/
```

### 2. 重新生成工程文件

在 UE 源码根目录运行：

```
GenerateProjectFiles.bat
```

### 3. 编译插件

方式一：编译整个引擎（包含插件）

```
UnrealBuildTool Development Win64 -Target=UnrealEditor
```

方式二：仅编译插件（需已编译过引擎）

```
UnrealBuildTool Development Win64 -Target=SimpleTemporalUpscaler
```

编译后在 `Engine/Binaries/Win64/` 下生成 DLL。

### 4. 启用插件

启动编辑器后：

1. 菜单 **Edit → Plugins**
2. 搜索 `SimpleTemporalUpscaler`
3. 勾选启用

或在 `.uproject` 中添加插件依赖：

```json
"Plugins": [
    {
        "Name": "SimpleTemporalUpscaler",
        "Enabled": true
    }
]
```

### 5. 运行时启用

插件加载后，通过控制台命令启用：

```
r.SimpleTemporalUpscaler.Enable 1
```

该插件通过 `FSceneViewExtension` 的 `BeginRenderViewFamily` Hook 自动注入上采样器，无需额外蓝图或 C++ 修改。

## 开发历史

| 版本 | 日期 | 说明 |
|------|------|------|
| V0.1 | 2026-04-28 | 极简超分框架，简单坐标采样+加权混合，无 MV |
| V0.2 | 2026-04-28 | 增加 MV 重投影 |
| V0.3 | 2026-04-28 | 增加邻域颜色 Clamp，抑制鬼影 |
| V0.4 | 2026-04-29 | 增加历史深度保存和深度拒绝 |
| V0.5 | 2026-04-29 | 增加相机 MV 计算、静态物体重投影、Dejitter |
| V0.6 | 2026-04-29 | 增加运动自适应 HistoryWeight |
| V0.65 | 2026-04-29 | 优化静态物体的动态权重计算 |
| V0.7 | 2026-04-29 | 增加 MV 膨胀 |
| V0.8 | 2026-04-30 | 过程数据可视化调试 |
| V0.85 | 2026-04-30 | 增加 MV 大小可视化 DebugMode |
| V0.88 | 2026-04-30 | 增加深度校验阈值，避免静态物体边缘伪影 |
| V0.9 | 2026-05-07 | 使用 YCoCg 做颜色 Clamp |
| V0.10 | 2026-05-07 | 增加 Lock 机制保护高频边界 |

## 许可证

本项目仅供学习和参考。
