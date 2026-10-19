# DST Render Probe — 原生图形拦截可行性验证（实验性）

**这不是已完成的光影 MOD。** 它是一个用于判定“是否能接管《饥荒联机版》逐次绘制”的最小 Windows/ReShade Add-on。首次启动仅监控，不修改画面。打开明确的测试开关后，允许按 draw-call 序号**临时跳过一条绘制命令**，观察树木、人物、UI 或其他内容是否消失。

## 为什么做它

过去的 Lua PostProcessor 只能改最终画面。要实现“物体级动态受光 + 2.5D 软阴影”，首先要证明可以拿到物体绘制指令；本程序验证的只是这个前置条件，不等同于已经得到精灵语义、法线和高度。

## v0.3：按 F6 抓取一帧纹理记录

v0.2 的真实离线测试中，用户观察到 F7 后短暂停顿再恢复；两次日志分别记录 33639、32409 次跳过请求，约 500ms 结束。这与暂时停止画面更新一致，但仍未识别任何独立物体。

v0.3 增加 **F6 短按一次**：关闭当前屏蔽，在该运行时的下一个 present 间隔中，记录同一 D3D9 设备上的 draw / draw_indexed，保存到插件目录旁的 `DSTRenderProbe-captures/`。文件包括 CSV 和同名 `.meta.txt`。最多记录 4096 个绘制回调，超过会在元数据和日志中明确标记截断。

捕获字段包括绘制序号、索引/顶点数量、实例数、偏移、着色器句柄、渲染目标与视口，以及像素着色器 0～15 纹理槽的资源句柄、尺寸、类型、格式和查询结果。直接读取原生 D3D9 当前状态，避免漏掉状态块恢复的绑定。返回的 COM 对象在查询后释放，不保留其指针供后续访问。

保存 CSV 后，插件请求 ReShade 保存带相同 capture 标记的游戏截图，截图目录与格式遵循 ReShade 的截图设置。只有 `[screenshot_saved]` 回调及实际文件存在才能确认截图保存成功；`screenshot_requested` 仅表示请求。截图可能短暂卡顿，可能包含游戏自身 UI 和 ReShade 叠加信息；请先关闭 ReShade 面板。

建议：站在树木附近按 F6，等待约 2 秒；移动到不同构图再按一次 F6。先确认纹理/着色器记录和截图完整，再分析哪些纹理页及绘制组随场景变化。没有语义分类、atlas UV 或顶点内容，不能把句柄直接称为某一棵树。纹理/着色器句柄仅本次进程有效，资源释放后也可能复用，跨运行不能比较。

按住 F6 不连续抓取；失去前台焦点、销毁运行时会取消尚未完成的抓取。仅支持 D3D9，其他后端会输出不支持提示；捕获间隔是 present 回调的边界，不能保证同设备其他交换链的绘制完全排除。没有读取纹理像素、顶点、常量、UV、间接绘制或 GPU 时间，也没有改变材质、光照和阴影。没有自动上传任何捕获文件。

`scripts/summarize_capture.py capture.csv` 可按纹理页汇总绘制组；这些统计只供定位候选，不会自动认出树木或人物。

测试包含原有两项，以及有界捕获/CSV 测试和原生 D3D9 已知纹理测试。如果 CI 没有可用图形适配器，原生测试会明确标为 skipped，并写入包内 `test-results.xml` 与 `build-info.json`，不会冒充通过。

## v0.2：屏蔽无可见变化时的诊断

上一版已在本机真实 ReShade 6.8.0 / DST x64 中注册，收到非零绘制计数和快捷键日志；用户在离线世界报告画面始终相同。日志中的 target 仍集中在最初几条绘制。尚无证据证明已经成功隐藏任何可见内容，不能宣布物体级拦截验收通过。

v0.2 默认仍只监控。新增操作：

- **F7 短按一次**：在随后最多 500 毫秒内，对本插件收到的 draw / draw_indexed 全部返回跳过请求，然后自动恢复；可能短暂黑屏、缺图或画面停留。按住不延长，松开再次按下才会重启测试。只在本机离线测试世界操作。
- **F8**：开关单条绘制屏蔽。按住不再反复开关。
- **F9 / F10**：目标 -1 / +1，按住 400ms 后每 60ms 重复；**Shift+F9/F10** 每次移动 100 条。
- **F11**：立即取消两种屏蔽并将目标重置为 1。失去前台焦点也会取消屏蔽。

日志显示 `v0.2`，新字段 `hits` 表示上一统计帧命中 `counted_target` 的次数，`skip_requests` 表示回调在上一统计帧返回 true 的次数；这只是向 ReShade 提交了跳过请求，不等于已证明 GPU 输出或某个物体变化。`target` / `skip` 是处理本次按键后的下一帧设置，`counted_target` 是这些计数使用的旧目标，因此在按键那一帧二者可以不同。

F7 会记录 `burst_start` / `burst_end` 和累计 `burst_requests`，计时截止也在 draw 回调检查，即使没有继续触发 present，也不会无限持续测试。若有大量请求但画面仍无变化，下一步检查真实图形后端和对应绘制类型，而不是声称增强已经生效。本插件仍不监控 indirect draw、clear、copy、dispatch 等其他操作，多个队列或交换链的分帧计数仍属近似。

新增确定性状态测试覆盖：单条请求、命中计数、按住不重复开关、按键重复与百条步进、半秒测试到期、无 present 时到期、失去焦点取消和 F11 复位。它不等于真实 Windows 输入或游戏画面验证。

## GitHub 云编译与下载

仓库根目录的 `.github/workflows/build-windows-x64.yml` 在 push / PR 时自动执行，也支持合并到默认分支后的手动运行。进入仓库 **Actions → Build DST Render Probe (Windows x64)**，选择对应提交且已成功的运行，在 Artifacts 下载 `DSTRenderProbe-Windows-x64-<提交SHA>`。

解压后使用 `DSTRenderProbe.addon64`，无需把整个源码放进游戏。包中含 `build-info.json`（源码提交、SDK 版本、构建链接和验证边界）及 `SHA256SUMS.txt`。工作流先编译、运行模拟宿主测试、验证 AMD64/PE32+、元数据导出和 DLL 依赖，全部通过后才上传安装包。

固定使用官方 ReShade **v6.6.2 / API 18** 头文件，提交 `7d13ead8f630d045b9e7e2ac2ce18b2e7e5b25c3`。运行时需要支持该 API 的 **ReShade with full add-on support**，并匹配实际游戏图形 API；SDK 固定版本不代表只允许同版本宿主，也不保证任意宿主版本都兼容。

模拟宿主只验证 DLL 加载、API 注册失败处理、回调注册、默认不屏蔽、计数重置、心跳日志与卸载。它不创建真实 GPU 设备，不验证快捷键干预或游戏兼容性。

## 构建环境

- Windows 10/11，Visual Studio 2022 Desktop development with C++，CMake 3.21+。使用静态 MSVC 运行库，不要求额外分发 VC 运行库 DLL。
- 来自官方仓库的 ReShade Add-on API 头文件： https://github.com/crosire/reshade （克隆后 `include/reshade.hpp`）。
- 使用支持 Add-on 的 ReShade 构建版本，确认图形 API、架构（游戏 x86/x64）均匹配。**不要同时往游戏目录放置 DXVK 和多个 d3d9.dll**。不要在公共服务器、无法接受封禁风险的账户上测试。

PowerShell 示例（路径按实际调整）：

```powershell
git clone --branch v6.6.2 --depth 1 https://github.com/crosire/reshade.git C:\src\reshade
cmake -S . -B build -A x64 -DRESHADE_INCLUDE_DIR=C:/src/reshade/include
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python scripts/verify_pe.py build/Release/DSTRenderProbe.addon64
```

编译后查看 `build/Release/DSTRenderProbe.addon64`。本工程仅生成 Windows x64 插件，不适用于 32 位游戏。将文件按 ReShade 官方文档放入 add-on 搜索路径（通常与 ReShade 安装目录相同）。

## 测试顺序（仅本机离线）

1. 备份游戏目录，记录启动方式与图形 API。先安装与游戏渲染 API 匹配的官方 ReShade，确认能够启动。
2. 添加 `DSTRenderProbe.addon64`，启动本地离线/测试场景；检查 ReShade 日志有没有 `DST Render Probe` 的记录。
3. 默认 `skip=OFF`，画面不应改变。观察日志中的每帧 `draws` 是否大于 0。每 180 帧输出一次状态。
4. F8：启用/停用“跳过目标 draw call”；F9：目标序号 -1；F10：目标序号 +1；F11：关闭干预并重置序号为 1。
5. 在相机与场景尽量静止时切换目标序号，查看是否出现一个局部物体/图层消失。**F8/F11 可恢复**，退出游戏、移除 `.addon64` 即可取消扩展。

## 判定与局限

- 若日志始终 0：首先检查是否采用了适配的图形后端、Add-on 是否加载、事件是否支持；不是“引擎不可破解”的结论。
- 若部分物体/图层可被逐绘制屏蔽：仅证明可拦截指令，**还不能证明**能将某个绘制命令稳定映射到某种物体。
- 若跳过某 draw 导致画面大面积消失：批处理或合成 Pass 可能包含多个对象；需要升级到纹理和几何数据层研究。
- ReShade 回调可能也统计由 ReShade 自身提交的绘制；帧计数只做粗略诊断，禁用任何会影响测量的额外效果。
- 游戏更新、驱动、Steam 叠加层、ReShade 版本、D3D9/D3D9Ex 差异会影响结果。
- **CI 编译和模拟宿主测试的实际结果见 Actions；它们不代表已在 DST 或真实 ReShade 中验证。尚无游戏内运行、画质或性能验证。**

## 下一阶段

见 `docs/AGENT_TASKS.md`：确认拦截稳定后，接入纹理/顶点/矩阵追踪和语义映射，最终做 2.5D 法线、高度缓冲、地面阴影及独立渲染 Pass。

参考：
- ReShade Add-on API https://crosire.github.io/reshade-docs/
- ReShade API trace example https://github.com/crosire/reshade/blob/main/examples/04-api_trace/api_trace_addon.cpp
- ReShade depth tracking example https://github.com/crosire/reshade/blob/main/examples/09-depth/generic_depth_addon.cpp

## 恢复来源与配置修正

源码来自上一段对话下载的 `DST-RenderProbe.zip`，保留原来的诊断功能及任务书。本轮未找回 `DST-RenderProbe-GitHubActions.zip` 的可访问附件，因此云构建配置是基于已恢复源码重新补齐，而非声称恢复了该 ZIP 的逐字副本。

修正包括显式链接 `user32`、只输出 x64 `.addon64`、固定 SDK 提交、静态 MSVC 运行库，以及防止注册失败后的 DLL detach 注销未注册事件。静态 CRT 构建不调用 `DisableThreadLibraryCalls`。增加名称/说明导出以便在 Add-ons 界面识别。

回滚：关闭游戏，移除 `DSTRenderProbe.addon64` 后重启游戏。仓库回滚可关闭 PR，默认分支原文件不受影响。测试记录请附实际 ReShade 日志、游戏版本/API、截图和帧耗时；尚未测量的项目不可填入示例值。
