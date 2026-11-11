# M05：全局光度与渲染基线

日期：2026-11-11。本轮只推进 M05，M06 尚未启动。正式运行目录为 `outputs/ryugu-realism/m05/run-v2/`；run-v1 保留首轮实验与审阅反例，run-v2 使用相同数值协议完整复现当前发布源码。

## 结果与适用范围

全部 276 帧完成固定 M04 位姿下的 DSK 求交、光照分类及线性 I/F 基线。四折均只用两个训练日期拟合、一个验证日期选择模型；测试日期只生成固定预测，不计算亮度误差。比较 E0 均匀 Lambert、E1 未经光度校正亮度纹理、纯 LS 和 LS–Lambert 混合。

四个验证日期均选择混合模型。以下是四模型共同掩膜上的验证 RMSE，单位是无量纲 I/F；先对每帧像元求 MSE，再帧等权取均值、开方。每折只有一个验证日期。测试日期列仅说明拆分，不是测试结果。

| 验证日期 | 对应留出测试日期 | 有效验证帧 | E0 | E1 | LS | 混合 |
|---|---|---:|---:|---:|---:|---:|
| 2018-07-03 | 2018-06-30 | 13 | 0.003659 | 0.004334 | 0.001939 | 0.001669 |
| 2018-07-10 | 2018-07-03 | 127 | 0.004354 | 0.004938 | 0.002951 | 0.002851 |
| 2018-07-12 | 2018-07-10 | 12 | 0.004388 | 0.004969 | 0.003009 | 0.002911 |
| 2018-06-30 | 2018-07-12 | 103 | 0.006341 | 0.007095 | 0.004933 | 0.004840 |

E1 在四个验证日期均差于 E0，未经校正的亮度纹理会混入观测光照。LS 类改善了本范围内的验证预测，但这些日期同时承担模型选择，不能当作最终独立测试证据，也不能据此宣称 E2 多日反照率已经成功。留出日期的正式性能比较留到 M07。

选择 LS/混合时使用二者更完整的统一几何有效掩膜，不要求 E1 已观测面片；因此选模 RMSE 与表中包含 E1 的共同掩膜 RMSE 略不同。四折选模混合 RMSE 依次为 0.001696、0.002866、0.002923、0.004840。

## 几何、采样与科学尺度

使用 M04 的 body 坐标相机中心与主动旋转，将太阳位置从 J2000 逆变换至 body。DSK 千米边界转米，原零基主点 511.5。点到相机和太阳的单位向量与实际三角形外法向计算入射、出射、相位；首交点确定可见性，表面外偏移 0.01 m 的太阳射线检查自遮挡。

固定 stride=8，在原像元中心 0,8,…,1016 求交，输出 **128×128 采样阵列**。这是全部 276 帧的原像元中心实验，不是 1024 全分辨率图像、像元积分、重采样均值或 PSF 模拟。背景保留 NaN；显示拉伸不参与拟合。I/F 已经 SOLDISCR，不再次乘太阳距离平方。既有 C++ UInt16 显示渲染未用于科学拟合；本轮任务实现位于独立适配器仓库。

共追踪 4,521,984 条采样相机射线：3,926,068 背景、573,025 直接受光、9,786 夜侧和 13,105 自遮挡。所有帧仍有几何与预测输出。

## 掩膜与饱和限制

拟合要求直接受光、i/e<75°、影像边界内缩 16 原像元、有限正 I/F，以及采样几何掩膜侵蚀通过。**侵蚀距离是到最近背景采样中心的 EDT，乘 stride 后需 >16；不认证距真实轮廓 >16 原像元。** 独立审阅发现首帧 (576,320) 的缓存距离 17.889 px，却有距其仅 15.524 px 的真实 DSK 背景原像元 (572,305)。已撤回初始设计中过强的真实轮廓净距说法；协议数值、资格和结果未改。精确反例保存在 run-v1，正式运行包含边界解释文件。全分辨率边界与配准稳健性尚须后续核验。

原 L2a 未作畸变校正，而 L2d 已校正，因此不能逐点套用 raw 饱和掩膜。本轮保守要求整帧原始实测最大值低于 DYNRNG−1；缺该字段的 127 帧以 2**BITDEPTH−1 为记录码值上限。21 帧达到码值上限，光度掩膜为空；保留几何和预测。该筛查不是独立探测器饱和标定，无法恢复历史削顶，也不解决损失压缩、非线性和杂散光未校正。

有效均匀模型掩膜累计 441,676 个样点；四折 E1 共同支持分别 434,005、424,159、424,778、434,476。E1 未观测面片为 NaN，不填充并计入可靠覆盖。训练/验证还应用正 I/F 有效性；测试预测掩膜只用几何与既定原始码值质量信息，不用其 I/F 选区域或模型。

## 模型、先验与隔离

- E0：s0*(0.05+0.95*mu0*direct)，保留历史 5% ambient 作为基线。
- E1：s1*Araw[plate]*(0.05+0.95*mu0*direct)。Araw 只由训练有效 I/F 的面片中位数构建，再令面片中位数为 1；它不是光度校正反照率。
- LS：sLS*2mu0/(mu0+mu)，零角归一为 1，阴影/夜侧直接光为 0。
- 混合：a*mu0+b*2mu0/(mu0+mu)，a,b≥0。scale=a+b，LS 权重 b/(a+b)。

四折训练目标均采用日期等权、日期内帧等权、帧内样点等权的 Huber IRLS 非负拟合，Huber 尺度固定 0.002 I/F。相位因子固定为 1；不自由拟合完整相位规律、逐帧增益或偏置，不将经验尺度称为几何反照率。

混合 LS 权重约 0.732–0.751，设计矩阵条件数 9.80–9.95，但参数相关性约 −0.977。两分量高度相关，不能将该权重作精确物理组成解释。相位适用域仍限于当前四日约 18° 的窄范围。6 月 30 日验证残差偏高，不能通过测试逐帧调亮掩盖；可能的标定、形状或光度失配留待后续分析。

`fit_fold` 的测试记录只有 frame_id/date，没有亮度数组。测试隔离测试把测试亮度和 mask 换成不可读取对象，拟合不受影响；改变验证亮度也不改变训练模型和纹理，只能影响选模。每折完全独立，几何已知协议沿用 M02，不冒充无测试影像辅助的姿态预测。

## 验证与复现

65 项单元测试、10 组独立产物检查，均零失败零跳过。检查独立重建公式/掩膜/面片中位数、日期帧权重、Huber 非负 KKT、验证评分和选模、全部浮点预测及训练/验证指标。最大投影 Huber 梯度约 2.50e-11；它验证固定几何下这个凸参数子问题，不意味着几何或完整反射模型全局正确。

全部 276 份原始/L2d、缓存及源哈希均复核，所有命中点的射线方向、角度与实际三角法向重算；另独立重放每帧 8 条 DSK 首交/阴影射线，共 2,208 条，并检查背景反例。该求交重放是确定性采样，不是全部射线二次独立求交。共核验 1,104 份预测文件和 828 个非测试角色的帧指标记录。预测为 float32，无 UInt16 截断；测试没有亮度指标。

`baseline-report.json` 内的 acceptance_passed 仅表示内部拟合数值健全性；最终 M05 完成还要求独立 validator、代码复核及 manager 完成证据全部通过。独立代码复核确认上述采样范围内无阻塞问题。原科学门槛未变，未用正结果作为阶段完成条件。

复现命令见适配器 README：

```bash
export PYTHONPATH=/home/gengxun/code/hayabusa2-render-adapter-worktrees/m03/src
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
/home/gengxun/miniconda3/envs/asp360_new/bin/python -m unittest discover \
  -s /home/gengxun/code/hayabusa2-render-adapter-worktrees/m03/tests -q
/home/gengxun/miniconda3/envs/asp360_new/bin/python -m hayabusa2_adapter.validate_photometry \
  --run /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m05/run-v2 \
  --audit /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m01/input-audit.json \
  --calibration /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m02/calibration-manifest.json \
  --states /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m04/adjusted-states.json \
  --splits /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m02/splits.json
```

正式合同文件为 `outputs/ryugu-realism/m05/models.json` 与 `baseline-report.json`，逐字复制验收运行。运行目录保留 geometry-manifest/protocol、全部几何与预测、构建与拟合实现快照、validation.json、边界解释及验证图脚本。图 `validation-panels.png` 按验证共同覆盖最多帧选取，每日期展示实拍/E0/E1/混合/残差，统一 I/F 0–0.045 与残差 ±0.015；没有按误差挑好看帧，也没有展示测试亮度。大型产物不入 Git，清理前须归档。

公式依据：[USGS ISIS photemplate](https://isis.astrogeology.usgs.gov/8.1.0/Application/presentation/Tabbed/photemplate/photemplate.html)。当前实验不估计完整[龙宫全局相位参数](https://doi.org/10.1051/0004-6361/201937096)。

下一阶段 M06 将在各折仅使用允许日期的前提下估计光度校正相对反照率，输出覆盖、来源和不确定度；本轮不实施。

验收源码提交：`d027c9356c413e276083a8356771a88f4bf32ade`。models.json SHA-256：`2059bf7d467a32475b68d8d9f681ec6701f391f2b580296e4ab5e7f642048213`；baseline-report.json：`0ab8c27288dd16d0826f07a3331e5ee748340631f3f71ccfbb1c6b12e2081f0f`。run-v2 的全部四折参数、纹理和评分与 run-v1 完全一致，比较记录已保存。

源码发布：[适配器 PR #3](https://github.com/henu-tw2/hayabusa2-render-adapter/pull/3)。最终两仓库发布与本地同步回执见 `outputs/ryugu-realism/m05/publication-receipt.json`。
