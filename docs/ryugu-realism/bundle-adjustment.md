# M04：SPICE 与 DSK 约束平差验收

2026-11-09。本轮只完成 M04；M05 尚未启动。验收运行是 `outputs/ryugu-realism/m04/run-v3/`。M01–M03 的输入、划分和检查轨迹保持不变，失败运行另存而未覆盖。

## 结论与范围

276 帧四日 ONC-T 550 nm 影像完成约束束平差。40,523 个独立检查观测的中位数/P90 从 **0.339/0.935 px** 降为 **0.217/0.624 px**，满足原定 ≤0.5/≤1.5 px 门槛。41 项单元测试和 9 组保存产物验证通过，均为零失败、零跳过。

这支持独立像点预测改善。轮廓代理指标略有变差，尚不能断言全部几何指标、绝对制图精度或最终反照率渲染效果改善。后续光度阶段须固定本阶段几何，并继续沿用 M02 的日期隔离。

## 数据与独立性

沿用 M03 的 11,135 条拟合轨迹、169,399 个拟合观测及 2,800 条完整隔离的检查轨迹。所有检查轨迹均不进入平差目标函数。初始几何在求解前冻结检查资格：预测每个检查像点时仅用同轨迹其他影像的至少两条射线，要求最大交会角 ≥1°、条件数 ≤1e8、初始投影有限且正深度；不按误差大小筛选。

初始几何排除 188 个检查观测；其余 40,523 个在初始/调整后使用同一集合，调整后无预测失败。验证器另用射线法方程求和后减去被留出观测的代数独立重算，避免待预测像点参与自身三角化。这是独立连接点检查，不是测量 GCP 的绝对精度验证；该多日几何网也不等同于后续跨日光度测试。

| 日期 | 检查观测数 | 调整后中位数 px | 调整后 P90 px |
|---|---:|---:|---:|
| 2018-06-30 | 18,159 | 0.2167 | 0.6249 |
| 2018-07-03 | 1,938 | 0.2028 | 0.5943 |
| 2018-07-10 | 18,638 | 0.2180 | 0.6202 |
| 2018-07-12 | 1,788 | 0.2349 | 0.6509 |

## 模型、基准与求解

固定内参、DSK 网格与自转，固定第一帧 `hyb2_onc_20180630_070007_tvf` 的位姿作为基准，优化其余位姿和拟合点。旋转用右乘局部增量；解析雅可比经中心差分核验。目标函数包含 1 px 分组二维 Huber 像点项、10 m 位置先验、0.001 rad 旋转先验，以及 3 m 的实际三角网欧氏最近距离软约束。它们是工程先验尺度，不是经独立标定的协方差，DSK 也未当作无误差真值。

求解使用点块 Schur 消元、对角尺度和 LM 阻尼。最近三角形自适应搜索具有距离下界检查，覆盖面内、边和顶点。最终目标函数 31,911.573 → 13,746.858；38 次外迭代、60 次试步，最后相对代价下降 7.86e-8，达到 1e-7 的数值停止条件。这不证明梯度驻点或全局最优。

| 诊断 | 中位数 | P90 | 最大值 |
|---|---:|---:|---:|
| 位置修正 m | 5.679 | 9.408 | 21.994 |
| 旋转修正 rad | 0.000525 | 0.004117 | 0.009113 |
| 点到实际网格最近距离 m | 0.339 | 1.800 | 10.837 |
| 点到径向 DSK 交点距离 m | 0.374 | 2.042 | 23.215 |

实际与径向距离均无超过 30 m 的拟合点。拟合点最大位移仍有 80.225 m，反映部分弱交会点的不稳定性，不能由像点误差推导完整三维不确定度。

## 大姿态修正的独立来源检查

18 帧旋转修正超过 0.005 rad，警告原样保留。另行卸载 SPC CK，并逐个隔离实测 HK/AOCS CK，核对精确覆盖区间与矩阵一致性；不凭加载优先级猜测来源。276 帧中 HK/ATTRPT2018 支持 128 帧，AOCS2018 支持 148 帧。18 个警告全部由 HK 支持，而非 AOCS。

警告仅在以光轴滚转为主、实测来源核验成功且滚转差异 ≤0.003 rad 时获得证据解释；未修改平差先验或隐藏警告。7 月 10 日 116 帧的 HK/SPC 滚转差与平差滚转修正相关系数约 0.992，差异中位数/P90 约 0.000390/0.001201 rad。SPC 的影像配准姿态与重建遥测存在差异，该证据不将 HK 认定为绝对真值，也不直接归因为插值错误。

## 轮廓与失败实验

全部 276 帧重算轮廓代理：I/F >0.004 的最大连通体与投影网格顶点凸包比较。平均 IoU 从 0.93985 降为 0.93822，平均对称边界距离从 9.791 增为 10.071 px。此代理受阈值、阴影、凸包近似和形状误差影响；结果已保留，不用于掩盖不利变化。

- `run-v1`：固定初始切平面导致部分点沿切面逃逸，最大实际网格距离约 41.7 m，同时有 18 个未解释姿态警告；未验收。
- `run-v2`：改为真实网格距离，检查精度满足门槛，但耗尽 30 次迭代预算；未验收。
- `run-v3`：保持先验尺度、检查资格和精度门槛，仅使用正确非线性表面距离、增加迭代预算至 100，并逐源解释警告；通过验收。

首次保存产物验证发现轮廓边界均值的 float32 舍入差异，随后增加先失败后通过的数值回归测试。最终像素数与 IoU 严格一致，仅边界距离均值允许最多 4 个 float32 ULP（下限 1e-6 px）；实测最大重放差 9.54e-7 px。该容差未改变科学验收门槛。

## 产物、验证与复现

任务实现位于独立的私有 `hayabusa2-render-adapter` 仓库，本仓库仅保存规划与报告。验收源码提交为 `fcbce8f392aebe75e188f718af2cd91fcd71b9c0`。源码涵盖 `bundle.py`、`bundle_evaluation.py`、`surface_distance.py`、`attitude_audit.py`、`bundle_cli.py`、`validate_bundle.py` 及对应测试。独立代码复核未发现阻塞问题，并以随机三角网穷举核验了最近面搜索与距离梯度。

正式验收文件是 `outputs/ryugu-realism/m04/adjusted-states.json` 和 `geometry-report.json`，内容逐字复制自 run-v3。后者记录输入和实现哈希、逐帧/逐日检查、迭代历史、修正幅度及轮廓。run-v3 还保留冻结协议、拟合点、初始/调整后检查、独立 CK 审计、验证实现快照和验证结果。`renderer-contract/` 包含 276 帧 CSV 与 TSAI：米、ET/TDB 秒、J2000 主动 wxyz；TSAI 使用逆旋转与一基主点 512.5，平差使用零基 511.5。所有转换已重放核验。

| 文件 | SHA-256 |
|---|---|
| adjusted-states.json | `4bf2e2026515ac0d0ea1f6fb24870b1f87716e3726b0718436447a5d07268f03` |
| geometry-report.json | `506a1627579d11f7839c265cc01b3b50988fad51046d689951639ccc5b36d9e4` |
| check-protocol.json | `7bc107c640cb2ee5be05866b65502219a69eb88775d06b429a3675601622b8d0` |

在适配器隔离工作区运行（复现实验须使用新的输出目录）：

```bash
export PYTHONPATH=/home/gengxun/code/hayabusa2-render-adapter-worktrees/m03/src
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
/home/gengxun/miniconda3/envs/asp360_new/bin/python -m unittest discover \
  -s /home/gengxun/code/hayabusa2-render-adapter-worktrees/m03/tests -v
/home/gengxun/miniconda3/envs/asp360_new/bin/python -m hayabusa2_adapter.validate_bundle \
  --run /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m04/run-v3 \
  --network /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m03/tracks.json \
  --audit /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m01/input-audit.json \
  --calibration /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m02/calibration-manifest.json
```

完整求解命令见适配器 README（`bundle_cli --max-iterations 100`）。9 组产物验证包括来源与轨迹隔离、基准/内参、实际表面距离和目标函数、独立检查代数重放、日期/帧覆盖、实测 CK、渲染合约、全部源影像/轮廓及停止条件/原门槛。大型结果不入 Git，由本地输出目录保存，后续清理前须归档。

下一阶段是 M05 全局光度与渲染基线，本轮不实施。

源码发布：[适配器 PR #2](https://github.com/henu-tw2/hayabusa2-render-adapter/pull/2)。两仓库最终合并和同步的机器回执保存为 `outputs/ryugu-realism/m04/publication-receipt.json`。
