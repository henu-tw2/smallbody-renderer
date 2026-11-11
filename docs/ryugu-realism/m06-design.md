# M06 多日相对反照率与不确定度设计

沿用用户已批准总设计，只实现M06。M05冻结几何/掩膜/模型/日期折不变，不重拟合光度规律、位姿或逐帧增益。任务源码位于既有隔离适配器；大结果保存在outputs/ryugu-realism/m06/，原始数据与M01–M05只读。

## 数据合同与职责

- albedo.py：观测误差/分辨率权重，分面片稳健估计，尺度转移，条件误差传播、日期敏感性和预测。
- albedo_cli.py：核验M05run-v2、M04states、M02splits与DSK，四折构建训练样本，写全部面片产品和全部帧的E2采样预测。
- validate_albedo.py：独立来源、训练来源/权重、优化KKT、尺度/未知/误差传播/面片来源及预测重放。
- tests/test_albedo.py、test_albedo_pipeline.py、test_validate_albedo.py：先失败再实现，覆盖已知反照率恢复、尺度不变预测、弱支持、坏观测拒绝和测试亮度不可读取。

## 固定估计协议

复用M05原stride8、i/e<75°、采样EDT侵蚀、原始码值整帧guard。仅加载训练I/F；验证/测试向估计器只传frame_id/date元数据，预测阶段也不加载其I/F。测试/验证不参与反照率、误差尺度、自由增益或阈值选择。

F为M05验证选定LS或混合模型的冻结完整I/F预测（含其固定全局尺度）。对每训练像元估计y≈F*q_plate，q>0。sigma_y²=0.002²+(0.5*gradient_F_per_original_px)²。梯度取冻结F的stride8中心差分，边界/无效邻居使用单侧差分，无有效邻居为0并另存标志；它是已解析粗尺度配准敏感性，不是高频纹理/轮廓误差的完整界。

像元表面投影面积area_m2=(range_m/focal_px)²/mu，质量基础权重base=date_frame_pixel_equal_weight/area_m2。总体日期/帧/像元平衡沿用M05，面积和sigma另加质量权重；它们会改变最终日期权重，须输出每面片最终日期贡献，不声称最终严格等权。独立观测目标是sum base*Huber((F*q-y)/sigma_y)，Huber cut=1.345。逐面片用单调梯度括根求解，输出站点残差核验；无跨面片平滑或未知区域填充。

每面片q由训练有效观测确定。c=median(q_observed)，相对A=q/c，输出global_multiplier=c；渲染E2=c*F*A，严格保留F*q的绝对I/F预测。相对A的面片中位数必须为1；不将其称为几何/单次散射反照率。中位归一只在已观测面片上，不按测试可见性选择。

## 不确定度和覆盖

固定稳健权重后的局部线性敏感系数alpha_i∝base_i*robust_i*F_i²/sigma_y_i²，归一和为1。输出q尺度的条件独立误差sqrt(sum alpha_i²*(sigma_y_i/F_i)²)，以及日期内完全相关、日期间独立的保守条件传播sqrt(sum_date (sum_i_in_date alpha_i*sigma_y_i/F_i)²)。二者在A尺度除以c，仅条件于冻结F、形状、几何、工程sigma和归一c；不包含这些上游模型或c本身的不确定度，不是统计置信区间。

另对同一面片每训练日期单独估计q_date；两日期都支持时输出半差abs(q_date1-q_date2)/2/c作为删一日期敏感性，只有一个日期则NaN。日期数最多2，不能据此强总体推断。单日期/单帧仍为observed但标记弱支持；multiday_supported=(date_count>=2 and frame_count>=3)只是支持标志，不等于已证明可靠。

全DSK面片数组包括A、q、valid/observed、sample_count/frame_count/date_count、date_bitmask、有效样本量、质量权重和日期贡献、三类误差量。未知面片A/q/误差均NaN、计数0、日期掩码0，不隐式填充。报告面片数量与三角形面积加权覆盖、各日期来源、全部训练面片空间图。预测记录模型专属/共同可见支持，测试不计算亮度指标。

## 验收与发布

全4折输出，相对中位数1、预测归一等价、未知保持NaN，观测误差/权重来源明确，测试/验证数组不可读取回归通过。独立重放每面片目标梯度、条件传播、日期贡献与敏感性，所有预测/来源hash核验；完成后生成新鲜evidence并manager关闭，PR合并同步。M07保持pending，不以某模型必须胜出作为本阶段完成条件。保留M05采样边界/压缩/杂散光/窄相位限制。

## 复核后的溯源加固与条件误差边界

M05旧保存validation绑定模型与报告但未直接绑定几何缓存清单，不能只接受缓存与自报hash一致。M06入口使用validate_photometry.verify只读重放上游全部10组检查，不改写M05产物；新sidecar绑定本次核验源路径/源hash、几何清单hash、模型/报告hash，manifest绑定sidecar。M06独立验收再只读重放该链。此完整性核验可能读取全部上游影像用于hash/既定指标重放，但不向当前折反照率拟合或预测传递留出亮度。

Huber极端数据可有平坦最优区间，零站点梯度不证明唯一性；冻结稳健权重传播也不是Huber真实曲率逆矩阵。在实际全4折联合/逐日期解中检查严格Huber内点数，未出现无内点的平坦个案。验证检查目标最优性，不强迫与另一求根算法返回完全相同的非唯一解。
