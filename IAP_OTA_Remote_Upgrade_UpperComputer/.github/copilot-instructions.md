# Copilot Instructions

## 项目指南
- User's lwIP networking is polling-based using mx_lwip_process (not RTOS/socket thread driven).
- For this OTA upper-computer project, WiFi upgrade is fixed to TCP and should prioritize Server mode (device actively connects to PC). The default recovery strategy is full restart (ABORT/session invalid + START full retransmission), not breakpoint resume. Use cross-channel single upgrade session lock and structured minimal logs (timestamp, transport, cmd, seq/offset, retry, result, reason).
- After disconnect, perform immediate local session cleanup to Idle and do not block on ABORT send; only send ABORT when the link is still online and the upgrade has failed. Before each full retransmission START, reset local packet index/retry/pending-wait state to initial. Use a global cross-channel upgrade session lock with owner validation on release. Standardize result codes as OK/TIMEOUT/NACK/BUSY/CRC_ERR/LEN_ERR/DISCONNECTED/RETRY_EXHAUSTED.