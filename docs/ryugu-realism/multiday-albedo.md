# M06：多日相对反照率、覆盖与条件误差

2026-11-11。本轮只完成 M06；M07 尚未启动。正式运行 `outputs/ryugu-realism/m06/run-v2/`，保留 run-v1 的初轮结果和溯源复核记录。M04 几何与 M05 光度模型/掩膜/四折划分均冻结，没有重新优化姿态、形状或逐帧增益。

## 交付与主要发现

四折均对全部 **49,152 个 DSK 面片**建立产品行，仅用该折两个训练日期估计相对反照率。各折中位数均为 1，未观测面片反照率/误差为 NaN、计数和日期掩码为 0。全部 276 帧输出 E2 的 float32 采样预测及观测/多日期支持掩膜，共 1,104 份预测文件；没有计算 M06 验证或测试亮度指标。

85 项单元测试及 10 组独立产物验证零失败、零跳过。完成的含义是反照率产品、来源/覆盖/条件误差和隔离验收成立，尚不是最终跨日渲染改进被证明。M07 才比较 E0/E1/E2 的留出日期性能。

| 折标签（留出日期） | 已观测面片 | 已观测面积 | 多日期多帧面片 | 其面积 | 归一乘子 c | 日期半差中位数 |
|---|---:|---:|---:|---:|---:|---:|
| 2018-06-30 | 42,449 | 87.07% | 14,973 | 32.42% | 1.004524 | 0.0146 |
| 2018-07-03 | 40,675 | 83.80% | 13,264 | 29.02% | 1.049665 | 0.0898 |
| 2018-07-10 | 40,743 | 83.94% | 15,000 | 32.81% | 1.037296 | 0.0871 |
| 2018-07-12 | 42,549 | 87.29% | 16,703 | 36.18% | 1.003290 | 0.0156 |

面积按真实三角形面积加权，数量覆盖约 82.75%–86.57%；面积覆盖约 83.80%–87.29%。多日期多帧标志要求 date_count≥2 且 frame_count≥3，其面积只有 29.02%–36.18%，不等于已经证明可靠。很多面片仅由一个日期支持，不能把全部覆盖称为可靠多日反照率。

包含 6 月 30 日作为训练日期的两折，日期半差中位数约 0.087–0.090；另两折约 0.015。前者与部分相对反照率变化处于同一量级，提示日期相关的标定、模型或几何残差尚不可忽略，不能把所有亮暗变化都解释为稳定表面性质。这是诊断事实，尚不能据此确定单一误差来源。

## 估计与尺度

F 是 M05 验证选中的冻结 LS/混合模型，包含其全局 I/F 尺度。逐训练样点拟合 `y≈F*q_plate`，最小化 `sum base*Huber((F*q−y)/sigma_y)`，标准化 Huber 截断 1.345。每面片用单调目标梯度括根求解，不跨面片平滑，不填补未知。

令 `c=median(q_observed)`，`A=q/c`；输出预测 `E2=c*F*A=F*q`。同时保存 c，避免相对归一后丢失绝对 I/F 尺度。c 只转移尺度，不是新拟合的逐帧/逐日期增益；相对 A 也不是几何反照率或单次散射反照率。

权重从日期等权、日期内帧等权、帧内有效像元等权出发，再除以表面投影像元面积代理 `(range/focal)²/mu`。工程观测尺度为 `sigma_y=sqrt(0.002²+(0.5*gradient_F)²)`，分别对应 I/F 尺度与 0.5 原像元配准尺度。梯度来自冻结 F 的 stride8 有限差分，有效邻居不足使用单侧差分或标记缺失；实际训练样本均可获得两轴差分。

质量面积及误差权重会改变最终日期贡献，所以最终权重不保证日期严格等权。产品显式输出每面片最终日期 alpha 贡献、权重和有效样本量。粗尺度梯度不是高频纹理、阴影或真实轮廓配准误差的完整上界；面积也是工程投影代理，而非真实像元积分。

## 条件误差与支持标志

固定当前稳健权重，令 `alpha_i∝base_i*robust_i*F_i²/sigma_y_i²`，每面片归一为和 1。保存：

- 独立观测条件传播：`sqrt(sum alpha_i²*(sigma_y_i/F_i)²)/c`。
- 日期内完全相关、日期间独立的条件传播：`sqrt(sum_date(sum_i_in_date alpha_i*sigma_y_i/F_i)²)/c`。
- 逐日期单独估计 q 后的半差：`abs(q_date1−q_date2)/(2c)`；仅有一个日期则 NaN。

前两者的各折面片中位数分别约 0.051–0.054 和 0.076–0.088。它们条件于冻结光度模型、形状、几何、工程 sigma、稳健权重及归一 c，不包含这些上游模型或 c 自身的完整不确定度；后者是两日期敏感性。**三者均不是完整统计置信区间**，有效样本量也不表示实际独立重复数。

Huber 极端观测可有平坦最优区间，零梯度不证明唯一性，冻结稳健权重传播也不是目标真实曲率的逆矩阵。独立验证检查实际每个联合/逐日期解的严格 Huber 内点：所有四折均无零内点个案。一般退化情形在测试和报告中保留，不用另一个求根算法的不同最优点误判为失败。

## 日期隔离与上游完整性

当前折估计器只收到训练日期的亮度、掩膜和有效观测。验证/测试在估计阶段只有 frame_id/date，预测阶段只加载其几何，不读取其 I/F。测试把留出亮度与掩膜替换为不可读取对象，训练仍通过；没有测试亮度参与反照率、权重阈值或自由增益。

源完整性核验是另一条只读路径，会读取全部上游源影像，并重放已冻结的 M05 指标；这些值不传入当前折 M06 反照率估计或预测。不能把隔离保证表述成整个审计流程从未读取留出影像。

初轮复核发现 M05 旧 validation 仅直接绑定模型/报告，未绑定缓存清单；仅检查“缓存与清单自报哈希一致”不足以继承科学验收。正式入口只读调用上游完整 verify，重放全部 10 组来源/几何/预测/隔离检查，另存 `upstream-geometry-validation.json` 并绑定当前几何清单哈希、源路径/哈希和模型/报告哈希。M06 manifest 再绑定此 sidecar；M06 独立验证再次只读重放并比较。M05 产物未改写。

run-v1 是溯源修补前的诊断运行；正式 run-v2 用当前发布源码完整重建，数值估计协议未变。独立代码复核确认溯源缺口已关闭，未遗留阻塞问题。

## 产物、复现与限制

正式合同为 `outputs/ryugu-realism/m06/albedo-manifest.json`、`reconstruction-report.json`，逐字复制 run-v2。每折含 `fold.json`、训练 `samples.npz`、全面片 `plates.npz` 与所有帧预测。面片字段含 q/A/observed、样本/帧/日期数、日期掩码与贡献、有效样本量/权重、逐日期 q、三类条件误差、支持标志及站点梯度。`shape.npz` 保存面片 ID、三角面积与体固坐标质心。

`albedo-support-uncertainty.png` 使用训练推导的面片质心经纬度散点展示相对反照率、日期数、日期相关条件尺度；灰色保持未观测，未作图像插值或纹理填充。统一 A 显示范围 0.6–1.4，超范围只在显示中截色，科学数组不截断；各折截色比例低于 0.5%。图与统计脚本随本地输出保存。

复现与独立验证入口：

```bash
export PYTHONPATH=/home/gengxun/code/hayabusa2-render-adapter-worktrees/m03/src
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
/home/gengxun/miniconda3/envs/asp360_new/bin/python -m unittest discover \
  -s /home/gengxun/code/hayabusa2-render-adapter-worktrees/m03/tests -q
/home/gengxun/miniconda3/envs/asp360_new/bin/python -m hayabusa2_adapter.validate_albedo \
  --run /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m06/run-v2 \
  --m05 /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m05/run-v2 \
  --states /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m04/adjusted-states.json \
  --splits /home/gengxun/code/smallbody-renderer/outputs/ryugu-realism/m02/splits.json \
  --dsk /home/gengxun/miniconda3/envs/asp360_new/data/hayabusa2/kernels/dsk/ryugu_shape_spc_49k_v20200323.bds
```

完整求解命令见适配器 README（`albedo_cli`，包含 `--audit`、`--calibration`，要求新输出目录）。独立验证重算全部训练样本、质量权重、每面片最优性/日期来源/尺度/误差代数、全部 1,104 份 E2 预测与掩膜、覆盖及报告。验证不调用估计器，也不强求非唯一极小值由另一算法精确重现。

仍沿用 M05 的 128×128 原像元中心采样，不是全分辨率纹理或像元积分；采样 EDT 不保证真实轮廓16px净距，21 帧原始码值 guard 排除、损失压缩、未校正杂散光/非线性、窄相位域均保留。泛化、残差和改善结论须在下一阶段独立留出日期上检验，本轮不进入 M07。

验收源码提交：`b7f4ee4460fd176da05ec5137fc5c46408f48458`。正式 manifest SHA-256：`5155e1e0b562eda90d8681f077f5e0a633a16c887d43521ee3807f189bd8d039`；report：`c79ecc4eeb11e8704c2d590172df0c5caf8e532bc2253c50fcd5c2f134037b72`。源模块快照与发布源码逐文件一致。

可重复性检查：run-v1 与 run-v2 的四折全部面片数组及归一乘子逐项完全相同，差别仅为新鲜溯源核验与哈希绑定；比较记录随 run-v2 保存。

源码发布：[适配器 PR #4](https://github.com/henu-tw2/hayabusa2-render-adapter/pull/4)。两仓库最终合并及本地同步回执见 `outputs/ryugu-realism/m06/publication-receipt.json`。
