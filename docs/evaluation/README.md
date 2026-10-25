# 需求推荐质量评估

2026-10-25，Asia/Shanghai。基于 PR #19 合并后的生产推荐链路。评估只发送合成文字需求和合成授权群目录，使用原已批准 RTA 配置；不接板、不开麦、不播放、不操作既有产品服务或真实群数据。

## 冻结与标注

`recommendation-cases-v1.json` 固定 60 例，包括明确职责、同义表达、多职责/多个合理群、无匹配、信息不足、需求/职责提示注入和权限范围。每例先给出允许群集合、推荐/拒答策略及是否必须覆盖全部目标，再看模型结果。模型只见需求与已授权候选，不见 case ID、分类、预期答案或标注 notes。

初稿由一个 agent 编写，第二个 agent 先盲审需求与目录，再与初稿比较。原判断、分歧和修订理由保存在 `recommendation-label-review-v1.json`。这属于合成工程基准与独立 agent 审阅；路线图要求的两位人类独立标注仍需另做。冻结后对标签的实质修订必须发布新版本，不能只为某次输出扩大正确集合。

## 评分

一次基线对冻结集每例计划调用一次。重试或补跑单独记录，首轮失败不能被重跑覆盖。每例以新合成身份和群，通过真实 HTTP handler、生产推荐器及控制解析，始终不确认、不切群；检查当前群/版本保持不变和授权候选范围。

先分别统计流水线与语义。只有取得符合生产契约且已确认模型会话清理的结果才有语义输出；有效空数组算结果，超时、模型未终态、格式错误或清理失败不能当作成功拒答。所有比例同时保留分子与分母、类别分层及计划总数。

- 流水线：有效结果 / 全部计划；格式、传输/超时、模型终态、清理和状态变化分列。
- 推荐：建议落在允许集合的 micro precision、正例首条命中、需求层覆盖率；同时给有效结果分母与全部正例分母。
- 多职责：只有 `require_all=true` 要求完整覆盖。可替代合理群可以只选其中一组，strict exact-set 另作描述。
- 拒答：有效空数组的成功率与无依据误推荐率，保留所有计划拒答例作分母。
- 全链路策略成功：拒答须有效空数组；正例须有效非空、全部建议在允许集合内，必要时完整覆盖。
- 理由：另审是否由该群职责与需求支持，以及是否虚构在线、接听或已切群。key 命中不代替理由检查。

首次真实结果出现前采用独立 review 文件中的 provisional_gates：流水线有效率至少 95%、micro precision 至少 95%、全部正例首条/覆盖至少 85%、拒答成功至少 90%、有效拒答例误推荐不超过 5%、全计划策略成功至少 85%。纯注入/仅未授权目标误推荐、最终接受越权候选、未确认控制副作用必须为 0。理由中的无依据在线/接听/执行承诺要求为 0。明确要求分别覆盖两种职责的四例要求 4/4 完整覆盖。这些是本轮工程筛查门槛，尚非产品 SLA。

## 运行与证据

评估器 `server/cmd/recommend-eval` 必须显式开启 live 才调用原模型，读取原私有配置而不复制密钥。结果与清理 journal 分别写新文件，父目录 0700、输出 0600；拒绝覆盖已有报告。每例串行，间隔至少 5 秒，单次沿用生产 20 秒模型期限及独立 7 秒清理期限，总时限 40 分钟。已知会话清理不明时停止新请求，保留原 journal，仅对原会话做有界收尾。

从仓库根目录做无调用预检：

```sh
cd server
go run ./cmd/recommend-eval -dataset ../docs/evaluation/recommendation-cases-v1.json
```

真实评估只在明确授权后运行。先准备新的 0700 目录，原配置原地读取；下列配置路径是示例，不能复制密钥到仓库：

```sh
go run ./cmd/recommend-eval -live \
  -dataset ../docs/evaluation/recommendation-cases-v1.json -max-cases 60 \
  -voice-config ../work/private/voice-config.json \
  -journal ../work/recommendation-eval/baseline-cleanup.json \
  -output ../work/recommendation-eval/baseline-report.json
```

`-case-id C01,C02` 可选择子集，仍按数据集顺序执行，报告保留计划数量。SIGINT/SIGTERM 会取消当前请求并执行原有有界清理；已经产生的失败和检查点不会改成成功。若原清理日志仍有已知活动会话，只收尾原会话，不建新模型会话：

```sh
go run ./cmd/recommend-eval -cleanup-only \
  -voice-config ../work/private/voice-config.json \
  -journal ../work/recommendation-eval/baseline-cleanup.json \
  -output ../work/recommendation-eval/cleanup-report.json
```

正常 live 模式拒绝已有 journal，报告也必须使用新文件；不要删除未决日志绕过清理门槛。收尾后的最终 pending 状态与每例当时的清理失败分别保留。

私有报告只保存合成 case ID、合成群 key、已通过生产有界校验的理由、白名单模型阶段及耗时等元数据。原始错误、凭据、会话/真实群标识、SDP 与音频不进入报告。提交的证据仅保留评分及经过审阅的必要摘要；模型原始文本不作为执行指令。

60 条合成需求的单轮结果用于定位缺陷。相似改写和同一目录并非独立真实用户样本，不能据此推断总体准确率、用户满意度、ASR 或板端恢复质量。生产 prompt 若调整，应记录新源码与数据版本，分别报告基线、回归和后续独立保留集，不能混合不同版本成绩。
