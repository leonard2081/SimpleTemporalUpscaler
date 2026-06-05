# FSR4 插帧（Frame Interpolation）逻辑分析

## 一、代码文件索引

### 1.1 UE Plugin 层（Unreal Engine 5 集成）

| 文件 | 路径 | 职责 |
|------|------|------|
| `FFXFrameInterpolation.h/.cpp` | `Source/FFXFrameInterpolation/Private/` | 插帧主控逻辑，入口与调度 |
| `FFXFrameInterpolationViewExtension.h/.cpp` | `Source/FFXFrameInterpolation/Private/` | 场景 View 扩展，PrePostProcess 钩子 |
| `FFXFrameInterpolationCustomPresent.h/.cpp` | `Source/FFXFrameInterpolation/Private/` | 自定义 Present，RHI 模式 SwapChain 接管 |
| `FFXFrameInterpolationSlate.h/.cpp` | `Source/FFXFrameInterpolation/Private/` | Slate 渲染器代理，多 DrawBuffer 环形缓冲 |
| `FFXFrameInterpolationModule.cpp` | `Source/FFXFrameInterpolation/Private/` | 模块加载/卸载 |
| `IFFXFrameInterpolation.h` | `Source/FFXFrameInterpolation/Public/` | 对外接口定义 |

### 1.2 FidelityFX SDK 层（Native 后端）

| 文件 | 路径 | 职责 |
|------|------|------|
| `ffx_frameinterpolation.h` | `sdk/include/FidelityFX/host/` | Host API 定义（枚举、结构体、函数声明） |
| `ffx_frameinterpolation.cpp` | `sdk/src/components/frameinterpolation/` | 插帧核心实现，GPU 资源管理与 Dispatch |
| `FrameInterpolationSwapchainDX12.h/.cpp` | `sdk/src/backends/dx12/FrameInterpolationSwapchain/` | DX12 SwapChain 代理，Present 拦截，线程 Pacing |
| `FrameInterpolationSwapchainDX12_UiComposition.h/.cpp` | `sdk/src/backends/dx12/FrameInterpolationSwapchain/` | UI 合成 Shader (VS/PS 全屏三角形) |
| `FrameInterpolationSwapchainDX12_DebugPacing.h/.cpp` | `sdk/src/backends/dx12/FrameInterpolationSwapchain/` | 调试 Pacing 线可视化 |

### 1.3 Shader 文件

| 目录 | 说明 |
|------|------|
| `Shaders/Private/ffx_frameinterpolation_*.usf` | UE .usf 封装 Shader（11 个 Pass） |
| `Source/fidelityfx-hlsl/ffx_frameinterpolation_*.hlsl` | FidelityFX HLSL 原始 Shader |
| `Source/fidelityfx-hlsl/frameinterpolation/ffx_frameinterpolation_*.h` | HLSL 公用头文件与绑定声明 |

---

## 二、插帧触发时机

### 2.1 两种模式概览

| 特性 | RHI 模式（默认） | SwapChain/Native 模式 |
|------|-----------------|---------------------|
| 启用方式 | 默认值 | `-fsr4swapchain` 启动参数 |
| Present 接管 | 否，UE 引擎正常 Present | 是，`FrameInterpolationSwapChainDX12` 代理 |
| 独立 Pacing 线程 | 无 | 有（两个线程） |
| GPU 指令录制时机 | PostRenderDelegate 回调 | PostRenderDelegate 回调 |
| GPU 指令执行时机 | PostRenderDelegate 回调内 Flush | `Present()` 调用内部 |

### 2.2 RHI 模式完整时序

```
帧 N 渲染:
  ├─ 3D场景渲染 + 后处理
  │   backbuffer = 游戏画面 (无 UI)
  │
  ├─ PrePostProcessPass_RenderThread()    ← ViewExtension 钩子
  │   └─ SetupView() 收集 View/SceenDepth/Velocity/Rect 等元数据
  │
  ├─ PostRenderDelegate 回调
  │   ├─ AddCopyTexturePass(BackBufferRDG → BackBufferRT)  ← 截取纯游戏画面
  │   ├─ SetPreUITextures(BackBufferRT, InterpolatedRT)     ← 标记 "Pre-UI" 纹理
  │   ├─ InterpolateView()           ← 消费 BackBufferRT（无UI），生成插帧到 InterpolatedRT
  │   │   ├─ ConvertVelocity()       ← UE Velocity → FFX MotionVectors
  │   │   ├─ ConvertDistortion()     ← UE Distortion → FFX Distortion (可选)
  │   │   ├─ ffxDispatch(PREPARE)    ← 光流、运动矢量场、遮蔽遮罩
  │   │   └─ ffxDispatch(FRAMEGENERATION)  ← 插帧合成
  │   └─ Flush()                     ← 等待 GPU 完成插帧
  │
  ├─ Slate UI 渲染
  │   backbuffer = 游戏画面 + UI      ← UE 原生行为，UI 画在 backbuffer 上
  │
  ├─ OnSlateWindowRendered()
  │   ├─ TransitionAndCopyTexture(InterpolatedFrame, BackBuffer)
  │   │                               ← 插帧结果覆盖 backbuffer（不含 UI）
  │   ├─ ForceRedrawWindow()          ← 强制 Slate 完整重绘一次
  │   │   backbuffer = 插帧画面 + 新UI  ← 这就是插帧帧的最终画面
  │   └─ SetCustomPresentStatus(PresentRT)
  │
  ├─ [引擎自动 Present 插帧帧]
  │
  ├─ OnBackBufferReadyToPresentCallback()
  │   ├─ CopyBackBufferRT(PresentRT)  ← 此时 backbuffer 含真实帧游戏+UI
  │   │   └─ 拷贝到 RealFrame 纹理备用
  │   └─ 将 RealFrame 拷回 backbuffer
  │
  └─ [引擎自动 Present 真实帧]
```

### 2.3 SwapChain/Native 模式完整时序

```
帧 N 渲染:
  ├─ 3D场景渲染 + 后处理
  │   绘制到 ReplacementBuffers (不是真正的 backbuffer)
  │
  ├─ PostRenderDelegate 回调
  │   ├─ InterpolateView()
  │   │   └─ getInterpolationCommandList() ← 获取 CommandList，录制 GPU 插帧指令
  │   │       → 指令暂存在 registeredInterpolationCommandLists 中
  │   └─ [GPU 指令未执行，只是录制]
  │
  ├─ Slate UI 渲染 → 绘制到独立 UI 纹理
  │   └─ registerUiResource(uiResource) ← 将 UI 纹理注册给 SwapChain
  │
  └─ Present() 被调用 ───────────────────── 进入 FrameInterpolationSwapChainDX12
      ├─ verifyUiDuplicateResource() / copyUiResource()  ← UI 双缓冲拷贝
      ├─ WaitForSingleObject(interpolationEvent)          ← 等上次 pacing 完成
      ├─ presentInterpolated()
      │   ├─ Signal(gameFence) / Wait(gameFence)         ← 游戏→插值队列同步
      │   ├─ dispatchInterpolationCommands()              ← 执行 GPU 插帧
      │   │   ├─ pRegisteredCommandList->execute(true)    ← 执行录制的指令
      │   │   └─ 输出到 interpolationOutputs
      │   ├─ 构建 PacingData(interpolated + real, 各一帧)
      │   ├─ LeaveCriticalSection(scheduledInterpolations)
      │   └─ SetEvent(presentEvent)                       ← 唤醒插值线程
      │
      ├─ [interpolationThread]
      │   ├─ WaitForFenceValue(interpolationFence)        ← 等 GPU 插帧完成
      │   ├─ 计算帧时间滑动平均 → 算出 presentQpcDelta
      │   ├─ 将 PacingData 写入 scheduledPresents
      │   └─ SetEvent(pacerEvent)
      │
      └─ [presenterThread]
          ├─ 先 Present Interpolated_1:
          │   ├─ compositeSwapChainFrame() ← UI 合成 (插帧画面 + UI → swapchain)
          │   ├─ waitForPerformanceCount(delta)  ← CPU 端精确定时
          │   └─ SwapChain->Present()
          │
          └─ 后 Present Real:
              ├─ compositeSwapChainFrame() ← UI 合成 (真实帧画面 + UI → swapchain)
              ├─ waitForPerformanceCount(delta)  ← CPU 端精确定时
              └─ SwapChain->Present()
```

---

## 三、UI 在插帧时的处理

### 3.1 RHI 模式 — 重新绘制 UI

**核心原则**：利用 UE 渲染时间线，在 UI 绘制前获取干净的游戏画面；插帧后让 Slate 再绘制一次。

```
关键代码位置:
  FFXFrameInterpolation.cpp:1065  OnSlateWindowRendered()
  FFXFrameInterpolation.cpp:1118  TransitionAndCopyTexture(InterpolatedFrame, BackBuffer)
  FFXFrameInterpolation.cpp:1146  App.ForceRedrawWindow()
```

**UI 处理特点**：
- 插帧输入的 backbuffer **不含 UI**（在 PostRenderDelegate 时截取，Slate 还没画）
- 插帧结果覆盖 backbuffer 后，`ForceRedrawWindow()` 触发完整 Slate 重绘
- **插帧帧的 UI 是实时生成的**：光标位置、文字、动画状态等都是当前时刻的
- Slate 需要通过 `FFXFrameInterpolationSlateRenderer`（环形 DrawBuffer 缓冲）支持额外的绘制帧
- 可选地通过 `CVarFFXFICaptureDebugUI` 启用的额外 UI 合成（Compute Shader `FFXFIAdditionalUICS`）

**优缺点**：
- 优点：UI 状态实时，不受插帧延迟影响
- 缺点：额外一次 Slate 绘制开销；两个 Present "挤在一起" 没有精确 pacing

### 3.2 SwapChain/Native 模式 — 复制真实帧 UI

**核心原则**：UI 和游戏画面完全分离，在 Present 时合成。

```
关键代码位置:
  DX12.cpp:1211  registerUiResource()             ← 注册 UI 纹理
  DX12.cpp:1555  copyUiResource()                 ← UI 资源双缓冲拷贝
  DX12.cpp:311   compositeSwapChainFrame()        ← Present 时合成，调 presentCallback
  UiComposition.cpp:250   ffxFrameInterpolationUiComposition()  ← 默认合成函数
```

**UI 合成 Pipeline**（`ffxFrameInterpolationUiComposition`）：
- 使用 GPU VS/PS 全屏三角形 Pass
- `t0` SRV → 游戏画面纹理（插帧结果或真实帧）
- `t1` SRV → UI 纹理
- 通过 Alpha 混合将 UI 叠加到游戏画面
- 支持预乘 Alpha (`FFX_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA`)
- 支持内部分配 UI 双缓冲 (`FFX_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING`)

**UI 处理特点**：
- **插帧帧和真实帧共享同一个 UI 纹理**（游戏帧渲染时产生的 UI）
- 插帧帧显示的是上一帧时刻的 UI 状态
- 插帧帧上鼠标光标的瞬移、按钮状态变化等与真实帧一致
- UI 纹理通过 `copyUiResource()` 拷贝到内部缓冲，避免与下一帧 UI 绘制竞争

**优缺点**：
- 优点：无额外 CPU 绘制开销；精确 GPU 端合成
- 缺点：插帧帧的 UI 状态相对滞后（与真实帧画面一致）

---

## 四、插帧与真实帧的显示节奏（Pacing）

### 4.1 RHI 模式 — 无独立 Pacing

RHI 模式没有独立的线程控制显示间隔，完全依赖 UE 引擎的帧循环节奏：

```
同一个帧周期内:
  [引擎 Present 插帧帧]  →  [引擎 Present 真实帧]
  
  两次 Present 间隔 ≈ Slate 重绘时间 (< 1ms)
```

**实际行为**：
- 两个帧几乎同时推入显示队列
- 无 V-Sync 时：由驱动/显示器决定顺序，可能出现撕裂
- 有 V-Sync 时（高刷新率显示器）：如果刷新率 > 帧率 × 2，两个帧各占一拍；如果刷新率 ≈ 帧率 × 2，较为理想
- **根本局限**：没有精确的 CPU 端定时，两个帧是被"挤"进去的

### 4.2 SwapChain/Native 模式 — 双线程精确 Pacing

#### 4.2.1 PacingData 结构 (`DX12.h:41`)

```cpp
typedef struct PacingData {
    FfxPresentCallbackFunc presentCallback;
    void*                  presentCallbackContext;
    FfxResource            uiSurface;
    bool                   vsync;
    bool                   tearingSupported;
    bool                   usePremulAlphaComposite;
    UINT64                 interpolationCompletedFenceValue;
    UINT64                 replacementBufferFenceSignal;
    UINT64                 numFramesSentForPresentationBase;
    UINT32                 numFramesToPresent;       // 本次要 Present 几个帧 (1 或 2)
    UINT64                 currentFrameID;
    
    struct FrameInfo {
        bool       doPresent;                        // 是否 Present 此帧
        FfxResource resource;                         // 纹理引用
        UINT64     interpolationCompletedFenceValue;  // GPU 侧完成标记
        UINT64     presentIndex;                      // 全局 Present 序号
        UINT64     presentQpcDelta;                   // CPU 定时间隔 (QPC 单位)
    };
    FrameInfo frames[2];  // [0]=Interpolated_1, [1]=Real
} PacingData;
```

#### 4.2.2 双帧调度逻辑

每次 `Present()` 时，`presentInterpolated()` 注册两个帧：

```cpp
// DX12.cpp:1442-1461
// 插帧（先显示）
fiInterpolated.doPresent = true;
fiInterpolated.presentIndex = ++framesSentForPresentation;  // e.g., 序号 N

// 真实帧（后显示）
fiReal.doPresent = true;
fiReal.presentIndex = ++framesSentForPresentation;          // e.g., 序号 N+1

entry.numFramesToPresent = 2;  // 两个帧都要 Present
```

显示顺序始终是：**先插帧 → 后真实帧**

#### 4.2.3 时间间隔计算（`interpolationThread`, DX12.cpp:498）

```
1. 获得当前帧时间 deltaQpc
2. frameTime.update(deltaQpc)              ← SimpleMovingAverage<10, double>
3. conservativeAvg = frameTimeAvg * 0.5  -  frameTimeVariance * varianceFactor
   // 激进预估：取一半平均值，扣除方差的方差系数倍
4. deltaToUse = conservativeAvg - qpcSafetyMargin
   // safetyMargin 默认 0.1ms，防止 overshoot
5. 插帧和真实帧使用相同的 presentQpcDelta
```

#### 4.2.4 Presenter 线程执行循环（`presenterThread`, DX12.cpp:411）

```
对每一个帧 (先 Interpolated_1，后 Real):
  1. compositeSwapChainFrame()      ← UI + 游戏画面 → SwapChain Buffer
  2. waitForFenceValue(compositionFenceGPU)
  3. waitForPerformanceCount(targetQpc)
     ├─ 间隔 > 2ms → WaitForSingleObject (节能)
     └─ 间隔 ≤ 2ms → 自旋等待 (低延迟 Hybrid Spin)
  4. presentToSwapChain()
     └─ SwapChain->Present(syncInterval, flags)  ← 实际 DXGI Present
```

#### 4.2.5 可调参数

```cpp
// DX12.h:115-119
safetyMarginInSec = 0.0001;   // 0.1ms，CPU 定时安全余量
varianceFactor    = 0.1;      // 方差系数
allowHybridSpin   = false;    // 是否允许混合自旋
hybridSpinTime    = 2;        // 系统定时器分辨率单位数
allowWaitForSingleObjectOnFence = false;
```

通过 `ffxConfigureFrameInterpolationSwapchainDX12()` + `FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING` 调整。

---

## 五、RHI 模式 vs SwapChain 模式对比总结

| 维度 | RHI 模式 | SwapChain 模式 |
|------|----------|---------------|
| **触发时机** | PostRenderDelegate 回调内录制+执行 | PostRenderDelegate 录制，Present 时执行 |
| **插帧输入** | 纯游戏画面（在 UI 绘制前截取） | 纯游戏画面（ReplacementBuffers，天然不含 UI） |
| **UI 处理** | 插帧后 `ForceRedrawWindow()` 重绘 UI | UI 纹理独立注册，Present 时 GPU 合成 |
| **插帧帧 UI 状态** | 实时生成（光标位置等是当前时刻的） | 拷贝真实帧时刻的 UI（保持一致性） |
| **显示节奏** | 无独立线程，挤在同一个引擎帧循环内 | 两个独立线程精确 CPU 端定时 |
| **帧间隔控制** | 无（依赖引擎帧率） | 基于滑动平均 + 方差的 μs 级精确控制 |
| **GPU 同步** | `Flush()` 同步等待 | Fence 信号 + Event 异步通知 |
| **适用场景** | 开发期参考，调试方便 | 正式运行推荐，质量更高 |
| **启用方式** | 默认 | `-fsr4swapchain` 命令行参数 |

### 5.1 RHI 模式关键局限

RHI 模式的两个帧共享同一个 backbuffer 作为输出目标，时间上是紧耦合的：

```
真实游戏帧 |--> 插帧生成 --> [Slate重绘] --> Present 插帧 --> [Swap Backbuffer] --> Present 真实帧
           |<---------------- 同一个帧周期 ----------------->|
```

在高刷新率 + 低帧率场景下可行（如 60fps 游戏 + 120Hz 显示器），但帧率接近刷新率时会出现排队和顺序问题。

### 5.2 SwapChain 模式设计亮点

- **完全解耦的三条时间线**：游戏渲染、插帧计算、屏幕输出各自独立
- **异步 GPU 执行**：插帧在 `interpolationQueue`（可配置为 async compute queue）上跑，不阻塞游戏队列
- **CPU 端精确 Pacing**：`waitForPerformanceCount()` 可以实现亚毫秒级的帧间隔控制
- **生产级 UI 处理**：UI 双缓冲、预乘 Alpha、GPU 端零拷贝合成
