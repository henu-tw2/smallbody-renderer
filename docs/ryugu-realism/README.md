# 龙宫多日反照率渲染实验

本目录保存用户要求的Markdown规划与报告。实际阶段状态以[里程碑索引](../../.planning/milestone-index.md)和机器注册表为准。

## 文档入口

- [实验设计](design.md)：目标、科学假设、输入约定、几何/光度路线、数据隔离、评价与完成定义。
- [实施计划](implementation-plan.md)：8阶段任务、职责边界、输入/输出合同与验收工作项。
- [M01输入审计](input-audit.md)：276帧实测事实、工具/基线结果与已发现问题。
- [M02标定与划分](calibration-split.md)：官方I/F全量验收、四折协议与坐标/辐射限制。
- [M03连接点网](control-network.md)：真实多日网验收、独立检查点、误差与复现入口。
- [初始化清单](milestones.manifest.json)：里程碑初始化来源，不替代生命周期注册表。

## 里程碑路线

M01输入审计 → M02标定与日期划分 → M03连接点网 → M04约束束平差 → M05光度基线 → M06多日反照率 → M07跨日对照 → M08复现交付。

每阶段都有task_plan.md、findings.md、progress.md，位于`.planning/<milestone-id>/`。唯一活动指针为`.planning/.active_plan`。未通过真实数据验收不能勾选完成；原始测试报错必须保留。用户已授权持续推进，常规阶段切换无需重新询问。

## 恢复执行

```bash
python /home/gengxun/.codex/skills/milestone-session-manager/scripts/verify_milestones.py \
  --repo /home/gengxun/code/smallbody-renderer
cat /home/gengxun/code/smallbody-renderer/.planning/.active_plan
```

读取该阶段的三个规划文件与总设计，核对实际工作树/分支、Git差异、依赖证据和产物哈希后，继续唯一Next Step。阶段切换与关闭用manager脚本，禁止手改注册表/生成索引。

## 产物保存

分析脚本、真实影像衍生产品与完成证据位于`outputs/ryugu-realism/`，由现有.gitignore排除。这些文件虽不入Git，仍是科学结果与生命周期校验的必要依赖；未归档前不可清理或改写已验收阶段的产物。原始影像和SPICE保留在既有数据目录，只读。

当前仓库有此前未跟踪文档，任务适配器有既存未提交代码修改；本轮规划和只读输入处理保留这些现场，不自动提交或覆盖。

2026-11-09：用户要求一次只推进一个里程碑，完成后PR并同步本地。本次只完成M03；下游代码提交与当前仓库文档PR分别记录，缺少下游remote时不能宣称源码PR已完成。

M03源码发布已补齐：[私有适配器仓库](https://github.com/henu-tw2/hayabusa2-render-adapter)、[源码PR #1](https://github.com/henu-tw2/hayabusa2-render-adapter/pull/1)。PR已合并，隔离工作区的本地main已同步；原工作区未提交改动保留。M04尚未启动。
