#include "eth_tcp_server.h"

/*
 * 这个文件实现的是：裸机 + LwIP RAW API 的 TCP Server“通道层”
 *
 * 你可以把它理解成：
 * - 负责 listen / accept / recv / send
 * - 收到 TCP 字节后通过回调 on_bytes 交给上层
 * - 不负责协议拆帧（TCP 粘包/拆包应该在上层做）
 *
 * 为什么 RAW API 适合裸机：
 * - 不依赖线程/阻塞 socket
 * - 主要由 LwIP 在你调用 MX_LWIP_Process() 时驱动回调
 */

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"

/* 监听 PCB（listen socket 的控制块） */
static struct tcp_pcb *g_listen_pcb = 0;
/* 当前客户端连接 PCB（我们先只支持单连接，简单稳定） */
static struct tcp_pcb *g_client_pcb = 0;
/* 上层回调：收到任意字节流就会回调 */
static EthTcpOnBytes_t g_on_bytes = 0;

/* 关闭当前客户端连接（释放回调绑定并 close） */
static void EthTcp_CloseClient(void)
{
    if (g_client_pcb != 0) {
        tcp_arg(g_client_pcb, 0);
        tcp_recv(g_client_pcb, 0);
        tcp_err(g_client_pcb, 0);
        tcp_poll(g_client_pcb, 0, 0);
        (void)tcp_close(g_client_pcb);
        g_client_pcb = 0;
    }
}

/*
 * recv 回调：LwIP 收到数据后会调用这里
 *
 * 参数说明：
 * - p == NULL：对端关闭连接（FIN）
 * - err != ERR_OK：发生错误
 * - p != NULL：收到数据（可能是链表 pbuf）
 *
 * 注意：
 * TCP 是字节流：
 * - 一次回调可能给你半帧、也可能给你多帧拼在一起
 * - 所以这里不要假设“一个 pbuf 就是一条命令”
 */
static err_t EthTcp_OnRecv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;

    /* 错误处理：释放 pbuf 并关连接 */
    if (err != ERR_OK) {
        if (p != 0) {
            pbuf_free(p);
        }
        EthTcp_CloseClient();
        return ERR_OK;
    }

    /* p==NULL：对端主动关闭 */
    if (p == 0) {
        EthTcp_CloseClient();
        return ERR_OK;
    }

    /*
     * 通知 LwIP：我们已经“消费”了 tot_len 字节
     * 否则 LwIP 认为窗口没腾出来，可能影响后续接收
     */
    tcp_recved(tpcb, p->tot_len);

    /*
     * pbuf 可能是链表：逐段取 payload
     * - 如果 g_on_bytes != NULL：交给上层处理（M3 之后）
     * - 如果 g_on_bytes == NULL：默认回显（用于 M2 验收通道）
     */
    for (struct pbuf *q = p; q != 0; q = q->next) {
        if (q->len > 0 && q->payload != 0) {
            if (g_on_bytes != 0) {
                g_on_bytes((const uint8_t *)q->payload, (uint16_t)q->len);
            } else {
                (void)tcp_write(tpcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
            }
        }
    }

    /* 释放 pbuf（必须做，否则内存池会被耗尽） */
    pbuf_free(p);

    /* 把 write 的数据推送出去 */
    (void)tcp_output(tpcb);

    return ERR_OK;
}

/* err 回调：连接异常中断时触发（对端复位等） */
static void EthTcp_OnErr(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    g_client_pcb = 0;
}

/*
 * poll 回调：LwIP 周期调用
 * 这里先不做事情；后续如果要做“发送队列/超时断开”等可放这里
 */
static err_t EthTcp_OnPoll(void *arg, struct tcp_pcb *tpcb)
{
    (void)arg;
    (void)tpcb;
    return ERR_OK;
}

/*
 * accept 回调：有新连接进来时触发
 * 我们做“单连接策略”：
 * - 如果已有连接：直接 abort 新连接
 * - 否则保存为 g_client_pcb 并绑定 recv/err/poll 回调
 */
static err_t EthTcp_OnAccept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;

    if (err != ERR_OK || newpcb == 0) {
        return ERR_VAL;
    }

    if (g_client_pcb != 0) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    g_client_pcb = newpcb;

    tcp_arg(g_client_pcb, 0);
    tcp_recv(g_client_pcb, EthTcp_OnRecv);
    tcp_err(g_client_pcb, EthTcp_OnErr);
    tcp_poll(g_client_pcb, EthTcp_OnPoll, 2);

    return ERR_OK;
}

void EthTcpServer_Init(uint16_t listen_port, EthTcpOnBytes_t on_bytes)
{
    g_on_bytes = on_bytes;

    EthTcpServer_DeInit();

    g_listen_pcb = tcp_new();
    if (g_listen_pcb == 0) {
        return;
    }

    if (tcp_bind(g_listen_pcb, IP_ADDR_ANY, listen_port) != ERR_OK) {
        (void)tcp_close(g_listen_pcb);
        g_listen_pcb = 0;
        return;
    }

    g_listen_pcb = tcp_listen(g_listen_pcb);
    tcp_accept(g_listen_pcb, EthTcp_OnAccept);
}

void EthTcpServer_DeInit(void)
{
    EthTcp_CloseClient();

    if (g_listen_pcb != 0) {
        tcp_accept(g_listen_pcb, 0);
        (void)tcp_close(g_listen_pcb);
        g_listen_pcb = 0;
    }
}

void EthTcpServer_Poll(void)
{
}

uint8_t EthTcpServer_IsConnected(void)
{
    return (g_client_pcb != 0) ? 1U : 0U;
}

uint8_t EthTcpServer_Send(const uint8_t *data, uint16_t len)
{
    if (g_client_pcb == 0 || data == 0 || len == 0) {
        return 1U;
    }

    if (tcp_write(g_client_pcb, data, len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return 1U;
    }

    (void)tcp_output(g_client_pcb);
    return 0U;
}
