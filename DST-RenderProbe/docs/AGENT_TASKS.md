# AI Agent 开发任务书 — DST Enhanced Renderer

## 目标

用原生图形扩展，让《饥荒联机版》在保持原有玩法、角色动画、UI 的前提下，实现明显的方向光、手绘风格法线受光、树木/建筑软阴影及水面高光。研究和运行限制于本机离线测试，不编写绕过游戏安全机制的代码。

## 切勿做的事

1. 不重启 Lua PostProcessor 路线；它无法提供精灵语义、几何与深度。
2. 不把 DXVK 当作自动拥有深度/法线信息的光照引擎。
3. 不在阶段 A 未通过前引入 Vulkan 光追、RTX Remix 或多个叠加 hook。
4. 不修改或覆盖原游戏的核心文件；测试过程务必可回滚。

## A — 测绘（Go/No-Go）

输入：真实客户端进程信息、日志、游戏版本、渲染 API、64/32-bit、显卡与驱动。

- A1 运行 RenderProbe（当前项目）确认渲染事件和 Draw Call 数量；输出日志、截图和帧统计。
- A2 追踪 shader / texture binding / pipeline / viewport / blend / draw indexed，捕获局部物体序号稳定性；可参考 ReShade 官方 `04-api_trace` 示例。
- A3 建立唯一可重复的纹理内容标识（hash）与“atlas 子矩形 -> 精灵帧”匹配策略。**资源句柄不可当跨运行稳定 ID**。
- A4 验证一个树冠、一个家具与一块 UI 是否可分离标识。如不可，报告是 atlas batching 还是数据访问缺失，不进入 B。

验收：有证据证明至少一类物体能在跨 100 帧中被识别，UI 可单独排除；60 FPS 基线与干预帧耗时有记录。

## B — 语义化贴图层

- 将纹理页/UV 矩形/动画帧与实体类别关联。缓存识别结果，不得每次 draw 做 CPU 图像识别。
- 为少量对象建立自定义材质 profile：tree, building, grass, water, UI-excluded。
- 生成或手工校验 normal map、height proxy、mask；特别注意原版黑色轮廓线不要被打亮。
- 证明对象屏幕坐标、朝向、地面接触点的可恢复性；否则采用人工锚点代理，不假装获得完整 3D 深度。

验收：树/房/草三类中至少两类被准确分类，动态画面中稳定，误识率可测量。

## C — 自定义光影 Render Graph（可由 D3D9 或独立后端实现）

```
Sprite draw telemetry
    ├── material classification
    ├── sprite normal pseudo-GBuffer
    ├── sprite height proxy / contact point
    └── UI classification
          ↓
Lighting pass → Shadow mask pass → Scene-only color composite → UI unchanged
```

- C1 单个树木可随太阳方向变化明暗，不改变油墨轮廓线。
- C2 地面软阴影与地砖/草地正确叠加，动态影长与遮挡近似可信。
- C3 UI 不受光照影响；火把等原有游戏发光效果避免双重叠加。
- C4 水面独立做局部高光与水波偏移，避免全屏泛光。

验收：可运行的游戏内实时截图、录屏、双时段比对和帧耗时；不能用概念图代替。

## D — Performance / regression

- 1080p 在目标显卡上记录 P50/P95 GPU 帧时间，不预设所有设备都能到 60FPS。
- 验证人物移动、昼夜切换、进入/退出洞穴、窗口重置、屏幕分辨率变化、ALT+TAB。
- 导出诊断日志和场景一致性截图；任何 GPU device-lost 或黑屏是失败。

## Agent 执行规则

每一个 PR 必须包含：验证假设、代码变更、可复现实验步骤、回滚方案、日志/帧耗时。最多只并行开发彼此接口稳定的模块。研究阶段不要写“已兼容当前 DST”“完全实时 GI”“替代引擎”之类未经验证的结论。
