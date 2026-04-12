# WiFi 升级日志对照验收（cmd/result/reason）

> 目的：按 `WiFi_Upgrade_Min_Test_Checklist.md` 逐条核对结构化日志字段是否符合预期。  
> 日志格式：`[HH:mm:ss.fff] transport=..., cmd=..., seq/offset=..., retry=..., result=..., reason=...`

---

## 0. 预检（所有用例共用）

### 必须字段
- `transport`
- `cmd`
- `seq/offset`
- `retry`
- `result`
- `reason`

### 结果码合法集合
`OK | TIMEOUT | NACK | BUSY | CRC_ERR | LEN_ERR | DISCONNECTED | RETRY_EXHAUSTED`

通过标准：抽样 20 条日志，字段齐全且 `result` 均在集合内。

---

## TC-01 正常升级闭环

### 关键日志序列（应出现）
1. `cmd=LISTEN, result=OK, reason=server-listening`（Server 模式）
2. `cmd=ACCEPT, result=OK, reason=server-accepted`
3. `cmd=CMD_UPDATE_START, result=OK, reason=start`
4. 多条 `cmd=CMD_UPDATE_DATA, result=OK, reason=pkt=...`
5. `cmd=CMD_UPDATE_END, result=OK, reason=end`
6. `cmd=UPGRADE, result=OK, reason=completed`

### 通过判定
- START/DATA/END 全为 `OK`
- 无 `RETRY_EXHAUSTED`
- 最终 `UPGRADE=OK`

---

## TC-02 断线恢复（全量重传）

### 关键日志序列（应出现）
1. 断线时：`cmd=LINK, result=DISCONNECTED, reason=tcp-disconnected`
2. 重连过程：`cmd=RECONNECT, result=OK|DISCONNECTED, reason=...`
3. 恢复后重启：`cmd=CMD_UPDATE_START, result=OK, reason=full-restart`（或同等重启语义）

### 通过判定
- 发生断线后不会卡死
- 在恢复窗口内可重连并重新 START
- 超窗时出现 `RECOVER + RETRY_EXHAUSTED` 并退出会话

---

## TC-03 CRC 错误帧恢复

### 关键日志
- `cmd=RX, result=CRC_ERR, reason=recv=...,calc=...`

### 通过判定
- CRC_ERR 后仍可继续出现后续有效 `CMD_UPDATE_DATA result=OK` 或其它合法响应
- 不出现永久阻塞（无响应且无退出）

---

## TC-04 长度异常帧恢复

### 关键日志
- `cmd=RX, result=LEN_ERR, reason=len=...`

### 通过判定
- LEN_ERR 后仍可继续接收并匹配有效帧
- 升级会话可继续或可控失败退出

---

## TC-05 单包超时重试

### 关键日志
- 同一 `seq/offset` 出现多次 `cmd=CMD_UPDATE_DATA`
- `retry` 递增（0..3）
- 最终：
  - 成功路径：某次 `result=OK`
  - 失败路径：`result=RETRY_EXHAUSTED` 或失败码并退出

### 通过判定
- 重试次数不超过 3
- 间隔符合退避（300/600/1000ms，允许少量调度误差）

---

## TC-06 升级期间禁止手工切通道

### 关键日志
- `cmd=CHANNEL_SWITCH, result=BUSY, reason=upgrade-in-progress`
- 或 `cmd=MODE_SWITCH, result=BUSY, reason=blocked-during-upgrade`

### 通过判定
- 升级期间手工切换操作被拒绝
- 升级会话不中断

---

## TC-07 跨通道会话锁

### 关键日志/现象
- 第一会话升级中，第二会话被拒绝（UI 提示或 BUSY 日志）
- 第一会话结束后可再次发起升级

### 通过判定
- 任意时刻仅一个升级会话 owner
- owner 释放后可正常进入下一会话

---

## 快速人工核对模板（执行时填写）

| 用例 | 是否通过 | 关键证据日志（复制 1~3 行） | 备注 |
|---|---|---|---|
| TC-01 |  |  |  |
| TC-02 |  |  |  |
| TC-03 |  |  |  |
| TC-04 |  |  |  |
| TC-05 |  |  |  |
| TC-06 |  |  |  |
| TC-07 |  |  |  |

---

## 验收结论
- 全部通过：可判定 WiFi Server 主路径升级闭环完成。  
- 任一失败：按失败用例编号回归修复后重测。
