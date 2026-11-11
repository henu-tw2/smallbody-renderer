# 龙宫多日反照率渲染实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. 用户已要求持续推进，逐里程碑执行，不重复征求阶段切换许可；本计划不授权覆盖现有未提交修改。

**Goal:** 完成设计文件规定的几何精化、多日反照率与跨日期渲染验证。

**Architecture:** 当前仓库保存主规划与状态。下游任务适配器实现数据、匹配、平差与实验；通用渲染器仅承载通用算法。原始数据只读，科学结果按运行目录保存并记录哈希。

**Tech Stack:** 现有 asp360_new Python/NumPy/SciPy/OpenCV/spiceypy，现有 C++/ISIS/CSPICE/Bullet/TIFF；复用既有工具前先核验环境和接口。依赖缺失应在隔离环境解决，不能悄悄修改已安装科学软件。

**Spec:** `docs/ryugu-realism/design.md`

## Global Constraints

- 原始数据只读；任务影像、核、大尺寸产品不入 Git。
- 渲染合约使用 ET/TDB 秒、米、主动旋转、wxyz；SPICE/DSK 边界千米。
- 不覆盖适配器中的未提交修改。2026-11-09用户已授权本次里程碑完成后的PR、合并与本地同步；一次只推进一个里程碑。
- 跨日训练/验证/测试隔离，测试亮度不得回流到反照率、光度参数与自由增益。
- 未完成真实数据验收，不以合成测试或文件存在代替。
- 唯一生命周期状态位于 `.planning/milestones.v1.json`；只由 manager 脚本变更。

## 执行粒度

本文件是跨阶段执行合同，不预先虚构尚未确定的软件接口。M01 为只读审计，执行精确命令见下；进入后续阶段时，先基于前阶段真实产物，将该阶段 task_plan.md 展开成文件级工作项、接口、失败测试和验收命令，再修改产品代码。前阶段未验收时不得实现后阶段。

## Task 1 / M01：真实输入与环境审计

**Files:**
- Create: `outputs/ryugu-realism/m01/audit_inputs.py`（一次性只读分析程序，随结果保留以复现，不作为库接口）
- Create: `outputs/ryugu-realism/m01/input-audit.json`
- Create: `docs/ryugu-realism/input-audit.md`
- Update: `.planning/milestone-01-input-audit/{task_plan,findings,progress}.md`

**Consumes:** 原始 FITS、元核、DSK、适配器源码、当前构建目录。
**Produces:** 每帧文件哈希、FITS 基本信息/统计、时间来源、SPICE 查询状态及数据缺口；每个加载核路径/哈希；工具和 Git 现场。

- [ ] 读取所有 FITS 的完整头，核验 2880 字节块与像元数据边界、BITPIX/NAXIS/BSCALE/BZERO、文件长度与有效像元；不擅自把特殊值当真实亮度。
- [ ] 核对实际相机/滤光片/产品级别/曝光/时刻，逐帧提取 SPICE 状态，记录成功/失败、范围和有限性；记录曝光中点尚待核实的语义。
- [ ] 对原始 FITS、元核、DSK、实际加载核与标定文件计算 SHA-256。
- [ ] 运行 `PYTHONPATH=/home/gengxun/code/hayabusa2-render-adapter/src /home/gengxun/miniconda3/envs/asp360_new/bin/python -m unittest discover -s /home/gengxun/code/hayabusa2-render-adapter/tests -v`，保存真实测试计数和输出。
- [ ] 在正确运行环境下执行 `ctest --test-dir /home/gengxun/code/smallbody-renderer/.build --output-on-failure` 与现有 Python 合约测试，记录成功或失败；缺少动态库不等于源码缺陷。
- [ ] 生成审计 Markdown，逐项注明哪些事实已核验，哪些只是候选；检查审计完整性后生成 M01 完成证据。

## Task 2 / M02：可追溯标定与冻结日期划分

**Files:** 下游 `src/hayabusa2_adapter/radiometry.py`、`tests/test_radiometry.py`；结果索引 `outputs/ryugu-realism/m02/{calibration-manifest,splits}.json`。
**Consumes:** M01 清单、官方标定说明与匹配到原始帧的 L2d 或完整标定参数。
**Produces:** 每帧线性 I/F 路径/单位/掩膜/标定来源/哈希，四日分组与每折训练验证测试映射。

- [ ] 对照官方说明核实 L2a 是否已做任何偏置或其他处理；不能重复校正。
- [ ] 优先只读查找已有同帧 L2d；其次核实公开归档对应产品；若需要本地标定，明确偏置/暗电流/平场/非线性/曝光/太阳距离/通带响应的适用公式与参数。
- [ ] 先写已知标定样例和无效输入拒绝测试，再实现单一标定入口；核对至少一帧官方产品或独立标准，不能用实现自身生成期望值。
- [ ] 按日期冻结拆分，检测文件/时间/衍生产品重叠及测试亮度泄漏。
- [ ] 写阶段报告并验证，缺失必要标定依据时保存阻塞与唯一下一步。

## Task 3 / M03：多日连接点网

**Files:** 下游 `src/hayabusa2_adapter/{matching,tracks}.py` 与对应 tests；`outputs/ryugu-realism/m03/{tracks,network-report}.json`。
**Consumes:** 固定相机内参、M01 SPICE/DSK、M02 影像/拆分；匹配用影像与科学影像分别记录。
**Produces:** `(frame_id,track_id,u_px,v_px,sigma_px)` 观测、track 三维初值和质量标志、独立检查点索引。

- [x] 写重复观测冲突、错误对应、多视轨迹合并和真实几何可见性检查测试。
- [x] 由预测共同可见区域构建影像对图，先同日后跨日匹配；对匹配失败原因分组，不任意放宽粗差阈值。
- [x] 三角化、正深度/交会角/表面距离检查；记录断开分量，不将未连通序列伪装为一个整体网。
- [x] 输出表面覆盖、每帧连接数、轨迹长度与连接图；独立地标在平差前隔离。

## Task 4 / M04：约束束平差

**Files:** 下游 `src/hayabusa2_adapter/bundle.py`、`tests/test_bundle.py`；`outputs/ryugu-realism/m04/{adjusted-states,geometry-report}.json`。
**Consumes:** M03 观测和点初值、SPICE 先验、DSK 误差模型。
**Produces:** 每帧修正位姿及合约导出、优化点、先验/基准/残差/独立检查结果。

- [x] 写已知扰动恢复、基准约束、弱交会拒绝和粗差稳健性测试。
- [x] 固定内参/网格/自转，优化位姿与点；旋转使用局部参数，先验单位明确，表面权重不视为无限大。
- [x] 用独立地标、轮廓和修正幅度验收；门槛为中位数<=0.5px、P90<=1.5px；未达标调查误差来源并保留失败证据。
- [x] 固定位姿后再进入光度；如有后续几何更新，废弃旧光度结果并重跑。

## Task 5 / M05：光度与基线

**Files:** 下游 `src/hayabusa2_adapter/photometry.py`、对应 tests；必要通用改动为 `cpp/rendering/` 与通用 tests；`outputs/ryugu-realism/m05/{models,baseline-report}.json`。
**Consumes:** 冻结几何、线性科学影像、i/e/alpha 与可见/受光掩膜。
**Produces:** E0/E1 与 LS 类模型、单位和归一化明确的科学预测与显示映射。

- [x] 测试角度定义、日夜/遮挡、相位函数归一化和非有限值拒绝；先测试后实现。
- [x] 先输出无拉伸浮点科学产品，单独保存显示图；不让 UInt16 截断影响光度拟合。
- [x] 用训练/验证拟合全局参数，冻结角度阈值；记录条件数、参数相关性和帧间标定残差。
- [x] 固定 ambient 的既有结果仅作为基线；新的科学模型显式区分直射、杂散光和背景。

## Task 6 / M06：多日反照率

**Files:** 下游 `src/hayabusa2_adapter/albedo.py`、对应 tests；`outputs/ryugu-realism/m06/{albedo-manifest,reconstruction-report}.json`。
**Consumes:** M05 模型、训练像元与对应面片、观测误差。
**Produces:** 面片相对反照率、有效标志、观测/日期数、权重与不确定度；未知区域保持未知。

- [x] 测试尺度简并消除、阴影/饱和拒绝、不同视角恢复及未观测面片行为。
- [x] 按分辨率/角度/配准误差加权稳健估计，报告误差传播；不把模型残差 RMS 直接称作完整统计置信区间。
- [x] 审核测试亮度不能参与全局参数、反照率、自由增益或阈值选择。
- [x] 图像分辨率足以支持时才升级细纹理；先交付面片级完整闭环。

## Task 7 / M07：跨日实验

**Files:** 下游 `src/hayabusa2_adapter/evaluate.py`、对应 tests；`outputs/ryugu-realism/m07/{metrics.json,ablation-report.md}` 与图组。
**Consumes:** 冻结四日协议与每折只用训练数据生成的模型。
**Produces:** 逐帧/日期/角度分组指标、共同掩膜、覆盖、资源与扩展决策。

- [ ] 测试指标零误差样例、空掩膜拒绝、按日期等权聚合和覆盖计算。
- [ ] 对四个留出日期逐折运行 E0/E1/E2；逐帧后匹配亮度仅作为附加诊断。
- [ ] 依可辨识性和残差决定 E3/E4，明确训练与调参范围；不为正结果修改主指标。
- [ ] 输出固定显示范围的实拍/渲染/残差及误差随角度变化图；只根据真实结果做结论。

## Task 8 / M08：交付复现

**Files:** `docs/ryugu-realism/final-report.md`、`outputs/ryugu-realism/m08/reproduction.json`、下游 README 与命令入口。
**Consumes:** M01–M07 所有带哈希产物和真实测试记录。
**Produces:** 可独立重放的工作流与实验结论。

- [ ] 在新输出目录重跑冻结的代表性案例并核对数值容差、掩膜和哈希来源；随机算法固定种子并记录版本。
- [ ] 复核运行命令、路径/单位、源数据许可证、输入缺口、各测试计数和未解决局限。
- [ ] 总报告明确“工程验收通过”与“科学改进被支持/未被支持”，不得承诺预设效果。
- [ ] 完成最后阶段证据验证，生成最终产品索引和可恢复状态。

## Self-Review

覆盖设计的几何、光度、数据隔离、覆盖、不确定度与真实对照要求；未假设所有276帧都有可用姿态或已标定。后续阶段细化是依赖前阶段测量的必要设计动作；活动阶段与完成状态以里程碑注册表为准，不能以本初始计划的旧清单推断状态。
