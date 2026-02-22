# 光线追踪 Demo 实施计划（Qt6 + OpenGL 4.3 Compute，Windows/MSVC）

## 摘要
- 目标：在 `d:\Program\trace` 构建一个可交互、可累积降噪的 Cornell Box 实时路径追踪 Demo，核心渲染由 Compute Shader 实现。
- 已锁定关键决策：
  - `Alpha` 策略：**两档保留**（动态清空 + 静态低 alpha 累积）
  - 三球初始定位：**玻璃变体**（同介质模型，参数区分外观）
  - CMake 路径策略：**缓存变量 + 默认值**
- 当前仓库现状（已探查）：`CMakeLists.txt` 为空，`TASKS.md` 已建立，构建缓存显示生成器为 `Visual Studio 17 2022 x64`。

## 技能使用说明
- 未使用 `skill-creator` / `skill-installer`，因为当前任务是项目实现规划，不属于技能创建或安装场景。

## 约束与默认配置
- 平台：Windows，MSVC 2022 x64。
- 依赖路径默认值：
  - `RT_QT6_ROOT = D:/Qt/6.9.3/msvc2022_64`
  - `RT_GLM_ROOT = D:/glm`
- API/库：OpenGL 4.3+、Qt6（Core/Gui/Widgets/OpenGL/OpenGLWidgets）、GLM。
- 固定渲染分辨率：`1280x720`；工作组：`16x16`。

## 目录与模块设计（实施后应形成）
- `CMakeLists.txt`
- `src/main.cpp`
- `src/app/MainWindow.{h,cpp}`（整体 UI 布局、参数面板、信息区）
- `src/render/RenderWidget.{h,cpp}`（QOpenGLWidget 生命周期、调度）
- `src/render/Renderer.{h,cpp}`（OpenGL 资源、SSBO、纹理、Timer Query）
- `src/scene/SceneData.{h,cpp}`（Cornell 三角形、球体、光源、材质、相机参数）
- `src/scene/BvhBuilder.{h,cpp}`（CPU BVH 构建与延迟重建）
- `src/render/glsl/raytrace.comp.glsl`
- `src/render/glsl/fullscreen.vert.glsl`、`src/render/glsl/fullscreen.frag.glsl`
- `README.md`

## 关键接口 / 类型（公开契约）
- CPU/GPU 对齐结构（`std430`）：
  - `GpuTriangle`, `GpuBvhNode`, `GpuSphere`, `GpuMaterial`, `GpuLight`, `GpuCamera`, `GpuRenderParams`
- Renderer 关键方法：
  - `initialize()`, `resize(int w,int h)`, `renderFrame()`
  - `setScene(const SceneData&)`, `setRenderParams(const RenderParams&)`
  - `markSceneDirty(SceneDirtyFlags flags)`（区分仅重置 accum vs 触发 BVH 延迟重建）
- UI -> 渲染桥接：
  - 参数变更信号统一进入 `applyAndInvalidate(...)`
  - `DirtyFlags`：`CameraChanged`, `MaterialChanged`, `LightChanged`, `SphereTransformChanged`

## 渲染与数据流（决策完成版）
1. UI 改参数 -> 标记 dirty。  
2. 每帧前：
   - 若 `SphereTransformChanged`：触发/合并 CPU BVH 延迟重建并上传 SSBO。
   - 若任意影响画面参数变化：清空 `accum`，`frameCountSinceReset=0`。
3. Compute Shader：
   - 每像素初始化 RNG（像素坐标 + 帧序号 + 采样序号；按你给的 PCG 变体）
   - 光线生成：Raster -> NDC -> World（`invProj * invView`）
   - 相交：三角形走 BVH（Möller-Trumbore），球体走解析求交，取最近命中
   - 材质：单层介质模型，Schlick Fresnel + transmission 俄轮盘分支
   - GGX：用于粗糙反射与粗糙折射
   - 色散：6 波段，IOR 用 Cauchy（A=1.5,B=0.004）
   - 面光源：顶墙中心 `1x1`，多重采样 + 阴影遮挡
   - 多弹射：迭代实现（上限 `MaxBounces`）
4. 累积：
   - 动态后首帧：等价 `alpha=1`
   - 静态连续帧：`accum = alpha * current + (1-alpha) * accum`
5. 显示：全屏三角形采样 `accum` 到左侧渲染区。

## UI 规格（实现级）
- 左：固定 `1280x720` 渲染区（禁止窗口缩放影响渲染尺寸）。
- 右上（选项卡+分组控件）：
  - 渲染：`MaxBounces`、静态 `alpha`、面光源采样数
  - 三球：位置/半径/粗糙度/IOR/透明度（按球类型显示可用参数）
  - 光源：颜色、强度
- 右下（每 250ms 刷新）：
  - GPU 渲染时间（ms，标注“GPU Timer Query”）
  - 累计帧数（自上次重置）
  - FPS
  - 渲染状态（动态/静态）
- 相机：
  - 初始 `(0,0,5)->(0,0,0)`
  - yaw ±135°, pitch ±20°, distance 2~10
  - 鼠标旋转 + 滚轮缩放，二者都触发 accum 重置。

## CMake 方案（已定）
- 使用缓存变量并提供默认值：
  - `RT_QT6_ROOT`、`RT_GLM_ROOT`
- 将 `RT_QT6_ROOT` 追加到 `CMAKE_PREFIX_PATH` 后 `find_package(Qt6 ... REQUIRED)`。
- `target_include_directories(... PRIVATE ${RT_GLM_ROOT})`。
- 拷贝 GLSL 到运行目录（`POST_BUILD`）。
- 编译标准：`C++20`（或 `C++17`，默认采用 `C++20`）。

## 测试与验收场景
1. **构建启动**
   - 在 MSVC 2022 x64 下成功配置/编译/运行。
2. **画面正确性**
   - Cornell 盒（无前墙）、三球可见，顶面中心 `1x1` 光源生效。
3. **交互正确性**
   - 任一 UI 参数变化即触发重置；球体位姿变化触发 BVH 延迟重建。
4. **光学特性**
   - 可观察反射/折射/全反射；粗糙度变化影响高光与透射模糊；色散可见。
5. **阴影**
   - 面光源采样数升高时软阴影更平滑。
6. **统计信息**
   - GPU ms/FPS/累计帧/动态静态状态按 250ms 刷新并可信。
7. **稳态降噪**
   - 场景静止后噪声逐步降低；动态调整后无明显拖影残留。

## 分阶段里程碑
- M1：CMake + Qt UI 骨架 + OpenGL 上下文 + shader 热加载通路。
- M2：场景数据、CPU BVH、SSBO 上传、基础求交可视化。
- M3：完整路径追踪（反射/折射/GGX/Fresnel/色散/面光源遮挡）。
- M4：累积降噪、动态重置、GPU Timer Query、信息面板。
- M5：README（详细构建/架构/参数说明）+ 验收清单。

## 显式假设
- 不新增第三方库（不引入 Embree/ImGui/Assimp 等），仅 Qt6 + GLM + OpenGL。
- 三球“玻璃变体”通过参数预设体现差异，不做多层材质系统。
- 性能以功能正确为先，不设置最低 FPS KPI。
