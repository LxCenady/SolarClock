#ifndef LINK_H
#define LINK_H

#include "core.h"

/* ============ SolarLink: 心跳 JSON 协议 ============
 * 职责: UART0 收发, 命令解析(solar/init/stop/get/nmea/tx1),
 *       心跳JSON序列化(SolarCore快照 -> 协议JSON)
 * 契约: 串口协议纯ASCII JSON, 见 GET 请求/应答格式 */

/* 初始化: 安装UART0驱动, 起命令接收任务 */
void link_init(void);

/* 序列化心跳JSON -> 发送UART0, 并填充 out 供SolarView渲染 */
void link_send_hb(const CloudHb *hb, char *out, int out_n);

#endif
