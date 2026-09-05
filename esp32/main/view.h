#ifndef VIEW_H
#define VIEW_H

/* ============ SolarView: 显示层 ============
 * 职责: 消费SolarLink的心跳JSON, 渲染到显示设备
 * 设计: 与显示硬件解耦, JSON是层间语言;
 *       当前实现=LCD(ST7789), 未来加显示器只需新renderer */

/* 初始化显示设备 */
void view_init(void);

/* 渲染心跳面板(传入心跳JSON字符串) */
void view_render_hb(const char *hb_json);

/* 20Hz判定入口(每50ms): 分块脏检查, 无变化零绘制 */
void view_poll(void);

/* 渲染搜星界面(无心跳时) */
void view_render_search(void);

/* 主动查询是否需显示搜星(via core同步状态) */
int view_need_search(void);

#endif
