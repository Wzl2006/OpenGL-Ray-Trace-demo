# 更新日志（alpha 0.1.0 ~ alpha 0.2.3）

## alpha 0.1.0
- 初始化可运行 Demo：Qt6 + OpenGL 4.3 Compute + CMake 工程骨架。
- 完成 Cornell Box + 三球（解析求交）+ CPU BVH/SSBO 上传。
- 实现基础路径追踪、累积缓冲、UI 控件与统计面板。

## alpha 0.1.1
- 重点修复 `raytrace.comp.glsl`：反射/折射/粗糙度与采样逻辑稳定性增强。
- 改善高亮噪点与路径分支行为，作为后续版本稳定基线。

## alpha 0.2.0
- 接入 OIDN（OpenImageDenoise）CPU 后端。
- 新增异步降噪线程与结果回传，支持静态场景节流触发。
- CMake 增加 OIDN 可配置路径与运行时 DLL 自动拷贝。

## alpha 0.2.1
- 增加降噪调度参数默认策略：
  - `denoiseMinAccumFrames = 16`
  - `denoiseIntervalMs = 200`
  - `denoiseBlend = 0.85`
- 显示端改为 `raw/denoised` 可混合输出，降低过度平滑感。

## alpha 0.2.2
- 增加 GPU in-flight 保护（Fence），限制同一时刻只提交一个重计算帧，防止高 SPP 下整机卡死。
- 后续在 0.2.3 中进一步修正了在途帧显示路径（避免花屏）。

## alpha 0.2.3（当前）
- 将路径维度随机源全面替换为低差异序列（Sobol 风格）：
  - 分支选择、GGX/半球采样、波长选择、RR 全部改为固定维度槽位采样。
- 光源采样改为“每像素独立低差异采样”，移除 workgroup 共享光源采样点带来的相关性。
- 保持主像素 Hammersley 不变，更新 README 的采样架构与后续路线说明。
