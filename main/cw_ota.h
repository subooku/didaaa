// main/cw_ota.h —— 固件空中升级（OTA）。
//
// 固件从 CW 服务器用明文 HTTP 拉，写到另一个 app 槽，收尾由 bootloader 换槽启动。
// 菜单里的 FIRMWARE 一页用它，流程是：查版本 → 二次确认 → 下载 → 重启。
//
// ★ 与音频的关系（这条约束和爆音治理是同一条，别破坏）：
//   写 Flash 会关 cache，那期间音频任务喂不上 PCM，DMA 放空就是一声爆响。
//   所以升级前调用方必须先停音频；好消息是 CONFIG_I2S_ISR_IRAM_SAFE 已开，
//   下载过程即便有声音也不会把波形搞坏，但保险起见还是静默升级。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CW_OTA_IDLE = 0,    // 没在跑（结果可用，见 cw_ota_msg / cw_ota_newver）
    CW_OTA_BUSY,        // 正在查版本或下载
    CW_OTA_OK,          // 完成（下载成功且已设置启动槽，等待重启）
    CW_OTA_ERR,         // 失败（原因见 cw_ota_msg）
} cw_ota_state_t;

// 开机调一次：把当前 app 标记为"跑起来了"，撤掉回滚保护。
// ★ 必须开 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE：新固件首次启动是"待确认"态，
//   确认前重启会自动弹回上一槽 —— 这样变砖的唯一途径是"新固件根本起不来"，
//   而那种情况本来就该回滚。
void cw_ota_init(void);

// 起一个短任务去问服务器"有没有新版"。不阻塞 UI：任务结束前 cw_ota_state() 是 BUSY。
void cw_ota_query_start(void);

// 把状态清回 IDLE（只在没任务跑时生效），用于"重试"：查版本失败后状态停在
// CW_OTA_ERR，不清掉的话页面上的进页查询条件（要求 IDLE）永远不成立，重试按不动。
void cw_ota_reset(void);

// 起升级任务：查版本 → 下载 → 写另一个槽 → 置启动槽。同样不阻塞 UI。
// ★ 调用前必须先停音频（见文件头说明）。成功后由调用方 esp_restart()。
void cw_ota_upgrade_start(void);

cw_ota_state_t cw_ota_state(void);
int            cw_ota_pct(void);          // 0..100，只在 BUSY 的下载阶段有意义
// 一行状态/错误文本。IDLE 时是上次查询的结论（"UP TO DATE" / "NEW: 1.0.1" 等）。
const char    *cw_ota_msg(void);
// 服务器上那份固件的版本号与字节数（查到才有，否则分别是空串和 0）。
const char    *cw_ota_newver(void);
uint32_t       cw_ota_newsize(void);
bool           cw_ota_busy(void);

// "1.0.1" 比 "1.0.0" 新则返回 true。按点分数字逐段比，不做字典序比较
// （否则 "1.0.10" 会被判成比 "1.0.9" 旧）。
bool cw_ota_ver_newer(const char *a, const char *b);
