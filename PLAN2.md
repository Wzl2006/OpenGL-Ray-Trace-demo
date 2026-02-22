# 光线追踪问题修复与降噪增强实施计划（当前分支）

## 摘要
本轮目标是一次性解决你列的 4 类问题：  
1) 球顶黑影（法线/自相交/阴影错误）  
2) 透明与半透明材质“看起来未生效”  
3) 噪点严重（引入路径采样优化）  
4) 墙体改为单向（仅盒内可见）  
并按你确认结果新增 **每帧每像素 SPP** 控件，支持 **>1000**。

已锁定偏好：
- SPP：每帧每像素
- 降噪：先做采样优化（不接第三方库）
- 墙体单向策略：只保留盒内可见面

---

## 现状诊断（基于代码探查）
1. **透明/半透明失效主因**：  
   `intersectSphere()` 与 `intersectTriangle()` 当前会把法线直接翻成“永远迎向入射光线”，导致 `entering = dot(ray.dir, n) < 0` 基本恒为 true，折射永远按“入射空气->介质”处理，缺失“出射介质->空气”，内部路径不正确。

2. **球顶黑影主因**（组合问题）：  
   - 阴影与后续弹射统一用固定 `+normal*eps`，未按 outgoing 方向做符号偏移，容易在曲面顶点与 grazing angle 出现自相交/漏光。  
   - 直接光照目前只做 Lambert 形式，透射材质路径能量评估偏弱，视觉上更易“发黑”。

3. **墙体双向问题主因**：  
   三角形求交没有背面剔除（且法线被翻向），所以从外部也能命中并反射。

4. **噪点问题主因**：  
   当前每帧每像素单样本，无 BRDF+光源 MIS 权重，且无高 SPP 控件。

---

## 公开接口 / 类型改动（决策完成）
### 1) 参数与GPU常量
- `RenderParams` 新增：
  - `int sppPerPixel`（默认 1，范围 1~2048）
- `GpuRenderParams` 扩展字段（保持 16-byte 对齐）：
  - `options` 增加 `sppPerPixel` 槽位（重排但不破坏当前绑定数量）

### 2) 命中记录结构
- Shader `Hit` 扩展：
  - `vec3 geometricNormal`
  - `vec3 shadingNormal`
  - `bool frontFace`
- 交点阶段只存几何法线；着色阶段根据 `frontFace` 派生 `shadingNormal`，不在求交函数里硬翻法线。

### 3) UI 控件
- 渲染设置页新增：
  - `SPP (per frame)` 滑块 + 数值显示（线性 1~2048，确保可取 >1000）
- `RenderWidget` 新增 `setSppPerPixel(int)` 并触发 `RenderParamChanged` + accum reset。

---

## 详细实现步骤
### A. 修复法线、折射与自相交（先做）
1. 修改 `intersectTriangle/intersectSphere`：
   - 不再把法线翻成迎向光线；
   - 返回几何法线与 `frontFace`（`dot(ray.dir, geometricNormal) < 0`）。
2. 着色统一使用：
   - `shadingNormal = frontFace ? geometricNormal : -geometricNormal`
   - 折射 `eta`：`frontFace ? (1/ior) : ior`。
3. 射线偏移改为方向相关：
   - `origin = hit.position + shadingNormal * (dot(nextDir, shadingNormal) > 0 ? eps : -eps)`
   - 阴影射线同理，避免球顶黑斑自遮挡。

### B. 墙体改为单向（盒内可见）
1. 三角形求交中启用背面剔除（按 Cornell 内壁绕序）：
   - 使用 det 符号做 one-sided culling（仅命中内壁朝向）。
2. 保持球体双面介质行为（球体不做背面剔除）。

### C. 透明/半透明材质真正参与渲染
1. 分支概率重构：
   - `P_refract = transmission`
   - `P_reflect = (1 - transmission) * Fresnel`
   - `P_diffuse = 1 - P_refract - P_reflect`（下限 clamp）
2. 透射路径：
   - 正确处理进/出介质与全反射；
   - 保留 Cauchy 色散与 Beer 吸收。
3. 半透明路径：
   - 保留 diffuse+transmission 共存，按概率采样并做 PDF 归一化。

### D. 降噪第一阶段：采样优化（不引入外部库）
1. **每帧多样本 SPP**：
   - Compute 每像素循环 `for (s=0; s<sppPerPixel; ++s)`。
2. **直接光照 NEE（面积光）**：
   - 保留当前光源采样，但改成按 BSDF 贡献评估。
3. **BRDF 重要性采样**：
   - 漫反射余弦加权；
   - GGX 反射/折射按微表面分布采样。
4. **MIS（power heuristic）**：
   - 合并 light sampling 与 BSDF sampling，降低 firefly/噪点。
5. 俄轮盘终止（>= 第3 bounce）：
   - 用 throughput 动态生存概率稳定能量与噪声。

### E. UI 与状态同步
1. MainWindow 渲染设置加入 SPP 控件（显示当前值，可达 2048）。
2. `Renderer::updateParamsBuffer()` 上传 SPP。
3. 参数变化保持“动态清空 accum + 首帧 alpha=1”的既有策略。

---

## 验收与测试场景
1. **法线/折射正确性**
   - 透明球可明显看到背景折射变形；
   - 相机绕球移动时无“始终同向折射”异常。
2. **黑影修复**
   - 球顶黑斑显著减轻/消失；
   - 强光+高粗糙度下无明显自阴影断层。
3. **墙体单向**
   - 从盒外视角不再看到墙体外侧反射命中；
   - 盒内渲染保持正常。
4. **SPP 功能**
   - UI 可设置 SPP > 1000（例如 1024/1536）；
   - SPP 提升时 FPS下降、噪点下降，行为符合预期。
5. **噪点改善**
   - 固定相机与参数下，较旧版同帧数噪声更低、亮斑更少。
6. **回归检查**
   - BVH 遍历、球体解析求交、面光源采样、GPU timer UI 仍正常。

---

## 默认值与假设（显式）
- SPP 上限默认设为 **2048**（满足 >1000 且避免 UI 过大）。
- `maxBounces` 保持当前上限 20，不再继续上调。
- 降噪本轮仅做“采样优化 + MIS + RR”，不接 OIDN/OptiX。
- 球体继续作为介质双面求交；“单向”只作用于墙体三角形。
