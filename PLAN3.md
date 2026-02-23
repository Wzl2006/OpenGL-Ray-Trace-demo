# 0.3.0 落地计划（Caustics 增强，不改主路径追踪框架）

## 1. 目标与边界

### 目标
- 在现有 `PT + NEE + MIS + OIDN` 架构上，加入可控的 **Caustics 预通道**，显著降低玻璃焦散区域（地板/墙面）高方差噪声。
- 不推翻当前主渲染管线，不引入 BDPT/VCM 级别大重构。

### 不在 0.3.0 范围内
- 不实现完整双向路径追踪（BDPT）或 VCM。
- 不改 UI 大布局（你这轮先做 UI 优化）。
- 不引入新的重量级依赖库（保持 Qt + OpenGL + OIDN）。

---

## 2. 当前问题归因（为什么要做）
- 低差异序列替换后，噪声分布更均匀，但焦散路径（SDS）仍是主噪声源。
- 仅靠采样序列优化无法显著改善折射聚焦热点区域的收敛速度。
- 需要一个“专门对焦散友好”的估计器补充项。

---

## 3. 技术方案总览

采用 **Light Tracing Caustic Cache（光源路径焦散缓存）**：

1. 在 compute 中新增一个“焦散 pass”：
   - 从面光源发射光子（light path）。
   - 追踪经过 specular/transmission 事件后的路径。
   - 当路径命中漫反射表面时，将贡献 splat 到 `causticTexture`。

2. 在主路径追踪 pass 中：
   - 继续原有 `PT + NEE + MIS`。
   - 在首个漫反射命中点读取 `causticTexture` 并叠加。

3. 累积策略：
   - `causticTexture` 独立累积，场景变化时与 `accum` 同步重置。
   - 静态时采用低 alpha 累积，动态时首帧强制重置。

---

## 4. 渲染管线改造（实现顺序）

### M1: 基础资源与调度
- 新增纹理：
  - `causticAccumTexture (RGBA32F)`
  - `causticOutputTexture (RGBA32F)`
- 新增 shader：
  - `caustics.comp.glsl`
- Renderer 调度顺序改为：
  1) `caustics.comp`（先）
  2) `raytrace.comp`（后，读取 caustic 输出）
  3) OIDN（可选）
  4) fullscreen 显示

### M2: 焦散光子追踪核心
- 光子发射：
  - 光源矩形均匀面积采样 + cosine 半球方向采样。
- 路径传播：
  - 限制 `maxPhotonBounces`（默认 6）。
  - 仅在经历过至少一次 specular/transmission 后，才允许向漫反射面写入焦散贡献。
- 写入策略：
  - 直接写 `causticOutputTexture`（初版）。
  - 若有并发噪点/竞态问题，再升级为 tile 缓冲 + resolve。

### M3: 主路径融合
- 在 `raytrace.comp.glsl` 增加对焦散纹理采样：
  - 仅在“首个漫反射命中”叠加（避免重复计数）。
  - 叠加权重参数化（`causticStrength`，默认 1.0）。
- 保留原 NEE/MIS 全逻辑不动。

### M4: 参数与重置联动
- RenderParams 新增：
  - `causticEnabled`（bool）
  - `causticPhotonsPerFrame`（int）
  - `maxPhotonBounces`（int）
  - `causticAlpha`（float）
  - `causticStrength`（float）
- 与现有 dirty flag 联动：
  - 相机/材质/光源/球体变化时重置 caustic 累积。

### M5: 调试可视化
- 显示模式新增（仅调试）：
  - Beauty
  - Caustic Only
  - Beauty + Caustic
- 便于确认焦散贡献是否正确、是否过曝。

---

## 5. 数据结构与接口变更（0.3.0 预计）

### C++（Renderer/Scene）
- `RenderParams` 新增字段（见 M4）。
- `Renderer` 新增：
  - `GLuint m_causticAccumTexture`
  - `GLuint m_causticOutputTexture`
  - `GLuint m_causticProgram`
- `updateParamsBuffer()` 扩展参数 UBO 布局（需同步 GLSL）。

### GLSL
- 新文件 `src/render/glsl/caustics.comp.glsl`。
- `raytrace.comp.glsl` 新增只读 `caustic` image/sampler 绑定。
- `fullscreen.frag.glsl` 可扩展调试显示模式开关。

---

## 6. 默认参数建议（首版）
- `causticEnabled = true`
- `causticPhotonsPerFrame = 131072`（可按机器调）
- `maxPhotonBounces = 6`
- `causticAlpha = 0.08`
- `causticStrength = 1.0`
- 动态交互时可临时降为 `32768` 光子/帧，静止后恢复。

---

## 7. 验收标准（必须达成）

### 功能正确
- 玻璃球下方聚焦亮斑可见且位置正确（随相机/球体变化合理变化）。
- 开关 `causticEnabled` 可明显切换焦散项。
- `Caustic Only` 模式下可看到主要焦散能量分布。

### 稳定性
- 不出现 NaN/Inf 爆点、全屏闪烁、随机花屏。
- 动态参数变化后，caustic 累积正确重置，无旧帧拖影。

### 质量收益
- 同等时间下，焦散区域噪声明显低于 0.2.3。
- 非焦散区域视觉基本不退化。

---

## 8. 性能与风险

### 主要风险
- 额外 pass 造成 GPU 压力提升。
- 并发写入导致局部热点噪点或条纹。
- 与 OIDN 叠加时可能出现过平滑或亮度偏移。

### 缓解策略
- 光子预算分档（低/中/高）并可动态降档。
- 初版先保证正确性，再做热点优化（tile 归约/分块）。
- 保留可快速回滚路径：`causticEnabled=false` 退回现有渲染。

---

## 9. 实施节奏建议（你 UI 优化后执行）
1. 先落 M1+M2（只做 caustic pass 与 caustic only 显示）。
2. 再做 M3（融合到 beauty）。
3. 再做 M4（参数 + dirty 联动）。
4. 最后做 M5（调试 UI 与 README 完整文档）。

---

## 10. 版本规划
- 当前稳定基线：`alpha-0.2.3`
- 0.3.0 开发建议分支：`feature/caustics-0.3.0`
- 里程碑标签建议：
  - `alpha-0.3.0-m1`（资源与调度）
  - `alpha-0.3.0-m2`（焦散可见）
  - `alpha-0.3.0-m3`（融合完成）
  - `alpha-0.3.0`（正式）
