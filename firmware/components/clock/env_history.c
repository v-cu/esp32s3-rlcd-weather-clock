#include "env_history.h"
#include <string.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_log.h"
#define HISTORY_ATTR RTC_NOINIT_ATTR
static const char *TAG = "EnvHistory";
#else
#define HISTORY_ATTR
#define ESP_LOGI(tag, ...) ((void)0)
#endif

#define HISTORY_MAGIC    0x45485632u   /* "EHV2" */
#define TEMP_MISSING     INT16_MIN
#define U16_MISSING      UINT16_MAX

/* Ring bookkeeping shared by both histories. */
typedef struct {
    int64_t  slot;            /* slot index (epoch / slot_sec) being accumulated */
    uint16_t head;            /* next write position */
    uint16_t count;           /* completed slots stored */
    int32_t  acc_a;           /* sums of the current slot */
    int32_t  acc_b;
    uint16_t acc_n;
    uint16_t reserved;
} Ring;

typedef struct {
    uint32_t magic;
    Ring     env;
    int16_t  temp[ENV_HISTORY_POINTS];
    uint16_t hum[ENV_HISTORY_POINTS];
    Ring     bat;
    uint16_t bat_mv[BAT_HISTORY_POINTS];
    uint32_t checksum;
} HistoryStore;

static HISTORY_ATTR HistoryStore s_store;

static uint32_t calc_checksum(const HistoryStore *h)
{
    const uint8_t *p = (const uint8_t *)h;
    size_t len = offsetof(HistoryStore, checksum);
    uint32_t sum = 0x811C9DC5u;                 /* FNV-1a */
    for (size_t i = 0; i < len; i++) {
        sum ^= p[i];
        sum *= 16777619u;
    }
    return sum;
}

static void seal(void)
{
    s_store.checksum = calc_checksum(&s_store);
}

void EnvHistory_Init(void)
{
    if (s_store.magic == HISTORY_MAGIC &&
        s_store.checksum == calc_checksum(&s_store) &&
        s_store.env.head < ENV_HISTORY_POINTS && s_store.env.count <= ENV_HISTORY_POINTS &&
        s_store.bat.head < BAT_HISTORY_POINTS && s_store.bat.count <= BAT_HISTORY_POINTS) {
        ESP_LOGI(TAG, "Restored history from RTC memory: env %u, battery %u points",
                 (unsigned)s_store.env.count, (unsigned)s_store.bat.count);
        return;
    }
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = HISTORY_MAGIC;
    seal();
    ESP_LOGI(TAG, "History cleared");
}

/* ---- generic ring logic -------------------------------------------------------------------- */

typedef void (*PushFn)(uint16_t pos, bool valid, int32_t a, int32_t b);

static void ring_push(Ring *r, uint16_t size, PushFn fn, bool valid, int32_t a, int32_t b)
{
    fn(r->head, valid, a, b);
    r->head = (uint16_t)((r->head + 1) % size);
    if (r->count < size) r->count++;
}

static void ring_add(Ring *r, uint16_t size, int slot_sec, PushFn fn, time_t now, int32_t a, int32_t b)
{
    int64_t slot = (int64_t)now / slot_sec;

    if (r->slot == 0) {
        r->slot = slot;
    }

    if (slot > r->slot) {
        if (r->acc_n > 0) {
            ring_push(r, size, fn, true, r->acc_a / r->acc_n, r->acc_b / r->acc_n);
        } else {
            ring_push(r, size, fn, false, 0, 0);
        }
        int64_t gap = slot - r->slot - 1;          /* device busy / time jumped forward */
        if (gap > size) gap = size;
        for (int64_t i = 0; i < gap; i++) {
            ring_push(r, size, fn, false, 0, 0);
        }
        r->slot = slot;
        r->acc_a = 0;
        r->acc_b = 0;
        r->acc_n = 0;
    } else if (slot < r->slot) {
        r->slot = slot;                             /* clock moved backwards - stay in slot */
    }

    r->acc_a += a;
    r->acc_b += b;
    r->acc_n++;
}

/* Walks the ring newest -> oldest; 'get' returns false for a missing point. */
typedef bool (*GetFn)(uint16_t pos, int32_t *a, int32_t *b);

static int ring_series(const Ring *r, uint16_t size, GetFn get, int32_t out_a[], int32_t out_b[], int32_t missing)
{
    int valid = 0;
    int out = size - 1;

    if (r->acc_n > 0) {
        out_a[out] = r->acc_a / r->acc_n;
        if (out_b) out_b[out] = r->acc_b / r->acc_n;
        valid++;
    } else {
        out_a[out] = missing;
        if (out_b) out_b[out] = missing;
    }
    out--;

    int idx = (int)r->head;
    for (int n = 0; out >= 0; n++, out--) {
        int32_t a = 0, b = 0;
        bool ok = false;
        if (n < (int)r->count) {
            idx = (idx - 1 + size) % size;
            ok = get((uint16_t)idx, &a, &b);
        }
        out_a[out] = ok ? a : missing;
        if (out_b) out_b[out] = ok ? b : missing;
        if (ok) valid++;
    }
    return valid;
}

/* ---- environment (temperature / humidity) -------------------------------------------------- */

static void env_push(uint16_t pos, bool valid, int32_t t, int32_t h)
{
    s_store.temp[pos] = valid ? (int16_t)t : TEMP_MISSING;
    s_store.hum[pos] = valid ? (uint16_t)h : U16_MISSING;
}

static bool env_get(uint16_t pos, int32_t *t, int32_t *h)
{
    if (s_store.temp[pos] == TEMP_MISSING) return false;
    *t = s_store.temp[pos];
    *h = s_store.hum[pos];
    return true;
}

void EnvHistory_AddSample(time_t now, float temperature_c, float humidity_percent)
{
    float tc = temperature_c * 10.0f;
    float hc = humidity_percent * 10.0f;
    int32_t t10 = (int32_t)(tc >= 0.0f ? tc + 0.5f : tc - 0.5f);
    int32_t h10 = (int32_t)(hc + 0.5f);
    if (h10 < 0) h10 = 0;
    if (h10 > 1000) h10 = 1000;

    ring_add(&s_store.env, ENV_HISTORY_POINTS, ENV_HISTORY_SLOT_SEC, env_push, now, t10, h10);
    seal();
}

int EnvHistory_GetSeries(int32_t temp_x10[], int32_t hum_x10[], int32_t missing)
{
    return ring_series(&s_store.env, ENV_HISTORY_POINTS, env_get, temp_x10, hum_x10, missing);
}

/* ---- battery ------------------------------------------------------------------------------- */

static void bat_push(uint16_t pos, bool valid, int32_t mv, int32_t unused)
{
    (void)unused;
    s_store.bat_mv[pos] = valid ? (uint16_t)mv : U16_MISSING;
}

static bool bat_get(uint16_t pos, int32_t *mv, int32_t *unused)
{
    (void)unused;
    if (s_store.bat_mv[pos] == U16_MISSING) return false;
    *mv = s_store.bat_mv[pos];
    return true;
}

void EnvHistory_AddBattery(time_t now, int battery_mv)
{
    if (battery_mv <= 0 || battery_mv > 6000) return;
    ring_add(&s_store.bat, BAT_HISTORY_POINTS, BAT_HISTORY_SLOT_SEC, bat_push, now, battery_mv, 0);
    seal();
}

int EnvHistory_GetBattery(int32_t mv[], int32_t missing)
{
    return ring_series(&s_store.bat, BAT_HISTORY_POINTS, bat_get, mv, NULL, missing);
}
