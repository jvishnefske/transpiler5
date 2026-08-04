/* Minimal FreeRTOS kernel configuration for the transpile corpus, paired with
 * the POSIX (ThirdParty/GCC/Posix) port so the kernel builds on the host. Its
 * only job is to let clang build an AST for the kernel .c files so emitrust-cc
 * can be measured against them; it is not tuned for any real deployment. */
#ifndef FREERTOS_CORPUS_CONFIG_H
#define FREERTOS_CORPUS_CONFIG_H

#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configTICK_RATE_HZ                       1000
#define configMINIMAL_STACK_SIZE                 4096
#define configTOTAL_HEAP_SIZE                    ( 64 * 1024 )
#define configMAX_TASK_NAME_LEN                  16
#define configUSE_16_BIT_TICKS                   0
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                3
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             4096
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configSUPPORT_STATIC_ALLOCATION          1
#define configMAX_PRIORITIES                     7
#define configQUEUE_REGISTRY_SIZE                10
#define configUSE_TASK_NOTIFICATIONS             1
#define configSTACK_DEPTH_TYPE                   uint32_t

#define INCLUDE_vTaskDelay                       1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1

#endif /* FREERTOS_CORPUS_CONFIG_H */
