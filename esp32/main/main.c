/* SolarClock 主程序 (胶水层)
 * SolarCore(内核/同步) + SolarLink(JSON协议) + SolarView(显示) */
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "core.h"
#include "view.h"


void app_main(void) {
    /* UART0驱动 (Link层命令收发与JSON输出的前提) */
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);

    core_init();   /* NVS/RTC/GNSS同步状态机/心跳模式决策 */
    view_init();   /* LCD初始化 */
    link_init();   /* UART0命令任务 */

    time_t last_tick = 0;
    for (;;) {
        time_t tn = core_now();
        if (tn != last_tick) {       /* 每秒: 同步状态机调度 */
            last_tick = tn;
            core_tick();
        }
        if (core_is_hb()) {
            CloudHb hb;
            if (core_compute_hb(&hb) == 0) {
                char j[256];
                link_send_hb(&hb, j, sizeof j);  /* 序列化+串口发送(100ms) */
                view_render_hb(j);               /* 喂给view缓存 */
            }
        } else {
            view_render_search();     /* 未进心跳: 搜星界面 */
        }
        view_poll();                  /* 20Hz判定(50ms): lcd分块脏检查 */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
