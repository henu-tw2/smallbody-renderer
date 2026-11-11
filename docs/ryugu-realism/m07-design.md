# M07 冻结跨日对照与扩展决策

本轮只实现M07，使用已批准总设计与M02四折。M04几何、M05基线、M06相对反照率与全部阈值均冻结；不根据测试误差改变模型、增益、掩膜或划分。工程验收不要求E2胜出。

## 模型和主评价

逐折只评价其test帧；四折test合起来正好覆盖276帧一次。E0/E1读M05保存的float32预测，E2读M06保存预测；源产品、源数据和全部角色哈希核验。先只读重放完整M06验证，包含M05新鲜来源检查；不得改写上游文件。主计算之外不拟合任何系数或光度增益。

science_mask=冻结几何掩膜 & finite(observed I/F) & observed>0；原始码值饱和整帧guard继承M05。common=science_mask & finite(E0)&finite(E1)&finite(E2)，所有三模型在同一common上计算，不以残差过滤。secondary multi=common & M06预存multiday_mask；这是训练支持诊断，不替换主指标。unknown不填充；报告geometry/science/每模型/common/multi覆盖计数，空共同掩膜帧也保留并说明原因。

每帧计算count、mean_abs_observed、MAE、MSE、RMSE、bias（pred-observed），nRMSE=RMSE/mean_abs_observed。每日期先对非空帧MAE/MSE/bias/mean_abs_observed等权平均，RMSE=sqrt(mean frameMSE)，nRMSE=该日期RMSE/该日期等权平均mean_abs_observed。跨日期再次对4日期上述基础量等权聚合（RMSE是dateMSE均值开方；不是所有像元池化，也不是RMSE直接算术平均）。比较百分比改善仅由冻结结果推导，不改变模型选择。四日期不支持强总体显著性结论，不以像元数作为独立统计重复。

## 次要诊断

SSIM只对clip到[0,.045]的显示值，L=.045，K1=.01/K2=.03，3×3采样窗口，局部均值和总体方差/协方差（不作N/(N-1)修正）。只平均3×3全部common有效的窗口中心；无窗口时score=null、window_count=0，不填未知灰色来计算分数。每日期对有效帧分数等权，跨日期对有效日期等权并记录可用数量。它是stride8产品上24原像元尺度的次要诊断，不代表完整分辨率视觉相似性。

i/e固定15°分箱0..90，phase固定1°分箱0..180；在每帧每箱common内计算基础指标，再按日期/帧等权。surface latitude固定[-90,-30,30,90]，同样保留各箱的帧/日期/样点数。无支持箱报告空，不当作零误差。

test图固定I/F显示0..045，残差±.015；每日期选择按帧ID排序的首个common非空帧，不按误差或视觉效果挑选。原坐标点样本中心0,8,..1016，imshow extent[-4,1020,1020,-4]，共享crop[250,800]只用于显示；源科学数组完整128×128、不拉伸/截断。全部test帧另存观测/三预测/三残差/common及multi科学产品，浮点，不修改源预测。

## E3/E4预先决策

E3 Hapke本轮不启用：M01靶心相位17.83–18.65°，M05固定phase=1；缺少宽相位训练覆盖，不能依测试残差自由拟合完整相位/粗糙度参数。
E4 PSF/像元积分本轮不启用：输入合同没有经标定或训练约束的PSF，stride8点样本不能验证原像元或亚像元卷积，不在测试图上调核。未来扩展须新增可追溯标定和训练/验证方案，独立测试重新冻结。
阴影只继承DSK分类为掩膜，没有独立标注，不把DSK预测本身当阴影真值评价。

## 实现与验收

evaluate.py负责独立可测指标/掩膜/SSIM/等权聚合/分箱；evaluate_cli.py负责新鲜来源重放、冻结protocol与扩展决定、逐帧test读取/输出/报表；validate_evaluation.py用独立公式逐像元和完整分层汇总重算、源产品和协议审计；图脚本只读取已完成结果。

先写零误差、非有限/空掩膜拒绝、覆盖和未知、日期与帧不等数量下的等权、SSIM完整窗口、分箱边界测试再实现。正式验收要求4日276帧全覆盖、共同掩膜主指标、固定图/显示、扩展决策有证据、独立数值与隔离审计通过。保存metrics.json/ablation-report.md及来源hash；验收后关闭M07、PR合并同步，M08保持pending。

SSIM公式参考：[Wang等作者页面](https://www.cns.nyu.edu/~lcv/ssim/)，本实验显式采用3×3总体矩的变体，不宣称与其默认11×11高斯实现数值一致。
