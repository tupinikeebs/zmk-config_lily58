#pragma once

#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Адрес I2C TPS43 */
#define TPS43_I2C_ADDR              0x74

/* Определения регистров контроллера TPS43 с IQS572 - На основе даташита IQS5xx-B000 */
/* https://www.azoteq.com/images/stories/pdf/iqs5xx-b000_trackpad_datasheet.pdf */

/* События жестов */
// Только чтение
#define TPS43_REG_GESTURE_EVENTS_0  0x000D  /* 1 байт */
#define TPS43_REG_GESTURE_EVENTS_1  0x000E  /* 1 байт */

/* Системная информация */
// Только чтение
#define TPS43_REG_SYSTEM_INFO_0     0x000F  /* 1 байт - Флаги состояния */
#define TPS43_REG_SYSTEM_INFO_1     0x0010  /* 1 байт - Флаги состояния */


/* Управление и конфигурация системы */
// Чтение-запись
#define TPS43_REG_SYSTEM_CONTROL_0  0x0431  /* 1 байт - Основное управление (ACK_RESET, AUTO_ATI) */
#define TPS43_REG_SYSTEM_CONTROL_1  0x0432  /* 1 байт - Управление режимом событий */
// Чтение-запись
#define TPS43_REG_ATI_TARGET        0x056D  /* 2 байта - целевое значение ATI (усиление); выше = чувствительнее (компенсация оверлея/стекла) */
/* Global ATI C multiplier. The OTHER ATI gain knob, and the one Azoteq reaches
 * for first when an overlay leaves a pad under-sensitive (AZD087 5.5, 6.7.1):
 * LOWERING it raises sensitivity, and unlike ati-target it does NOT lengthen the
 * charge time, so it costs no report rate. Their 0.2mm-overlay reference design
 * runs 1 against the bare design's 2. See the Sofle config's keyboard_learnings.md 13.1.1. */
#define TPS43_REG_ATI_C_GLOBAL      0x056B  /* 1 байт - глобальный множитель ATI C */
/* Per-channel ATI compensation, chosen by AUTO_ATI so each channel's count lands
 * near the ATI target. Read-only diagnostic: the datasheet (3.7.4) wants these
 * around 100-150 so automatic Re-ATI has room to move in both directions, and
 * pinning at 0 or 255 is what raises ATI_ERROR. This is the feedback signal for
 * choosing ATI C, hence debug-report-registers. */
#define TPS43_REG_ATI_COMPENSATION  0x043F  /* 150 байт - компенсация ATI по каналам */
#define TPS43_ATI_COMPENSATION_LEN  150
#define TPS43_REG_SYSTEM_CONFIG_0   0x058E  /* 1 байт - Конфигурация системы */
#define TPS43_REG_SYSTEM_CONFIG_1   0x058F  /* 1 байт - Дополнительная конфигурация */

/* Данные касания - основные координаты */
// Только чтение
#define TPS43_REG_NUM_FINGERS       0x0011  /* 1 байт */
#define TPS43_REG_REL_X             0x0012  /* 2 байта - относительное движение по X */
#define TPS43_REG_REL_Y             0x0014  /* 2 байта - относительное движение по Y */
#define TPS43_REG_ABS_X             0x0016  /* 2 байта - абсолютная позиция X */
#define TPS43_REG_ABS_Y             0x0018  /* 2 байта - абсолютная позиция Y */
#define TPS43_REG_TOUCH_STRENGTH    0x001A  /* 2 байта */
#define TPS43_REG_TOUCH_AREA        0x001C  /* 1 байт */

/* Конфигурация XY */
// Чтение-запись
#define TPS43_REG_XY_CONFIG_0       0x0669  /* 1 байт */

/* Конфигурация жестов */
// Чтение-запись // Низкоуровневая конфигурация жестов // наверное стоит убрать
#define TPS43_REG_SINGLE_FINGER_GESTURES 0x06B7  /* 1 байт */
#define TPS43_REG_MULTI_FINGER_GESTURES  0x06B8  /* 1 байт */
#define TPS43_REG_TAP_TIME               0x06B9  /* 2 байта, ms */
#define TPS43_REG_HOLD_TIME              0x06BD  /* 2 байта, ms - press&hold срабатывает через TapTime+HoldTime */
#define TPS43_REG_SCROLL_INIT_DIST       0x06C8  /* 2 байта, px - порог начала скролла (меньше = отзывчивее) */


/* Максимальное число одновременных касаний (см. 5.3). Если сенсор видит больше,
 * ставится флаг TOO_MANY_FINGERS и XY-данные обнуляются. По умолчанию низкое
 * (~2), поэтому для 3-пальцевых жестов его нужно поднять до >=3. */
#define TPS43_REG_MAX_MULTITOUCH    0x066A  /* 1 байт */
#define TPS43_REG_FINGER_SPLIT      0x066B  /* 1 байт - агрессивность разделения пальцев; 0 = не разделять (см. 5.7) */

/* Настройки фильтра */
// Чтение-запись
#define TPS43_REG_FILTER_SETTINGS   0x0632  /* 1 байт */

/* Dynamic XY filter tuning (datasheet 5.9.2.1). Damping factor = beta/256, so a
 * SMALLER beta means STRONGER filtering. The chip interpolates the beta between
 * "bottom beta" (at/below lower-speed) and minimum filtering (at/above
 * upper-speed), speed being pixels travelled per report cycle. Low bottom beta
 * + low speed thresholds = heavily smoothed when creeping, unfiltered as soon
 * as the finger actually moves. Only active while TPS43_IIR_SELECT is clear. */
#define TPS43_REG_XY_DYN_BOTTOM_BETA 0x0637  /* 1 байт */
#define TPS43_REG_XY_DYN_LOWER_SPEED 0x0638  /* 1 байт, px/cycle */
#define TPS43_REG_XY_DYN_UPPER_SPEED 0x0639  /* 2 байта, px/cycle */

/* Global touch multipliers (datasheet 3.5.2). A channel counts as "touched" when
 *     Threshold = Reference x (1 + Multiplier / 128)
 * is exceeded, so a SMALLER multiplier is a MORE sensitive threshold. There are
 * two: SET decides when a channel starts being touched, CLEAR when it stops.
 * Making CLEAR smaller than SET adds hysteresis, which is Azoteq's documented
 * cure for jitter on the touch outputs (AZD087 5.6.1) -- without it, channels at
 * the rim of the contact patch flicker in and out and drag the XY centroid with
 * them. Note the threshold is RELATIVE to the reference, so raising ati-target
 * scales both and does NOT by itself make touch detection more sensitive. */
#define TPS43_REG_TOUCH_MULT_SET    0x0596  /* 1 байт */
#define TPS43_REG_TOUCH_MULT_CLEAR  0x0597  /* 1 байт */

/* Report rate in Active mode (datasheet 4.1), in ms per cycle. Lower = more
 * reports per second = lower latency, at a higher current draw. Azoteq's own
 * reference designs use 10-13 ms. If sensing+processing can't keep up the chip
 * raises TPS43_RR_MISSED. */
#define TPS43_REG_REPORT_RATE_ACTIVE 0x057A  /* 2 байта, ms */

/* Mode timeout - Idle mode (datasheet 4.2): how long the chip stays in Idle
 * (75 ms cycle, ~340 uA) after a finger lifts before stepping down to LP1
 * (~47 uA). The NVM default on this family is 10 s, which on a battery-powered
 * half means it sits at the higher current for the whole gap between strokes
 * during a mousing session. 255 = never step down.
 *
 * Siblings, not used (documented for later): 0x0584 Active timeout [s] (default
 * 5 s of a stationary finger -> Idle-Touch; the learnings flag it as a possible
 * press-and-hold lag cause, so it is deliberately left alone), 0x0585 Idle-Touch
 * timeout [s] (stuck-finger reseed), 0x0587 LP1 timeout in units of 20 s (LP1
 * ~47 uA -> LP2 ~26 uA; left at default because 21 uA is not worth the slower
 * first-touch wake from LP2's 160 ms cycle), 0x057C-0x0583 the per-mode report
 * rates. */
#define TPS43_REG_TIMEOUT_IDLE       0x0586  /* 1 байт, s */


/* ============================================================ */
/* Системная информация 0 (0x000F) - Флаги состояния - 8 бит */
/* ============================================================ */

#define TPS43_SHOW_RESET            BIT(0)      /* Проверка флага при сбросе системы */

/* ============================================================ */
/* Системная информация 1 (0x0010) - Флаги состояния - 8 бит */
/* ============================================================ */

#define TPS43_SWITCH_STATE          BIT(5)      /* Состояние пина SW_IN */
#define TPS43_SNAP_TOGGLE           BIT(4)      /* Переключение канала Snap */
#define TPS43_RR_MISSED             BIT(3)      /* Пропущена частота отчетов */
#define TPS43_TOO_MANY_FINGERS      BIT(2)      /* Превышено максимальное количество точек касания */
#define TPS43_PALM_DETECT           BIT(1)      /* Обнаружена ладонь */
#define TPS43_TP_MOVEMENT           BIT(0)      /* Движение тачпада */


/* ============================================================ */
/* Управление системой 0 (0x0431) - 8 бит */
/* ============================================================ */

#define TPS43_ACK_RESET             BIT(7)      /* Подтверждение сброса */
#define TPS43_AUTO_ATI              BIT(5)      /* Запуск алгоритма ATI (перетюнинг усиления) */

/* ============================================================ */
/* Управление системой 1 (0x0432) - 8 бит */
/* ============================================================ */

#define TPS43_RESET                 BIT(1)
#define TPS43_SUSPEND               BIT(0)      /* Режим сна */


/* ============================================================ */
/* Конфигурация системы 0 (0x058E) - 8 бит */
/* ============================================================ */

#define TPS43_SETUP_COMPLETE        BIT(6)      /* Флаг завершения настройки */
#define TPS43_WDT_ENABLE            BIT(5)      /* Включение таймера сторожевого пса */


/* ============================================================ */
/* Конфигурация системы 1 (0x058F) - 8 бит */
/* ============================================================ */

#define TPS43_EVENT_MODE            BIT(0)
#define TPS43_GESTURE_EVENT         BIT(1)
#define TPS43_TP_EVENT              BIT(2)
#define TPS43_TOUCH_EVENT           BIT(6)


/* ============================================================ */
/* Конфигурация XY 0 (0x0669) - 8 бит */
/* ============================================================ */

#define TPS43_FLIP_X                BIT(0)      /* Отразить ось X */
#define TPS43_FLIP_Y                BIT(1)      /* Отразить ось Y */
#define TPS43_SWITCH_XY_AXIS        BIT(2)      /* Поменять местами оси X и Y */


/* ============================================================ */
/* События жестов 0 (0x000D) - 8 бит */
/* ============================================================ */

#define TPS43_SINGLE_TAP            BIT(0)      /* Обнаружено касание одним пальцем */
#define TPS43_PRESS_AND_HOLD        BIT(1)      /* Обнаружено нажатие и удержание */
#define TPS43_SWIPE_UP              BIT(2)      /* Проведение одним пальцем вверх */
#define TPS43_SWIPE_DOWN            BIT(3)      /* Проведение одним пальцем вниз */
#define TPS43_SWIPE_LEFT            BIT(4)      /* Проведение одним пальцем влево */
#define TPS43_SWIPE_RIGHT           BIT(5)      /* Проведение одним пальцем вправо */

/* ============================================================ */
/* События жестов 1 (0x000E) - 8 бит */
/* ============================================================ */

#define TPS43_TWO_FINGER_TAP        BIT(0)      /* Касание двумя пальцами */
#define TPS43_SCROLL                BIT(1)      /* Прокрутка вверх */
#define TPS43_ZOOM                  BIT(2)      /* Жест масштабирования */

/* ============================================================ */
/* Настройки фильтра 0 (0x0632) - 8 бит */
/* ============================================================ */

#define TPS43_IIR_FILTER           BIT(0)      /* Фильтр IIR */
#define TPS43_MAV_FILTER           BIT(1)      /* Фильтр MAV */
#define TPS43_IIR_SELECT           BIT(2)      /* Выбор IIR */
#define TPS43_ALP_COUNT_FILTER     BIT(3)      /* Фильтр подсчета ALP */


/* Окончание окна связи - ОБЯЗАТЕЛЬНО вызывать после каждого чтения (запись в недопустимый адрес 0xEEEE вызывает NACK) */
#define TPS43_REG_END_COMM_WINDOW   0xEEEE

struct tps43_config {
    struct i2c_dt_spec i2c_bus;
    struct gpio_dt_spec rdy_gpio;
    struct gpio_dt_spec rst_gpio;

    bool single_tap;
    bool press_and_hold;
    /* Press-and-hold trigger delay = TapTime + hold_time_ms (datasheet 6.2).
     * Lower hold_time_ms = drag engages sooner. Written to reg 0x06BD when
     * press_and_hold is enabled. */
    int16_t hold_time_ms;
    bool two_finger_tap;
    bool scroll;
    bool swipes;            
    /* Pinch-to-zoom: enables the chip's ZOOM gesture (reg 0x06B8 bit 2) and
     * turns finger separation into zoom-in/zoom-out button pulses, which a
     * behaviors input processor on the central maps to Ctrl+= / Ctrl+-. The chip
     * only calls it a zoom once the separation changes by zoom-initial-distance,
     * so a parallel two-finger drag still scrolls. */
    bool zoom;
    /* Separation change (px) per emitted zoom step -- the "detent" size. The
     * chip reports zoom continuously, so without this one pinch would fire
     * dozens of Ctrl+= presses. */
    int16_t zoom_step_px;
    /* Swap zoom in/out, for when the separation delta's sign is the other way
     * round than expected on this module. */
    bool invert_zoom;
    bool invert_x;
    bool invert_y;
    bool switch_xy;
    bool invert_scroll_x;
    bool invert_scroll_y;

    /* Tap-then-hold drag ("double-tap-drag"): after a single tap, a fresh touch
     * that lands within tap_drag_window_ms grabs (holds the left button) and
     * drags until all fingers lift. Faster/more reliable than the chip's native
     * press-and-hold, which requires holding a finger still for the hold timer. */
    bool tap_drag;
    int16_t tap_drag_window_ms;

    /* Three-finger middle-button drag: while >=3 fingers touch, hold the middle
     * mouse button and move the cursor (a quick 3-finger tap = middle click; a
     * 3-finger hold+move = middle-drag, e.g. rotate/pan in a 3D slicer). */
    bool three_finger_drag;
    /* Max simultaneous touches written to reg 0x066A. 0 = leave chip default.
     * Forced to >=3 when three_finger_drag is enabled. */
    uint8_t max_multitouches;

    int16_t sensitivity;
    /* Cursor acceleration. The gain ramps linearly from `sensitivity` at
     * accel_lower_speed raw counts/report up to accel_max at accel_upper_speed.
     * accel_max <= sensitivity = off (flat `sensitivity`, the old behaviour).
     * Cursor only: scroll still sees the flat `sensitivity`. */
    int16_t accel_max;
    int16_t accel_lower_speed;
    int16_t accel_upper_speed;
    int16_t scroll_sensitivity;
    /* Wheel ticks emitted per scrolled LINE. 1 = whole lines (HID's classic
     * line wheel). >1 needs CONFIG_ZMK_POINTING_SMOOTH_SCROLLING on the central,
     * which adds the HID Resolution Multiplier feature the host uses to ask for
     * sub-line counts; 16 matches the descriptor's physical maximum, i.e. the
     * finest the host will ever request. See the Sofle config's keyboard_learnings.md P3.1. */
    int16_t scroll_steps_per_line;
    /* Momentum ("coast") scrolling: after a two-finger flick, keep emitting
     * wheel ticks at a decaying rate instead of stopping dead on the lift.
     * Pointless below ~8 steps per line, where each tick is a visible jump. */
    bool scroll_momentum;
    /* Percent of the velocity kept per momentum step (lower = stops sooner). */
    int16_t scroll_momentum_decay;
    /* Milliseconds between momentum steps. */
    int16_t scroll_momentum_interval_ms;
    /* Minimum ticks on the final scroll report for a flick to coast at all, so
     * a slow deliberate scroll ends exactly where the finger left it. */
    int16_t scroll_momentum_min_ticks;
    /* Hard cap on one coast, so a bad velocity estimate cannot scroll forever. */
    int16_t scroll_momentum_max_ms;
    /* Distance (px) the two fingers must travel before scroll starts (reg
     * 0x06C8). Lower = scroll engages on slighter movement. 0 = chip default. */
    int16_t scroll_init_distance;
    /* ATI target (reg 0x056D): touch-detection gain the chip auto-tunes to.
     * Higher = more sensitive, compensates for a glass/overlay. 0 = chip default.
     * Writing it re-runs AUTO_ATI so the new gain takes effect. */
    int16_t ati_target;
    /* Global ATI C (reg 0x056B): the gain knob that costs nothing. LOWER = more
     * sensitive, without the slower response rate that a higher ati_target buys
     * it with. The right answer for a thick overlay, but it must be picked from
     * the resulting ATI compensation (aim ~100-150) rather than guessed, so read
     * the factory value with debug_report_registers first. -1 = chip default. */
    int16_t ati_c;
    /* Diagnostic: after configuring, read this pad's touch SET/CLEAR multipliers,
     * Global ATI C and the ATI compensation spread back off the chip and log
     * them. The factory NVM values are published nowhere and Azoteq's tuning
     * procedure is measure-then-choose, so this is how the numbers are obtained. */
    bool debug_report_registers;
    /* Finger-split aggression (reg 0x066B, datasheet 5.7): how aggressively the
     * chip splits a merged touch polygon into multiple fingers. 0 = never split
     * (one finger stays one contact -> preserves single-finger gestures even
     * with max-multitouches>=3). -1 = leave chip default. */
    int16_t finger_split;

    bool enable_power_management;

    uint8_t filter_settings;

    /* Dynamic XY filter shaping (regs 0x0637/0x0638/0x0639). -1 = leave the chip
     * default. See the register comments above for the beta/speed semantics. */
    int16_t dyn_bottom_beta;
    int16_t dyn_lower_speed;
    int16_t dyn_upper_speed;

    /* Active-mode report rate in ms (reg 0x057A). 0 = leave the chip default
     * (13 ms on this family). */
    int16_t report_rate_active_ms;

    /* Global touch multipliers (regs 0x0596 set / 0x0597 clear). -1 = leave the
     * chip default. touch_hysteresis is the safer relative form: the driver
     * reads the SET value off the chip and writes CLEAR = SET - hysteresis, so
     * stability can be added without having to know this pad's NVM defaults. */
    int16_t touch_set_multiplier;
    int16_t touch_clear_multiplier;
    int16_t touch_hysteresis;

    /* Idle-mode timeout in seconds (reg 0x0586). 0 = leave the chip default
     * (10 s). Purely a battery knob: shorter = the chip drops from Idle
     * (~340 uA) to LP1 (~47 uA) sooner after a lift. */
    int16_t timeout_idle_s;
};

struct tps43_drv_data {
    const struct device *dev;
    struct k_sem lock;
    struct gpio_callback rdy_cb;
    struct k_work work;

    bool device_ready;
    bool initialized;
    bool scroll_active;
    bool drag_active;
    bool suspended;         

    /* Tap-then-hold drag state */
    int64_t last_tap_time;      /* uptime (ms) of the last single tap; 0 = none */
    uint8_t prev_num_fingers;   /* finger count from the previous work cycle */

    /* Three-finger middle-drag state: true while the middle button is held. */
    bool three_drag_active;

    /* Cursor accumulators: carry the sub-count remainder of rel*sensitivity/100
     * between events instead of truncating it away each report. Reset when the
     * touch ends or scroll takes over. */
    int32_t move_accum_x;
    int32_t move_accum_y;
    /* Smoothed finger speed for the acceleration curve, raw counts/report x16.
     * Averaged so one noisy report can't flick the gain up and down (which would
     * read as uneven motion). Reset with the move accumulators. */
    int32_t accel_speed_x16;

    /* Smooth-scroll accumulators: carry the fractional wheel remainder between
     * events (value is rel*scroll_sensitivity, /100 to whole lines) so low
     * sensitivities scroll evenly instead of in bursts. Reset when not scrolling. */
    int32_t scroll_accum_x;
    int32_t scroll_accum_y;

    /* 3-finger swipe debounce: true after a swipe fired, until fingers lift
     * below 3 (so one swipe = one back/forward, not one per cycle). */
    bool swipe_fired;

    /* 3-finger TAP -> middle click. tf_active: currently in a >=3-finger touch;
     * tf_start: uptime (ms) it began; tf_moved: significant movement happened
     * during it (=> a swipe/drag, not a tap). A quick touch-and-lift with no
     * movement emits a middle click; movement suppresses it. */
    bool tf_active;
    int64_t tf_start;
    bool tf_moved;

    /* Momentum scroll state. mom_velocity is in ticks-per-step x1000 so the
     * decay percentage still has resolution once it drops below one tick.
     * mom_last_ticks is the tick count from the most recent scroll report, which
     * is the flick speed we coast from. */
    struct k_work_delayable momentum_work;
    int32_t mom_velocity;
    int32_t mom_remainder;
    int16_t mom_last_ticks;
    bool mom_last_horizontal;
    bool mom_active;
    int64_t mom_start_ms;

    /* Pinch-zoom: accumulated finger-separation delta, spent one zoom step at a
     * time (see zoom_step_px). */
    int32_t zoom_accum;
};

int tps43_set_sleep(const struct device *dev, bool sleep);

#ifdef __cplusplus
}
#endif

