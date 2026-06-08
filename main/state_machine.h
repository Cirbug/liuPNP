/*
 * state_machine.h — 贴片机状态机
 *
 * 状态流转:
 *   IDLE → HOMING → IDLE
 *   IDLE → CALIBRATE → IDLE
 *   IDLE → PICK → BOTTOM_ALIGN → PLACE → PICK (循环) → DONE → IDLE
 *   任何状态 → ERROR → IDLE
 */

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

typedef enum {
    STATE_IDLE,
    STATE_HOMING,
    STATE_CALIBRATE,
    STATE_PICK,
    STATE_BOTTOM_ALIGN,
    STATE_PLACE,
    STATE_DONE,
    STATE_ERROR
} pnp_state_t;

/* 初始化: 加载标定参数 + 坐标文件 */
void state_machine_init(void);

/* 触发状态转移 */
void state_machine_trigger(pnp_state_t target);

/* 执行一步 (被 FreeRTOS 任务周期性调用) */
void state_machine_run(void);

/* 获取当前状态 */
pnp_state_t state_machine_get(void);

/* 状态名称 */
const char* state_name(pnp_state_t s);

#endif
