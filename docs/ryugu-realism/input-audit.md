# M01：龙宫输入与环境审计

日期：2026-11-06。状态：只读输入审计已完成；科学实验尚未完成，适配器基线尚未就绪。

## 实际输入

- 原始目录：`/home/gengxun/miniconda3/envs/asp360_new/data/hayabusa2`，只读，未修改。
- FITS 共276帧，全部1024×1024，全部可解析IMAGE扩展；主HDU无图像。读取器仅覆盖本归档的primary/IMAGE布局，不宣称完整FITS标准/校验和认证。
- SHA256SUMS的276条全部匹配当前实文件。每帧哈希、完整头与统计已保存。
- 日期：2018-06-30（124帧）、2018-07-03（13帧）、2018-07-10（127帧）、2018-07-12（12帧）。全部ONC-T 550nm。
- 276/276帧在FITS DATE-OBS曝光中点可查询SPICE位置/姿态。DATE-OBS与DATE-BEG/END平均之差不超过约0.000501秒。
- 相比文件名整秒时间，中点偏移范围0.217–0.971秒；后续必须使用头中的中点。
- 相机距靶心19497.63–20222.17米。
- 靶心近似相位角17.83248–18.64614度；不是逐像元角度，后者需表面求交计算。
- 元核实际加载30个文件，路径与SHA-256全部记录；合计约532 MB。使用`hyb2_onc_spc_v02.tm`，DSK为49k版本。

## 辐射与适用性

- 全部L2a；149帧BUNIT=DN，127帧BUNIT=COUNTS。不能未经元数据映射混用。
- 2018-06-30共124帧实际8 bit且LOSSY；其余152帧10 bit且LOSSLESS。存储BITPIX不能代表有效位深。量化/压缩限制必须进入不确定度与结果解释。
- 全部影像包含负像元；没有统一BLANK/SATURATE键。负数不能自动当零，也不能未经规则将其当科学目标信号。
- 全部头标记bias与smear已ONBOARD处理，不能重复减去；dark/非线性/flat/radiance尚未应用。
- 本地hyb2onccal说明明确只输出DN，不输出radiance/I/F；校准参数表还含拖影常数来源疑问。仅运行该工具不能满足M02。
- 当前相位覆盖不足以自由拟合完整相位函数或Hapke参数。现有数据可支持固定/文献约束相位规律下的局部反射和跨日一致性实验；大相位重照明泛化需额外观测，不能从这四天直接宣称。

## 新鲜基线结果

| 检查 | Passed | Failed/Error | Skipped | Exit |
|---|---:|---:|---:|---:|
| 渲染器CTest | 11 | 0 | 0 | 0 |
| 渲染器Python合约pytest | 1 | 0 | 0 | 0 |
| 适配器unittest | 3 | 1 error | 0 | 1 |

适配器失败：`write_onct_tsai_null`不接收测试传入的`camera_center_j2000_m`/`camera_to_j2000_wxyz`。现有测试还以45度四元数期望90度逆旋转矩阵，不能照着错误期望实现。原代码保持未改，先在隔离工作区定义相机合约再修复。此错误不妨碍只读输入审计，但阻止宣称适配器已可直接生产精化相机。

完成证据中的`baseline-tests`专指对以上运行记录、退出码和日志完整性的审计检查，不是说原始适配器测试通过；原始失败始终保留在JSON和本表。

## Git与工具现场

- 当前仓库main；既存`docs/superpowers/`未跟踪文档保留。
- 任务适配器存在已修改camera/test及未跟踪cli/contract/tests，初始差异保存于结果目录，不覆盖、不提交。
- asp360_new Python有NumPy/SciPy/OpenCV/spiceypy/pytest，无astropy；FITS审计用受限读取器且对归档SHA256SUMS交叉核验。
- ISIS导入/标定程序存在，但调用需显式环境；本次未运行会改变原始影像的命令。

## 产物与复现

- 机器清单：`outputs/ryugu-realism/m01/input-audit.json`。
- 只读脚本：`outputs/ryugu-realism/m01/audit_inputs.py`。
- 验证脚本与结果：`outputs/ryugu-realism/m01/validate_audit.py`、`audit-validation.json`。
- 原始测试日志：同目录`renderer-ctest.txt`、`renderer-python.txt`、`adapter-unittest.txt`。
- 脚本命令：`/home/gengxun/miniconda3/envs/asp360_new/bin/python outputs/ryugu-realism/m01/audit_inputs.py`；随后运行同目录`validate_audit.py`。

## 后续顺序

M02优先核实官方同帧L2d与PDS标签、冻结日期划分；同时把适配器接口修复作为进入真实几何生产前的明确前置条件。仍未完成连接点、平差、反照率或真实渲染对照。
