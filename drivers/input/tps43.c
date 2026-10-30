#include <stdint.h>
#define DT_DRV_COMPAT azoteq_tps43

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>

#include "tps43.h"

LOG_MODULE_REGISTER(tps43, CONFIG_INPUT_LOG_LEVEL);

/* True while >=1 finger is on the pad: set on any touch event, cleared on the
 * lift event, and it stays set through a still rest (no event needed to hold it). */
static bool tps43_finger_down = false;

/**
 * @brief Завершает окно связи с тачпадом
 * 
 * После каждого чтения регистров тачпада необходимо завершить окно связи,
 * записав специальный адрес 0xEEEE, что вызывает NACK от устройства.
 * Это обязательный шаг согласно протоколу IQS5xx.
 * 
 * @param dev Указатель на устройство тачпада
 */
static void tps43_end_communication_window(const struct device *dev) {
    const struct tps43_config *config = dev->config;
    uint8_t end_buf[2];

    sys_put_be16(TPS43_REG_END_COMM_WINDOW, end_buf);

    int ret = i2c_write_dt(&config->i2c_bus, end_buf, sizeof(end_buf));
    if (ret != 0 && ret != -EIO) {
        LOG_INF("Запись окончания окна связи вернула: %d (ожидается NACK)", ret);
    }
}

/**
 * @brief Читает последовательность регистров тачпада
 * 
 * Выполняет чтение нескольких байт из последовательных регистров тачпада,
 * начиная с указанного адреса. Используется для чтения связанных регистров,
 * таких как события жестов (GESTURE_EVENTS_0 и GESTURE_EVENTS_1).
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес начального регистра (16-битный)
 * @param val Указатель на буфер для данных
 * @param len Количество байт для чтения
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int read_sequence_registers(const struct device *dev, uint16_t reg, void *val, size_t len) {
    const struct tps43_config *config = dev->config;
    uint8_t addr_buf[2];
    addr_buf[0] = (uint8_t)((reg >> 8) & 0xFF);
    addr_buf[1] = (uint8_t)(reg & 0xFF);

    return i2c_write_read_dt(&config->i2c_bus, addr_buf, 2, val, len);
}

/**
 * @brief Читает 16-битный регистр тачпада через I2C
 * 
 * Выполняет чтение 16-битного значения из указанного регистра тачпада.
 * Данные интерпретируются как big-endian (MSB first).
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Указатель на переменную для сохранения прочитанного значения
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
/* __maybe_unused: the hot path now reads these fields via one burst read, but
 * this stays as the counterpart to tps43_i2c_write_reg16 for one-off reads. */
__maybe_unused static int tps43_i2c_read_reg16(const struct device *dev, uint16_t reg, uint16_t *val)
{
    const struct tps43_config *config = dev->config;
    uint8_t buf[2];
    // формирует 2-байтовый адрес регистра: (MSB, LSB)
    // MSB: сдвиг на 8 бит вправо (0x2F00 -> 0x2F)
    // LSB: побитовое И с маской - маска, оставляет только младший байт (0x2F00 -> 0x00)
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};
    int ret;
    
    // записывает адрес регистра (reg_buf) и читает 2 байта данных (в буфер buf)
    ret = i2c_write_read_dt(&config->i2c_bus, reg_buf, sizeof(reg_buf), buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Ошибка чтения регистра 0x%04x: %d", reg, ret);
        return ret;
    }
    
    // преобразует big-endian данные (MSB first) обратно в 16-битное значение
    *val = (buf[0] << 8) | buf[1];
    return 0;
}

/**
 * @brief Записывает 16-битное значение в регистр тачпада через I2C
 * 
 * Выполняет запись 16-битного значения в указанный регистр тачпада.
 * Данные передаются как big-endian (MSB first).
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Значение для записи (16-битное)
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_i2c_write_reg16(const struct device *dev, uint16_t reg, uint16_t val)
{
    const struct tps43_config *config = dev->config;
    // формирует 4-байтовый адрес регистра: (MSB, LSB, MSB_VALUE, LSB_VALUE)
    uint8_t buf[4] = {reg >> 8, reg & 0xFF, val >> 8, val & 0xFF};
    int ret;
    
    ret = i2c_write_dt(&config->i2c_bus, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Ошибка записи регистра 0x%04x: %d", reg, ret);
        return ret;
    }
    
    return 0;
}

/**
 * @brief Читает 8-битный регистр тачпада через I2C
 * 
 * Выполняет чтение 8-битного значения из указанного регистра тачпада.
 * Используется для чтения большинства регистров конфигурации и статуса.
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Указатель на переменную для сохранения прочитанного значения
 * @param with_err Признак логирования ошибки или ожидаемое поведение
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_i2c_read_reg8_w_err(const struct device *dev, uint16_t reg, uint8_t *val, bool with_err)
{
    const struct tps43_config *config = dev->config;
    // формирует 2-байтовый адрес регистра: (MSB, LSB)
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};
    int ret;
    
    ret = i2c_write_read_dt(&config->i2c_bus, reg_buf, sizeof(reg_buf), val, 1);
    if (ret != 0) {
        if (!with_err) {
            LOG_INF("Ожидаемое завершение чтение регистра 0x%04x: %d", reg, ret);
        } else {
            LOG_ERR("Ошибка чтения регистра 0x%04x: %d", reg, ret);
        }
        return ret;
    }
    return 0;
}

static inline int tps43_i2c_read_reg8(const struct device *dev, uint16_t reg, uint8_t *val)
{
    return tps43_i2c_read_reg8_w_err(dev, reg, val, true);
}

/**
 * @brief Записывает 8-битное значение в регистр тачпада через I2C
 * 
 * Выполняет запись 8-битного значения в указанный регистр тачпада.
 * Используется для записи регистров конфигурации и управления.
 * 
 * @param dev Указатель на устройство тачпада
 * @param reg Адрес регистра (16-битный)
 * @param val Значение для записи (8-битное)
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_i2c_write_reg8(const struct device *dev, uint16_t reg, uint8_t val)
{
    const struct tps43_config *config = dev->config;
    uint8_t buf[3] = {reg >> 8, reg & 0xFF, val};
    int ret;
    
    ret = i2c_write_dt(&config->i2c_bus, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Ошибка записи регистра 0x%04x: %d", reg, ret);
        return ret;
    }
    
    return 0;
}

/**
 * @brief Callback обработчик прерывания от пина RDY тачпада
 * 
 * Вызывается при изменении состояния пина RDY (Ready) тачпада,
 * который сигнализирует о наличии новых данных для чтения.
 * Планирует выполнение обработчика работы для чтения данных.
 * 
 * @param dev Указатель на устройство тачпада
 * @param cb Указатель на структуру callback GPIO
 * @param pins Маска пинов, вызвавших прерывание
 */
 static void tps43_rdy_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
     struct tps43_drv_data *drv_data = CONTAINER_OF(cb, struct tps43_drv_data, rdy_cb);
 
     k_work_submit(&drv_data->work);
 }
 
/**
 * @brief Внутренняя функция для перевода тачпада в режим suspend/resume
 * 
 * Управляет регистром SYSTEM_CONTROL_1 (0x0432), устанавливая или снимая бит SUSPEND.
 * В режиме suspend тачпад переходит в состояние низкого энергопотребления и не обрабатывает
 * касания до пробуждения.
 * 
 * @param dev Указатель на устройство тачпада
 * @param suspend true - перевести в suspend, false - вывести из suspend
 * @param lock_held true если семафор уже захвачен (для внутреннего использования)
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_set_suspend_internal(const struct device *dev, bool suspend, bool lock_held) {
    struct tps43_drv_data *drv_data = dev->data;
    const struct tps43_config *config = dev->config;
    int ret = 0;

    // Если управление питанием отключено, ничего не делаем
    if (!config->enable_power_management) {
        return 0;
    }

    // Захватываем семафор, если он еще не захвачен
    if (!lock_held) {
        if (k_sem_take(&drv_data->lock, K_MSEC(100)) != 0) {
            LOG_WRN("Не удалось захватить семафор для suspend/resume");
            return -EBUSY;
        }
    }

    // Отключаем прерывания RDY при переходе в suspend (до любых I2C операций)
    // Это предотвращает race condition когда RDY срабатывает между попыткой suspend и установкой признака
    if (suspend && config->rdy_gpio.port != NULL) {
        ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_DISABLE);
        if (ret == 0) {
            LOG_INF("Прерывания RDY отключены перед suspend");
        }
    }

    uint8_t control_reg = 0;
    
    // При выходе из suspend первая транзакция вернет NACK (п.7.3.1)
    if (drv_data->suspended && !suspend) {
        ret = tps43_i2c_read_reg8_w_err(dev, TPS43_REG_SYSTEM_CONTROL_1, &control_reg, false);
        k_sleep(K_MSEC(200));
        LOG_INF("I2C Wake: устройство пробуждено из suspend");
        
        // После пробуждения читаем регистр повторно
        ret = tps43_i2c_read_reg8_w_err(dev, TPS43_REG_SYSTEM_CONTROL_1, &control_reg, false);

    } else if (!drv_data->suspended) {
        // Читаем текущее значение только если не в suspend
        ret = tps43_i2c_read_reg8_w_err(dev, TPS43_REG_SYSTEM_CONTROL_1, &control_reg, false);
        if (ret != 0) {
            // Если ошибка -5 (EIO) при попытке перевести в suspend - устройство уже в suspend
            if (ret == -EIO && suspend) {
                LOG_INF("Устройство уже в suspend (ошибка I2C)");
                drv_data->suspended = true;
                ret = 0;
                goto done;
            }
            LOG_ERR("Ошибка чтения SYSTEM_CONTROL_1: %d", ret);
            goto done;
        }
    }

    if (suspend) {
        control_reg |= TPS43_SUSPEND;
        LOG_INF("Перевод в suspend (низкое энергопотребление)");
    } else {
        control_reg &= ~TPS43_SUSPEND;
        LOG_INF("Выход из suspend");
    }

    ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONTROL_1, control_reg);
    if (ret != 0) {
        if (ret == -EIO && suspend) {
            LOG_INF("Не удалось записать suspend, устройство уже в suspend");
            drv_data->suspended = true;
            ret = 0;
            goto done;
        }
        LOG_ERR("Ошибка записи SYSTEM_CONTROL_1: %d", ret);
        goto done;
    }

    drv_data->suspended = suspend;

done:
    // Включаем прерывания RDY после resume
    if (!suspend && config->rdy_gpio.port != NULL) {
        ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_EDGE_TO_ACTIVE);
        if (ret == 0) {
            LOG_INF("Прерывания RDY включены");
        }
    }
    tps43_end_communication_window(dev);
    if (!lock_held) {
        k_sem_give(&drv_data->lock);
    }
    return ret;
}

/* Cursor gain (percent) for this report's movement: flat `sensitivity` for
 * slow/precision movement, ramping linearly to accel_max for fast swipes.
 *
 * Why not just raise `sensitivity`: it is a flat multiplier, so every 1-count
 * precision delta gets bigger too and fine movement turns coarse; it also
 * speeds up scroll, which consumes the scaled delta. Gaining only at speed
 * leaves precision movement exactly as before, and at speed the per-report
 * deltas are already many pixels, so a larger multiplier can't be seen as
 * stepping. The ramp is linear (no threshold jump) and the speed it keys off
 * is averaged, so the gain changes gradually rather than per noisy report.
 */
static int16_t tps43_accel_gain(const struct tps43_config *config, struct tps43_drv_data *data,
                                int16_t raw_x, int16_t raw_y) {
    const int16_t base = config->sensitivity;
    if (config->accel_max <= base || config->accel_upper_speed <= config->accel_lower_speed) {
        return base;
    }
    /* |v| ~= max + min/2 (within ~12% of hypot, no sqrt) -- same gain on both
     * axes so the direction of travel is preserved. */
    int32_t ax = abs(raw_x), ay = abs(raw_y);
    int32_t speed_x16 = (MAX(ax, ay) + MIN(ax, ay) / 2) * 16;
    /* 50/50 average with the previous report: ~one 10 ms report of lag on the
     * gain only, never on the motion itself. */
    data->accel_speed_x16 = (data->accel_speed_x16 + speed_x16) / 2;

    const int32_t lo = config->accel_lower_speed * 16;
    const int32_t hi = config->accel_upper_speed * 16;
    const int32_t v = CLAMP(data->accel_speed_x16, lo, hi);
    return (int16_t)(base + (int32_t)(config->accel_max - base) * (v - lo) / (hi - lo));
}

/* Scale a raw delta by `sensitivity` (percent) while carrying the sub-count
 * remainder in *accum instead of truncating it every report.
 *
 * Truncating rel*sensitivity/100 per event silently drops up to a whole count
 * each time. That is invisible during fast movement (deltas are large) but
 * dominates slow/precision movement, where deltas are 1-2 counts: at 150% a
 * stream of 1,1,1 emits 1,1,1 (a third of the motion lost) and 1,2,1,2 emits
 * 1,3,1,3 -- uneven stepping that reads as jumpiness. Raising the report rate
 * makes it worse, since faster reporting means smaller per-report deltas.
 * Carrying the remainder makes slow movement advance evenly without changing
 * the overall speed. Same technique already used for the scroll wheel.
 */
static int16_t tps43_scale_move(int16_t rel, int16_t sensitivity, int32_t *accum) {
    *accum += (int32_t)rel * sensitivity;
    int32_t whole = *accum / 100; /* truncates toward zero, so symmetric for +/- */
    /* Subtract the UNCLAMPED amount on purpose: if a delta were ever big enough
     * to clamp, keeping the excess would leave a backlog that fires max-size
     * reports for many cycles afterwards (a runaway cursor). Dropping it costs
     * motion only in a case that needs a full-pad sweep inside one 10 ms cycle. */
    *accum -= whole * 100;
    return (int16_t)CLAMP(whole, INT16_MIN, INT16_MAX);
}

/* Turn one axis' raw delta into wheel TICKS, carrying the fraction between
 * reports exactly as before -- the only change is the tick size.
 *
 * The accumulator counts rel*scroll_sensitivity in hundredths of a line, so
 * multiplying by scroll_steps_per_line before accumulating makes `/100` yield
 * sub-line ticks instead of whole lines. steps == 1 is bit-for-bit the old
 * behaviour, which is why it stays the default.
 */
static int16_t tps43_scroll_ticks(const struct tps43_config *config, int16_t rel, int32_t *accum) {
    const int32_t steps = MAX(config->scroll_steps_per_line, (int16_t)1);

    *accum += (int32_t)rel * config->scroll_sensitivity * steps;
    /* Backlog cap, so a fast flick can't burst a pile of pending motion into one
     * report (reads as a jump). The old fixed +-150 was 1.5 LINES only because a
     * tick was a line; scale it with the tick size to keep that meaning. */
    const int32_t cap = 150 * steps;
    *accum = CLAMP(*accum, -cap, cap);

    int16_t ticks = (int16_t)(*accum / 100);
    if (ticks != 0) {
        *accum -= (int32_t)ticks * 100;
    }
    return ticks;
}

/* ---- Momentum ("coast") scrolling --------------------------------------
 *
 * A MacBook keeps scrolling after the fingers leave the pad, decaying to a stop.
 * Nothing below us does that for a HID mouse: libinput only does kinetic
 * scrolling for real touchpads, and this pad reports as a mouse. So the driver
 * keeps emitting ticks itself, from the flick speed measured on the last scroll
 * report, decaying by a fixed percentage per step.
 *
 * Only worth enabling alongside hi-res ticks: at one tick per line each coast
 * step is a visible jump rather than a glide.
 */
static void tps43_momentum_stop(struct tps43_drv_data *drv_data) {
    if (drv_data->mom_active) {
        k_work_cancel_delayable(&drv_data->momentum_work);
        drv_data->mom_active = false;
    }
    drv_data->mom_velocity = 0;
    drv_data->mom_remainder = 0;
    drv_data->mom_last_ticks = 0;
}

static void tps43_momentum_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct tps43_drv_data *drv_data = CONTAINER_OF(dwork, struct tps43_drv_data, momentum_work);
    const struct device *dev = drv_data->dev;
    const struct tps43_config *config = dev->config;

    /* Decay BEFORE emitting: the finger's own last report already emitted a
     * full-speed tick, so repeating that speed once more would show up as a
     * lurch at the moment of lift. */
    drv_data->mom_velocity = drv_data->mom_velocity * config->scroll_momentum_decay / 100;

    if (k_uptime_get() - drv_data->mom_start_ms > config->scroll_momentum_max_ms) {
        drv_data->mom_velocity = 0;
    }

    drv_data->mom_remainder += drv_data->mom_velocity;
    int16_t ticks = (int16_t)(drv_data->mom_remainder / 1000);
    if (ticks != 0) {
        drv_data->mom_remainder -= (int32_t)ticks * 1000;
        input_report_rel(dev, drv_data->mom_last_horizontal ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL,
                         ticks, true, K_FOREVER);
    }

    /* Below a quarter tick per step the coast is no longer visible, and a
     * geometric decay never actually reaches zero. */
    if (abs(drv_data->mom_velocity) < 250) {
        drv_data->mom_active = false;
        drv_data->mom_velocity = 0;
        drv_data->mom_remainder = 0;
        return;
    }
    k_work_schedule(&drv_data->momentum_work, K_MSEC(config->scroll_momentum_interval_ms));
}

/* Start a coast from the flick speed of the last scroll report, if it was fast
 * enough. A slow, deliberate scroll must stop exactly where the finger left it. */
static void tps43_momentum_start(const struct device *dev) {
    const struct tps43_config *config = dev->config;
    struct tps43_drv_data *drv_data = dev->data;

    if (!config->scroll_momentum ||
        abs(drv_data->mom_last_ticks) < config->scroll_momentum_min_ticks) {
        drv_data->mom_last_ticks = 0;
        return;
    }

    drv_data->mom_velocity = (int32_t)drv_data->mom_last_ticks * 1000;
    drv_data->mom_remainder = 0;
    drv_data->mom_start_ms = k_uptime_get();
    drv_data->mom_active = true;
    drv_data->mom_last_ticks = 0;
    k_work_schedule(&drv_data->momentum_work, K_MSEC(config->scroll_momentum_interval_ms));
}

/**
 * @brief Основной обработчик работы для обработки событий тачпада
 * 
 * Выполняется при получении прерывания от тачпада (RDY pin).
 * Читает и обрабатывает события жестов, движения курсора и прокрутки,
 * преобразуя их в события ввода для системы ZMK.
 * Также управляет пробуждением тачпада из режима suspend при обнаружении активности.
 * 
 * Защищен семафором для предотвращения прерывания другими операциями I2C,
 * что обеспечивает плавное движение курсора без прерываний.
 * 
 * @param work Указатель на структуру работы
 */
static void tps43_work_handler(struct k_work *work) {
    struct tps43_drv_data *drv_data = CONTAINER_OF(work, struct tps43_drv_data, work);
    const struct device *dev = drv_data->dev;
    const struct tps43_config *config = dev->config;
    bool is_scroll_active = drv_data->scroll_active;
    bool is_drag_active = drv_data->drag_active;
    bool is_three_drag = drv_data->three_drag_active;
    /* Set by the ZOOM gesture below; suppresses cursor/scroll for that cycle the
     * same way `swiping` does, so a pinch doesn't also fling the pointer. */
    bool zooming = false;
    int ret;
    
    // Если устройство в suspend, игнорируем прерывание (RDY должен быть отключен)
    if (drv_data->suspended) {
        LOG_WRN("Прерывание RDY в suspend режиме - игнорируем");
        return;
    }
    
    // Захватываем семафор для защиты всех операций I2C от прерывания
    // Это предотвращает конфликты при одновременном доступе к тачпаду
    k_sem_take(&drv_data->lock, K_FOREVER);

    /* ONE burst read of the whole event block instead of a transaction per field.
     * 0x000D..0x0015 is contiguous and the chip latches its data for the entire
     * communication window, so this returns a single consistent snapshot:
     *
     *   [0] 0x000D gestures 0   [1] 0x000E gestures 1
     *   [2] 0x000F sys info 0   [3] 0x0010 sys info 1
     *   [4] 0x0011 finger count [5..6] 0x0012 rel X   [7..8] 0x0014 rel Y
     *
     * Every one of those used to be its own I2C transaction, and the finger count
     * was re-read up to three times per report by the different gesture blocks.
     * At a 10 ms report rate that was ~600 transactions/second, each one waking
     * the MCU and holding the bus -- a real cost on the battery-powered
     * peripheral half. Sampling them together is also more correct: the fields
     * can no longer disagree because they were read at different points in the
     * window. */
    uint8_t snap[9];
    ret = read_sequence_registers(dev, TPS43_REG_GESTURE_EVENTS_0, snap, sizeof(snap));
    if (ret < 0) {
        LOG_ERR("Ошибка чтения блока состояния: %d", ret);
        goto done;
    }

    const uint8_t gestures_events[2] = {snap[0], snap[1]};
    const uint8_t sys_info = snap[3];
    const uint8_t num_fingers = snap[4];
    /* Big-endian on the wire, same as tps43_i2c_read_reg16. */
    int16_t rel_x = (int16_t)(((uint16_t)snap[5] << 8) | snap[6]);
    int16_t rel_y = (int16_t)(((uint16_t)snap[7] << 8) | snap[8]);

    if (gestures_events[0] != 0 || gestures_events[1] != 0) {

        LOG_INF("Жесты: Одиночное=0x%02X, Мульти=0x%02X", gestures_events[0], gestures_events[1]);

        if (gestures_events[0] & TPS43_SINGLE_TAP) {
            LOG_INF("Одиночное касание → ЛЕВАЯ КНОПКА");
            input_report_key(dev, INPUT_BTN_0, 1, true, K_FOREVER);
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);
            /* Arm tap-then-hold drag: a fresh touch landing within the window
             * after this tap will grab and drag (see tap-drag block below). */
            if (config->tap_drag) {
                drv_data->last_tap_time = k_uptime_get();
            }
        }
        if (gestures_events[1] & TPS43_TWO_FINGER_TAP) {
            LOG_INF("Касание двумя пальцами → ПРАВАЯ КНОПКА");
            input_report_key(dev, INPUT_BTN_1, 1, true, K_FOREVER);  
            input_report_key(dev, INPUT_BTN_1, 0, true, K_FOREVER); 
        }
        /* Native chip press-and-hold drag (only when explicitly enabled; tap-drag
         * manages is_drag_active on its own, so keep these gated to avoid a
         * spurious release mid tap-drag). */
        if (config->press_and_hold && (gestures_events[0] & TPS43_PRESS_AND_HOLD) && (!(is_drag_active))) {
            LOG_INF("Обнаружено нажатие и удержание - ПЕРЕТАСКИВАНИЕ (УДЕРЖАНИЕ ЛЕВОЙ КНОПКИ)");
            // установить внутренний флаг на перетаскивание и нажимает левую кнопку мыши
            is_drag_active = true;
            input_report_key(dev, INPUT_BTN_0, 1, true, K_FOREVER); 
        }
        if (config->press_and_hold && (!(gestures_events[0] & TPS43_PRESS_AND_HOLD)) && (is_drag_active)) {
            LOG_INF("Обнаружено окончание нажатия и удержания - ОТПУСКАНИЕ (ОТПУСК ЛЕВОЙ КНОПКИ)");
            // установить внутренний флаг на перетаскивание и нажимает левую кнопку мыши
            is_drag_active = false;
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);   // отпускание + синхронизация
        }
        if (gestures_events[1] & TPS43_SCROLL) {
            LOG_INF("Обнаружена прокрутка - Прокрутка");
            // устанавливаем признак скролла для обработки в блоке tp_movement
            is_scroll_active = true;
        }
        /* Pinch-to-zoom -> Ctrl+= / Ctrl+- on the central.
         *
         * The chip reports ZOOM for every cycle of the pinch, so the separation
         * delta is spent in detents (zoom_step_px) -- emitting one key per report
         * would fire dozens of presses per pinch. The buttons are consumed by the
         * behaviors input processor on the central, so they never reach the HID
         * mouse report (and they are above ZMK's 5 mouse buttons anyway).
         *
         * ⚠️ rel_x carrying the change in FINGER SEPARATION during a zoom comes
         * from the IQS5xx gesture documentation, not from measurement on this
         * module: if zoom works but inverts, set `invert-zoom`; if it misfires,
         * drop `zoom` from the overlay and 2-finger scroll is untouched. */
        if (config->zoom && (gestures_events[1] & TPS43_ZOOM)) {
            zooming = true;
            is_scroll_active = false;
            drv_data->zoom_accum += config->invert_zoom ? -rel_x : rel_x;
            const int32_t step = MAX(config->zoom_step_px, (int16_t)1);
            while (drv_data->zoom_accum >= step) {
                drv_data->zoom_accum -= step;
                LOG_INF("Zoom in (BTN_5)");
                input_report_key(dev, INPUT_BTN_5, 1, true, K_FOREVER);
                input_report_key(dev, INPUT_BTN_5, 0, true, K_FOREVER);
            }
            while (drv_data->zoom_accum <= -step) {
                drv_data->zoom_accum += step;
                LOG_INF("Zoom out (BTN_6)");
                input_report_key(dev, INPUT_BTN_6, 1, true, K_FOREVER);
                input_report_key(dev, INPUT_BTN_6, 0, true, K_FOREVER);
            }
        }
    }

    {
        bool now_down = (num_fingers >= 1);
        /* Emit a HELD "touch present" button on the finger-down/up edges so the
         * central can gate a momentary layer on finger PRESENCE with a behaviors
         * input processor (INPUT_BTN_TOUCH -> &mo). ZMK's input listener ignores
         * the code when nothing maps it, so it never reaches the HID mouse report.
         * tps43_finger_down is only written here, so it holds the previous state
         * -> fire once per transition. */
        if (now_down != tps43_finger_down) {
            input_report_key(dev, INPUT_BTN_TOUCH, now_down ? 1 : 0, true, K_FOREVER);
        }
        tps43_finger_down = now_down;
        if (now_down) {
            /* Touching the pad kills a coast in progress -- that's how you "catch"
             * a scrolling page, exactly like a MacBook. */
            if (drv_data->mom_active) {
                tps43_momentum_stop(drv_data);
            }
        } else {
            /* Drop the cursor sub-count remainder once every finger is off the
             * pad. Carrying it across a lift would let up to ~0.99 of a count
             * from the last touch be emitted on the FIRST report of the next
             * one -- a stray pixel at an unrelated screen position. Within a
             * single touch the remainder is exactly what we want to keep. */
            drv_data->move_accum_x = 0;
            drv_data->move_accum_y = 0;
            drv_data->accel_speed_x16 = 0;
            drv_data->zoom_accum = 0;
            /* Every finger is off the pad: if the last thing we emitted was a
             * fast scroll, keep it gliding. */
            tps43_momentum_start(dev);
        }
    }

    /* Robust release for the press-and-hold LEFT drag. The chip usually emits NO
     * gesture event on the finger-lift cycle, so the in-gesture release above
     * gets skipped and BTN_0 stays stuck DOWN -> the next single tap then just
     * releases the stuck button instead of clicking (state desync). Release here
     * on finger lift (count == 0) as the source of truth. */
    if (config->press_and_hold && is_drag_active) {
        if (num_fingers == 0) {
            LOG_INF("Press-hold: отпускание левой кнопки по отрыву пальца");
            is_drag_active = false;
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);
        }
    }

    /* Tap-then-hold drag ("double-tap-drag"): a fresh touch (finger count going
     * 0 -> >=1) that lands within tap_drag_window_ms of a single tap grabs by
     * holding the left button; releasing all fingers releases it. This runs
     * OUTSIDE the movement block so the release is detected even when the final
     * lift carries no movement. */
    if (config->tap_drag) {
        const uint8_t num_fingers_now = num_fingers;

        if (!is_drag_active && drv_data->prev_num_fingers == 0 && num_fingers_now >= 1 &&
            drv_data->last_tap_time != 0 &&
            (k_uptime_get() - drv_data->last_tap_time) <= config->tap_drag_window_ms) {
            LOG_INF("Tap-drag: захват (удержание левой кнопки)");
            is_drag_active = true;
            drv_data->last_tap_time = 0;
            input_report_key(dev, INPUT_BTN_0, 1, true, K_FOREVER);
        }
        if (is_drag_active && num_fingers_now == 0) {
            LOG_INF("Tap-drag: отпускание левой кнопки");
            is_drag_active = false;
            input_report_key(dev, INPUT_BTN_0, 0, true, K_FOREVER);
        }
        drv_data->prev_num_fingers = num_fingers_now;
    }

    /* Three-finger middle-button drag: hold the MIDDLE button while >=3 fingers
     * touch, release when the count drops below 3. A quick 3-finger tap becomes
     * a middle click; a 3-finger hold + move becomes a middle-drag (the cursor
     * movement below is emitted normally while the button is held) -> lets you
     * rotate/pan a 3D view in a slicer. Runs OUTSIDE the movement block so the
     * press/release fire even with no movement. */
    if (config->three_finger_drag) {
        if (!is_three_drag && num_fingers >= 3) {
            LOG_INF("3-finger drag: захват (удержание средней кнопки)");
            is_three_drag = true;
            input_report_key(dev, INPUT_BTN_2, 1, true, K_FOREVER);
        }
        if (is_three_drag && num_fingers < 3) {
            LOG_INF("3-finger drag: отпускание средней кнопки");
            is_three_drag = false;
            input_report_key(dev, INPUT_BTN_2, 0, true, K_FOREVER);
        }
    }

    /* Re-arm the 3-finger swipe once fewer than 3 fingers remain (so the next
     * swipe can fire). Runs outside the movement block because the finger-lift
     * cycle may carry no movement. */
    if (config->swipes && drv_data->swipe_fired) {
        if (num_fingers < 3) {
            drv_data->swipe_fired = false;
        }
    }

    /* 3-finger TAP -> MIDDLE click (BTN_2). A quick >=3-finger touch-and-lift
     * with no significant movement. Coexists with the back/forward swipes: real
     * 3-finger movement sets tf_moved (movement block above) and suppresses the
     * tap. Needs max-multitouches>=3 (already set for swipes). Runs outside the
     * movement block because a tap and the finger-lift carry no movement.
     * 300ms cap keeps a slow 3-finger drag from counting as a tap. */
    if (config->swipes) {
        if (num_fingers >= 3 && !drv_data->tf_active) {
            drv_data->tf_active = true;
            drv_data->tf_start = k_uptime_get();
            drv_data->tf_moved = false;
        } else if (num_fingers == 0 && drv_data->tf_active) {
            if (!drv_data->tf_moved && (k_uptime_get() - drv_data->tf_start) <= 300) {
                LOG_INF("3-finger tap -> MIDDLE (BTN_2)");
                input_report_key(dev, INPUT_BTN_2, 1, true, K_FOREVER);
                input_report_key(dev, INPUT_BTN_2, 0, true, K_FOREVER);
            }
            drv_data->tf_active = false;
        }
    }

    if (sys_info & TPS43_TP_MOVEMENT) {
        /* rel_x / rel_y already came from the snapshot burst read above. */
        // Отправляем движение курсора
        if (rel_x != 0 || rel_y != 0) {
            /* Unscaled deltas, kept for the cursor accumulator below. The scaling
             * that follows stays as-is because the scroll branch consumes its
             * result (scroll speed is tuned against that combined factor). */
            const int16_t raw_x = rel_x;
            const int16_t raw_y = rel_y;
            if (rel_x != 0 ) {
                int32_t scaled_x = ((int32_t)rel_x * config->sensitivity) / 100;
                rel_x = (int16_t)CLAMP(scaled_x, INT16_MIN, INT16_MAX);
            }
            if (rel_y != 0) { 
                int32_t scaled_y = ((int32_t)rel_y * config->sensitivity) / 100;
                rel_y = (int16_t)CLAMP(scaled_y, INT16_MIN, INT16_MAX);
            }
            LOG_INF("Отправка движения: dx=%d, dy=%d", rel_x, rel_y);

            /* 3-finger swipe -> browser Back/Forward. Debounced (one press per
             * swipe, not one per cycle). BTN_3 = back, BTN_4 = forward (these are
             * the mouse buttons the OS maps to page nav; ZMK's HID reports 5
             * buttons). While >=3 fingers are down we're "swiping", so the
             * cursor/scroll handling below is skipped. If the direction feels
             * reversed, swap BTN_3 and BTN_4. */
            bool swiping = false;
            if (config->swipes) {
                if (num_fingers >= 3) {
                    swiping = true;
                    is_scroll_active = false;
                    /* Any significant 3-finger movement means swipe/drag, not a
                     * tap -> cancel the pending 3-finger-tap middle click. */
                    if (abs(rel_x) > 8 || abs(rel_y) > 8) {
                        drv_data->tf_moved = true;
                    }
                    /* Horizontal 3-finger swipe -> browser Back/Forward. If the
                     * direction feels reversed, swap BTN_3 and BTN_4. */
                    if (!drv_data->swipe_fired && abs(rel_x) > 8) {
                        if (rel_x < 0) {
                            LOG_INF("3-finger swipe -> BACK (BTN_3)");
                            input_report_key(dev, INPUT_BTN_3, 1, true, K_FOREVER);
                            input_report_key(dev, INPUT_BTN_3, 0, true, K_FOREVER);
                        } else {
                            LOG_INF("3-finger swipe -> FORWARD (BTN_4)");
                            input_report_key(dev, INPUT_BTN_4, 1, true, K_FOREVER);
                            input_report_key(dev, INPUT_BTN_4, 0, true, K_FOREVER);
                        }
                        drv_data->swipe_fired = true;
                    }
                }
            }

            if (!swiping && !zooming && is_scroll_active) {
                /* Scroll ONLY while two fingers are physically down. The IQS5xx keeps
                 * reporting the SCROLL gesture for the whole touch session (until ALL
                 * fingers lift), so lifting one finger to keep navigating would stay
                 * stuck in scroll mode. Check the live finger count each event (from
                 * the snapshot read at the top) and fall back to cursor movement the
                 * moment it drops below two. */
                if (num_fingers >= 2) {
                    /* Scroll owns the motion now -- drop any cursor remainder so
                     * it can't leak into the next cursor movement. */
                    drv_data->move_accum_x = 0;
                    drv_data->move_accum_y = 0;
                    drv_data->accel_speed_x16 = 0;
                    /* Smooth scroll via a fractional accumulator: keep the
                     * remainder between events instead of truncating each time,
                     * so low sensitivities emit an even 1-line cadence instead of
                     * 0,0,0,N bursts (the source of the jerkiness). Only the
                     * dominant axis scrolls; the other axis' accumulator resets. */
                    if (abs(rel_x) > abs(rel_y)) {
                        // Горизонтальный скролл
                        if (config->invert_scroll_x) {
                            rel_x = -rel_x;
                        }
                        int16_t wheel = tps43_scroll_ticks(config, rel_x,
                                                           &drv_data->scroll_accum_x);
                        if (wheel != 0) {
                            input_report_rel(dev, INPUT_REL_HWHEEL, wheel, true, K_FOREVER);
                            /* Flick speed for the coast, should the fingers lift. */
                            drv_data->mom_last_ticks = wheel;
                            drv_data->mom_last_horizontal = true;
                        }
                        drv_data->scroll_accum_y = 0;
                    } else {
                        // Вертикальный скролл
                        if (config->invert_scroll_y) {
                            rel_y = -rel_y;
                        }
                        int16_t wheel = tps43_scroll_ticks(config, rel_y,
                                                           &drv_data->scroll_accum_y);
                        if (wheel != 0) {
                            input_report_rel(dev, INPUT_REL_WHEEL, wheel, true, K_FOREVER);
                            drv_data->mom_last_ticks = wheel;
                            drv_data->mom_last_horizontal = false;
                        }
                        drv_data->scroll_accum_x = 0;
                    }
                } else {
                    // Меньше двух пальцев -> завершаем скролл, двигаем курсор
                    drv_data->scroll_accum_x = 0;
                    drv_data->scroll_accum_y = 0;
                    /* Moving the cursor after a scroll means the lift that
                     * follows is not a scroll flick -- don't coast. */
                    drv_data->mom_last_ticks = 0;
                    int16_t gain = tps43_accel_gain(config, drv_data, raw_x, raw_y);
                    int16_t mx = tps43_scale_move(raw_x, gain, &drv_data->move_accum_x);
                    int16_t my = tps43_scale_move(raw_y, gain, &drv_data->move_accum_y);
                    input_report_rel(dev, INPUT_REL_X, mx, false, K_FOREVER);
                    input_report_rel(dev, INPUT_REL_Y, my, true, K_FOREVER);
                }
                is_scroll_active = false;
            } else if (!swiping && !zooming) {
                // Обычное движение курсора (сбрасываем накопители скролла)
                drv_data->scroll_accum_x = 0;
                drv_data->scroll_accum_y = 0;
                drv_data->mom_last_ticks = 0;
                int16_t gain = tps43_accel_gain(config, drv_data, raw_x, raw_y);
                int16_t mx = tps43_scale_move(raw_x, gain, &drv_data->move_accum_x);
                int16_t my = tps43_scale_move(raw_y, gain, &drv_data->move_accum_y);
                input_report_rel(dev, INPUT_REL_X, mx, false, K_FOREVER);
                input_report_rel(dev, INPUT_REL_Y, my, true, K_FOREVER);
            }
        }
    }

done:
    // Сохраняем для следующего вызова
    drv_data->scroll_active = is_scroll_active;
    drv_data->drag_active = is_drag_active;
    drv_data->three_drag_active = is_three_drag;
    tps43_end_communication_window(dev);
    
    // Освобождаем семафор после завершения всех операций I2C
    k_sem_give(&drv_data->lock);
}

/**
 * @brief Сбрасывает внутренние значения состояния драйвера
 * 
 * Инициализирует все флаги состояния драйвера в начальные значения.
 * Используется при инициализации и сбросе устройства.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе
 */
static int tps43_reset_values(const struct device *dev) {
    struct tps43_drv_data *drv_data = dev->data;

    drv_data->device_ready = false;
    drv_data->initialized = false;
    drv_data->scroll_active = false;
    drv_data->drag_active = false;
    drv_data->last_tap_time = 0;
    drv_data->prev_num_fingers = 0;
    drv_data->three_drag_active = false;
    drv_data->scroll_accum_x = 0;
    drv_data->scroll_accum_y = 0;
    drv_data->move_accum_x = 0;
    drv_data->move_accum_y = 0;
    drv_data->accel_speed_x16 = 0;
    drv_data->swipe_fired = false;
    drv_data->tf_active = false;
    drv_data->tf_start = 0;
    drv_data->tf_moved = false;
    drv_data->zoom_accum = 0;
    tps43_momentum_stop(drv_data);

    LOG_INF("Сброс значений");
    return 0;
}

/* ---- Factory-value readout (debug-report-registers) ----
 *
 * This pad's NVM defaults are published nowhere: the TPS43 module datasheet
 * carries no register values, Azoteq ship each module "bundled with settings
 * specifically configured for the current hardware", and no reference driver
 * writes these registers. But Azoteq's tuning procedure is measure-then-choose
 * (AZD070 4.3: read the counts, pick ATI C from them, check the resulting
 * compensation lands mid-range), so tuning the overlay blind is guesswork.
 * The values are logged; read them with USB logging on the trackpad half.
 */

static void tps43_report_registers(const struct device *dev) {
    /* AUTO_ATI self-clears when it finishes, and the compensation values are only
     * meaningful afterwards. The chip holds off I2C during the routine anyway;
     * this just avoids reading a half-tuned set. */
    for (int i = 0; i < 30; i++) {
        uint8_t ctrl = 0;
        if (tps43_i2c_read_reg8(dev, TPS43_REG_SYSTEM_CONTROL_0, &ctrl) != 0) {
            break;
        }
        if ((ctrl & TPS43_AUTO_ATI) == 0) {
            break;
        }
        k_sleep(K_MSEC(10));
    }

    uint8_t set_mult = 0, clear_mult = 0, ati_c = 0;
    if (tps43_i2c_read_reg8(dev, TPS43_REG_TOUCH_MULT_SET, &set_mult) != 0 ||
        tps43_i2c_read_reg8(dev, TPS43_REG_TOUCH_MULT_CLEAR, &clear_mult) != 0 ||
        tps43_i2c_read_reg8(dev, TPS43_REG_ATI_C_GLOBAL, &ati_c) != 0) {
        LOG_WRN("Не удалось прочитать регистры для диагностики");
        return;
    }

    /* Static: 150 bytes is more than the init thread's stack wants to carry, and
     * this runs once. */
    static uint8_t comp[TPS43_ATI_COMPENSATION_LEN];
    uint8_t comp_min = 0, comp_max = 0;
    if (read_sequence_registers(dev, TPS43_REG_ATI_COMPENSATION, comp, sizeof(comp)) == 0) {
        comp_min = 255;
        for (size_t i = 0; i < sizeof(comp); i++) {
            /* Channels the pad does not use are never tuned and stay 0; counting
             * them would peg the minimum at 0 and hide the real spread. */
            if (comp[i] == 0) {
                continue;
            }
            comp_min = MIN(comp_min, comp[i]);
            comp_max = MAX(comp_max, comp[i]);
        }
        if (comp_min == 255 && comp_max == 0) {
            comp_min = 0;
        }
    } else {
        LOG_WRN("Не удалось прочитать ATI compensation");
    }

    LOG_INF("Факт. значения пада: touch SET=%u CLEAR=%u, Global ATI C=%u, "
            "ATI compensation %u..%u (цель 100-150)",
            (unsigned int)set_mult, (unsigned int)clear_mult, (unsigned int)ati_c,
            (unsigned int)comp_min, (unsigned int)comp_max);
}

/**
 * @brief Конфигурирует системные регистры тачпада для работы
 * 
 * Настраивает регистры тачпада для отслеживания событий касания, жестов и движения.
 * Включает необходимые жесты (single tap, press and hold, scroll, two finger tap),
 * настраивает инверсию осей и устанавливает флаг завершения настройки.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_configure_device(const struct device *dev) {

    const struct tps43_config *config = dev->config;
    int ret;

    // запись в TPS43_REG_SYSTEM_CONFIG_1 событий для отслеживания  
    uint8_t events_to_track = TPS43_TP_EVENT | TPS43_EVENT_MODE;
    
    // Жесты (single_tap, press_and_hold, scroll, two_finger_tap)
    if (config->single_tap || config->press_and_hold || 
        config->scroll || config->two_finger_tap) {
        events_to_track |= TPS43_GESTURE_EVENT;
    }
    
    // Touch events для абсолютных координат
    events_to_track |= TPS43_TOUCH_EVENT;
    
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONFIG_1, events_to_track);
    if (ret != 0) {
        LOG_WRN("Ошибка записи событий для отслеживания: %d", ret);
        return ret;
    }
    LOG_INF("События сконфигурированы: 0x%02X", events_to_track);

    // конфигурация осей
    uint8_t xy_config = 0;
    xy_config |= config->invert_x ? TPS43_FLIP_X : 0;
    xy_config |= config->invert_y ? TPS43_FLIP_Y : 0;
    xy_config |= config->switch_xy ? TPS43_SWITCH_XY_AXIS : 0;
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_XY_CONFIG_0, xy_config);
    if (ret != 0) {
        LOG_WRN("Ошибка записи конфигурации XY: %d", ret);
        return ret;
    }

    // включение одиночных жестов на уровне железа
    if (config->single_tap || config->press_and_hold || config->swipes) {
        uint8_t single_gestures = 0;
        single_gestures |= config->single_tap ? TPS43_SINGLE_TAP : 0;
        single_gestures |= config->press_and_hold ? TPS43_PRESS_AND_HOLD : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_UP : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_DOWN : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_LEFT : 0;
        single_gestures |= config->swipes ? TPS43_SWIPE_RIGHT : 0;
        
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_SINGLE_FINGER_GESTURES, single_gestures);
        if (ret != 0) {
            LOG_WRN("Ошибка конфигурации одиночных жестов: %d", ret);
            return ret;
        }
        LOG_INF("Одиночные жесты включены: 0x%02X", single_gestures);
    }

    /* Press-and-hold hold time (reg 0x06BD, big-endian ms). The drag gesture
     * fires after TapTime + HoldTime, so a smaller HoldTime = faster drag.
     * Only written when press-and-hold is enabled (else the chip default
     * stands). */
    if (config->press_and_hold && config->hold_time_ms > 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_HOLD_TIME, (uint16_t)config->hold_time_ms);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Hold time: %d", ret);
            return ret;
        }
        LOG_INF("Hold time установлен: %d ms", config->hold_time_ms);
    }

    // включение мульти-жестов
    if (config->two_finger_tap || config->scroll || config->zoom) {
        uint8_t multi_gestures = 0;
        multi_gestures |= config->two_finger_tap ? TPS43_TWO_FINGER_TAP : 0;
        multi_gestures |= config->scroll ? TPS43_SCROLL : 0;
        multi_gestures |= config->zoom ? TPS43_ZOOM : 0;
        
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_MULTI_FINGER_GESTURES, multi_gestures);
        if (ret != 0) {
            LOG_WRN("Ошибка конфигурации мульти-жестов: %d", ret);
            return ret;
        }
        LOG_INF("Мульти-жесты включены: 0x%02X", multi_gestures);
    }

    /* Max simultaneous touches (reg 0x066A). Default is low (~2) -> a 3rd finger
     * trips TOO_MANY_FINGERS and clears XY. Raise it so 3-finger gestures get
     * data. Forced to >=3 when three-finger-drag is enabled. */
    uint8_t max_touch = config->max_multitouches;
    if (config->three_finger_drag && max_touch < 3) {
        max_touch = 3;
    }
    if (max_touch > 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_MAX_MULTITOUCH, max_touch);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Max multi-touches: %d", ret);
            return ret;
        }
        LOG_INF("Max multi-touches установлен: %d", max_touch);
    }

    /* Finger-split aggression (reg 0x066B, datasheet 5.7). 0 = never split a
     * merged polygon, so a single finger stays ONE contact -> single-finger
     * gestures survive even with max-multitouches>=3 (which otherwise lets the
     * chip split one finger into two and invalidate single-tap). >=0 = write. */
    if (config->finger_split >= 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_FINGER_SPLIT, (uint8_t)config->finger_split);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Finger split: %d", ret);
            return ret;
        }
        LOG_INF("Finger split aggression установлен: %d", config->finger_split);
    }

    /* Scroll initial distance (reg 0x06C8, big-endian px). The chip only starts
     * reporting SCROLL after the two fingers travel this far, so a smaller value
     * makes scroll engage on slighter movement. Only written when scroll is on
     * and a value is set (else chip default stands). */
    if (config->scroll && config->scroll_init_distance > 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_SCROLL_INIT_DIST,
                                    (uint16_t)config->scroll_init_distance);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Scroll initial distance: %d", ret);
            return ret;
        }
        LOG_INF("Scroll initial distance установлен: %d px", config->scroll_init_distance);
    }

    /* ATI target (reg 0x056D, big-endian). The chip auto-tunes its touch gain to
     * this target; a higher value = more sensitive, to compensate for a glass/
     * overlay. A hardware reset reloads the NVM default, so we (re)write it here
     * every configure; AUTO_ATI below makes the new gain take effect. */
    if (config->ati_target > 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_ATI_TARGET, (uint16_t)config->ati_target);
        if (ret != 0) {
            LOG_WRN("Ошибка записи ATI target: %d", ret);
            return ret;
        }
        LOG_INF("ATI target установлен: %d", config->ati_target);
    }

    /* Global ATI C (reg 0x056B). The other half of the ATI gain setting, and the
     * one to prefer under a thick overlay: lowering it raises sensitivity without
     * lengthening the charge time, whereas reaching the same sensitivity by
     * raising ati_target costs report rate. Must be written before AUTO_ATI. */
    if (config->ati_c >= 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_ATI_C_GLOBAL, (uint8_t)config->ati_c);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Global ATI C: %d", ret);
            return ret;
        }
        LOG_INF("Global ATI C установлен: %d", config->ati_c);
    }

    // конфигурация фильтров
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_FILTER_SETTINGS, config->filter_settings);
    if (ret != 0) {
        LOG_WRN("Ошибка записи настроек фильтров: %d", ret);
        return ret;
    }
    LOG_INF("Настройки фильтров установлены: 0x%02X", config->filter_settings);

    /* Shape the dynamic filter (datasheet 5.9.2.1). filter-settings only turns
     * the filter on; these decide how it behaves. A low bottom beta smooths hard
     * while the finger creeps (kills jitter), and low speed thresholds make the
     * filter release early once the finger moves (kills lag). */
    if (config->dyn_bottom_beta >= 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_XY_DYN_BOTTOM_BETA,
                                   (uint8_t)config->dyn_bottom_beta);
        if (ret != 0) {
            LOG_WRN("Ошибка записи XY dynamic bottom beta: %d", ret);
            return ret;
        }
        LOG_INF("XY dynamic bottom beta установлен: %d", config->dyn_bottom_beta);
    }
    if (config->dyn_lower_speed >= 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_XY_DYN_LOWER_SPEED,
                                   (uint8_t)config->dyn_lower_speed);
        if (ret != 0) {
            LOG_WRN("Ошибка записи XY dynamic lower speed: %d", ret);
            return ret;
        }
        LOG_INF("XY dynamic lower speed установлен: %d", config->dyn_lower_speed);
    }
    if (config->dyn_upper_speed >= 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_XY_DYN_UPPER_SPEED,
                                    (uint16_t)config->dyn_upper_speed);
        if (ret != 0) {
            LOG_WRN("Ошибка записи XY dynamic upper speed: %d", ret);
            return ret;
        }
        LOG_INF("XY dynamic upper speed установлен: %d", config->dyn_upper_speed);
    }

    /* Touch thresholds (datasheet 3.5.2, AZD087 5.6.1). Threshold = Reference x
     * (1 + Multiplier/128), so a SMALLER multiplier detects a WEAKER touch.
     *
     * The important one is the SET/CLEAR gap. The chip derives the finger's XY
     * from the centroid of the channels currently over threshold; through a
     * glass + film overlay the rim channels of the contact patch sit right AT
     * the threshold, so they flip in and out between reports and drag the
     * centroid with them -- which is felt as fine movement jumping, worst when
     * the contact patch is changing shape (diagonals, direction changes during a
     * drag). Hysteresis (CLEAR below SET) latches those channels on once they
     * are seen, which is Azoteq's documented fix for exactly this. It cannot
     * make touch detection less sensitive: SET is untouched, only release is. */
    if (config->touch_set_multiplier >= 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_TOUCH_MULT_SET,
                                   (uint8_t)config->touch_set_multiplier);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Touch multiplier (set): %d", ret);
            return ret;
        }
        LOG_INF("Touch multiplier (set) установлен: %d", config->touch_set_multiplier);
    }
    if (config->touch_clear_multiplier >= 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_TOUCH_MULT_CLEAR,
                                   (uint8_t)config->touch_clear_multiplier);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Touch multiplier (clear): %d", ret);
            return ret;
        }
        LOG_INF("Touch multiplier (clear) установлен: %d", config->touch_clear_multiplier);
    } else if (config->touch_hysteresis >= 0) {
        /* Relative form: read whatever SET currently is (NVM default, or what we
         * just wrote) and place CLEAR below it. Avoids hard-coding a threshold
         * for a pad whose factory values we have never read. */
        uint8_t set_mult = 0;
        ret = tps43_i2c_read_reg8(dev, TPS43_REG_TOUCH_MULT_SET, &set_mult);
        if (ret != 0) {
            LOG_WRN("Ошибка чтения Touch multiplier (set): %d", ret);
        } else {
            /* Keep CLEAR >= 1: a 0 multiplier would clear only on a delta of
             * exactly the reference, effectively latching the channel on. */
            uint8_t clear_mult = (set_mult > config->touch_hysteresis)
                                     ? (uint8_t)(set_mult - config->touch_hysteresis)
                                     : 1;
            ret = tps43_i2c_write_reg8(dev, TPS43_REG_TOUCH_MULT_CLEAR, clear_mult);
            if (ret != 0) {
                LOG_WRN("Ошибка записи Touch multiplier (clear): %d", ret);
                return ret;
            }
            LOG_INF("Touch hysteresis: set=%d -> clear=%d", set_mult, clear_mult);
        }
    }

    /* Active-mode report rate (reg 0x057A, big-endian ms). Lower = more reports
     * per second = less latency. A hardware reset reloads the NVM default, so
     * this is rewritten on every configure. */
    if (config->report_rate_active_ms > 0) {
        ret = tps43_i2c_write_reg16(dev, TPS43_REG_REPORT_RATE_ACTIVE,
                                    (uint16_t)config->report_rate_active_ms);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Active report rate: %d", ret);
            return ret;
        }
        LOG_INF("Active report rate установлен: %d ms", config->report_rate_active_ms);
    }

    /* Idle-mode timeout (reg 0x0586, seconds). Battery knob for the peripheral
     * half: the chip holds Idle mode (~340 uA) this long after a finger lifts
     * before stepping down to LP1 (~47 uA), and the NVM default of 10 s is longer
     * than the gaps between strokes in normal use, so it effectively never steps
     * down while you are mousing. Like the report rate above, a hardware reset
     * reloads the NVM default, so this is rewritten on every configure. */
    if (config->timeout_idle_s > 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_TIMEOUT_IDLE,
                                   (uint8_t)config->timeout_idle_s);
        if (ret != 0) {
            LOG_WRN("Ошибка записи Idle timeout: %d", ret);
            return ret;
        }
        LOG_INF("Idle timeout установлен: %d s", config->timeout_idle_s);
    }

    // установка признака завершения конфигурации
    ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONFIG_0, TPS43_SETUP_COMPLETE);
    if (ret != 0) {
        LOG_WRN("Ошибка записи флага завершения настройки: %d", ret);
        return ret;
    }

    /* Re-run ATI so a changed ATI target actually re-tunes the gain (the startup
     * ATI ran with the NVM-default target before we wrote ours). The AUTO_ATI bit
     * auto-clears when the routine finishes. */
    if (config->ati_target > 0 || config->ati_c >= 0) {
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONTROL_0, TPS43_AUTO_ATI);
        if (ret != 0) {
            LOG_WRN("Ошибка запуска AUTO_ATI: %d", ret);
        } else {
            LOG_INF("AUTO_ATI запущен (перетюнинг усиления под стекло)");
            k_sleep(K_MSEC(50));
        }
    }

    if (config->debug_report_registers) {
        tps43_report_registers(dev);
    }

    return 0;
}

/**
 * @brief Проверяет состояние сброса устройства и выполняет реконфигурацию
 * 
 * Ожидает готовности устройства после сброса, проверяет флаг SHOW_RESET
 * и отправляет подтверждение сброса (ACK_RESET) при необходимости.
 * Затем выполняет полную конфигурацию устройства.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int check_reset_and_reconfigure(const struct device *dev) {
    struct tps43_drv_data *drv_data = dev->data;
    int ret;
    uint8_t sys_info = 0;
    uint8_t wait_count = 0;
    const uint8_t max_wait_count = 50;

    // Ожидание готовности устройства
    do {
        ret = tps43_i2c_read_reg8(dev, TPS43_REG_SYSTEM_INFO_0, &sys_info);
        if (ret < 0) {
            k_sleep(K_MSEC(100));
            wait_count++;
            if (wait_count >= max_wait_count) {
                LOG_ERR("Устройство не отвечает после %d мс", wait_count * 100);
                return -ETIMEDOUT;
            }
        }
    } while (ret < 0);
    
    LOG_INF("Устройство готово через %d мс", wait_count * 100);

    // после сброса устанавливаем флаг на подтверждение что сброс был выполнен
    if (sys_info & TPS43_SHOW_RESET) {
        LOG_INF("Обнаружен SHOW_RESET, отправка ACK_RESET");
        ret = tps43_i2c_write_reg8(dev, TPS43_REG_SYSTEM_CONTROL_0, TPS43_ACK_RESET);
        if (ret != 0) {
            LOG_ERR("Ошибка отправки ACK_RESET: %d", ret);
            return ret;
        }
        k_sleep(K_MSEC(10));
    }

    ret = tps43_configure_device(dev);
    if (ret != 0) {
        LOG_ERR("Ошибка конфигурации устройства: %d", ret);
        return ret;
    }

    drv_data->device_ready = true;
    
    return 0;
}

/**
 * @brief Публичная функция для перевода тачпада в suspend/resume
 * 
 * @param dev Указатель на устройство тачпада
 * @param suspend true - перевести в suspend, false - вывести из suspend
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_set_suspend(const struct device *dev, bool suspend) {
    return tps43_set_suspend_internal(dev, suspend, false);
}

/**
 * @brief Инициализирует драйвер тачпада TPS43
 * 
 * Выполняет полную инициализацию драйвера: проверяет доступность I2C шины,
 * выполняет аппаратный сброс через GPIO RST (если подключен), ожидает готовности
 * устройства, конфигурирует регистры тачпада и настраивает прерывания GPIO RDY.
 * Также инициализирует систему управления питанием при необходимости.
 * 
 * @param dev Указатель на устройство тачпада
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
static int tps43_init(const struct device *dev) {

    struct tps43_drv_data *drv_data = dev->data;
    const struct tps43_config *config = dev->config;
    int ret;

    drv_data->dev = dev;

    LOG_INF("=== Драйвер Azoteq tps43 для устройства %s ===", dev->name);
    
    // Проверка I2C шины
    if (!device_is_ready(config->i2c_bus.bus)) {
        LOG_ERR("Шина I2C не доступна");
        return -ENODEV;
    }
    
    LOG_INF("I2C шина: %s", config->i2c_bus.bus->name);
    LOG_INF("I2C адрес: 0x%02x", config->i2c_bus.addr);

    ret = tps43_reset_values(dev);
    if (ret != 0) {
        LOG_ERR("Ошибка сброса значений: %d", ret);
        return ret;
    }

    // GPIO сброс через hardware RST
    if (config->rst_gpio.port) {
        ret = gpio_pin_configure_dt(&config->rst_gpio, GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERR("Ошибка конфигурации RST GPIO: %d", ret);
            return ret;
        }
        
        gpio_pin_set_dt(&config->rst_gpio, 0);
        k_sleep(K_MSEC(10));
        gpio_pin_set_dt(&config->rst_gpio, 1);
        k_sleep(K_MSEC(610));
        
        LOG_INF("Аппаратный сброс завершен");
    }

    // проверка SHOW_RESET и конфигурация
    ret = check_reset_and_reconfigure(dev);
    if (ret != 0) {
        LOG_ERR("Ошибка конфигурации устройства: %d", ret);
        return ret;
    }

    // прерывания RDY настраиваем только ПОСЛЕ конфигурации устройства!
    if (config->rdy_gpio.port != NULL) {
        ret = gpio_pin_configure_dt(&config->rdy_gpio, GPIO_INPUT);
        if (ret != 0) {
            LOG_WRN("Ошибка конфигурации RDY GPIO: %d", ret);
        } else {
            ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, 
                                                    GPIO_INT_EDGE_TO_ACTIVE);
            if (ret == 0) {
                gpio_init_callback(&drv_data->rdy_cb, tps43_rdy_callback, 
                                    BIT(config->rdy_gpio.pin));
                ret = gpio_add_callback(config->rdy_gpio.port, &drv_data->rdy_cb);
                if (ret == 0) {
                    LOG_INF("Прерывание RDY сконфигурировано");
                } else {
                    LOG_WRN("Ошибка добавления callback RDY: %d", ret);
                }
            }
        }
    }

    drv_data->initialized = true;
    drv_data->suspended = false;

    // Инициализируем семафор для защиты операций I2C
    // Первый параметр - начальное количество (1 = доступен)
    // Второй параметр - максимальное количество (1 = бинарный семафор)
    k_sem_init(&drv_data->lock, 1, 1);

    k_work_init(&drv_data->work, tps43_work_handler);
    k_work_init_delayable(&drv_data->momentum_work, tps43_momentum_work_handler);
    
    LOG_INF("Драйвер TPS43 успешно инициализирован");
    return 0;
}

 
#define TPS43_INIT(inst)                                                                             \
    static struct tps43_drv_data tps43_##inst##_drvdata = {                                          \
        .device_ready = false,                                                                       \
        .initialized = false,                                                                        \
        .scroll_active = false,                                                                      \
        .drag_active = false,                                                                        \
        .suspended = false,                                                                          \
        .last_tap_time = 0,                                                                          \
        .prev_num_fingers = 0,                                                                       \
        .three_drag_active = false,                                                                  \
        .scroll_accum_x = 0,                                                                         \
        .scroll_accum_y = 0,                                                                         \
        .move_accum_x = 0,                                                                           \
        .move_accum_y = 0,                                                                           \
        .accel_speed_x16 = 0,                                                                        \
        .swipe_fired = false,                                                                        \
        .tf_active = false,                                                                          \
        .tf_start = 0,                                                                               \
        .tf_moved = false,                                                                           \
        .mom_velocity = 0,                                                                           \
        .mom_remainder = 0,                                                                          \
        .mom_last_ticks = 0,                                                                         \
        .mom_last_horizontal = false,                                                                \
        .mom_active = false,                                                                         \
        .mom_start_ms = 0,                                                                           \
        .zoom_accum = 0,                                                                             \
    };                                                                                               \
                                                                                                     \
    static const struct tps43_config tps43_##inst##_config = {                                       \
        .i2c_bus = I2C_DT_SPEC_INST_GET(inst),                                                       \
        .rdy_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, rdy_gpios, {0}),                                  \
        .rst_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, rst_gpios, {0}),                                  \
        .single_tap = DT_INST_PROP(inst, single_tap),                                                \
        .press_and_hold = DT_INST_PROP(inst, press_and_hold),                                        \
        .hold_time_ms = DT_INST_PROP_OR(inst, hold_time_ms, 300),                                    \
        .tap_drag = DT_INST_PROP(inst, tap_drag),                                                    \
        .tap_drag_window_ms = DT_INST_PROP_OR(inst, tap_drag_window_ms, 300),                        \
        .three_finger_drag = DT_INST_PROP(inst, three_finger_drag),                                  \
        .max_multitouches = DT_INST_PROP_OR(inst, max_multitouches, 0),                              \
        .two_finger_tap = DT_INST_PROP(inst, two_finger_tap),                                        \
        .scroll = DT_INST_PROP(inst, scroll),                                                        \
        .swipes = DT_INST_PROP(inst, swipes),                                                        \
        .invert_x = DT_INST_PROP(inst, invert_x),                                                    \
        .invert_y = DT_INST_PROP(inst, invert_y),                                                    \
        .switch_xy = DT_INST_PROP(inst, switch_xy),                                                  \
        .invert_scroll_x = DT_INST_PROP(inst, invert_scroll_x),                                      \
        .invert_scroll_y = DT_INST_PROP(inst, invert_scroll_y),                                      \
        .sensitivity = DT_INST_PROP_OR(inst, sensitivity, 100),                                      \
        .accel_max = DT_INST_PROP_OR(inst, accel_max, 0),                                            \
        .accel_lower_speed = DT_INST_PROP_OR(inst, accel_lower_speed, 4),                            \
        .accel_upper_speed = DT_INST_PROP_OR(inst, accel_upper_speed, 30),                           \
        .scroll_sensitivity = DT_INST_PROP_OR(inst, scroll_sensitivity, 50),                         \
        .scroll_init_distance = DT_INST_PROP_OR(inst, scroll_initial_distance, 0),                   \
        .scroll_steps_per_line = DT_INST_PROP_OR(inst, scroll_steps_per_line, 1),                    \
        .scroll_momentum = DT_INST_PROP(inst, scroll_momentum),                                      \
        .scroll_momentum_decay = DT_INST_PROP_OR(inst, scroll_momentum_decay, 88),                   \
        .scroll_momentum_interval_ms = DT_INST_PROP_OR(inst, scroll_momentum_interval_ms, 15),       \
        .scroll_momentum_min_ticks = DT_INST_PROP_OR(inst, scroll_momentum_min_ticks, 3),            \
        .scroll_momentum_max_ms = DT_INST_PROP_OR(inst, scroll_momentum_max_ms, 1200),               \
        .zoom = DT_INST_PROP(inst, zoom),                                                            \
        .zoom_step_px = DT_INST_PROP_OR(inst, zoom_step_px, 40),                                     \
        .invert_zoom = DT_INST_PROP(inst, invert_zoom),                                              \
        .ati_target = DT_INST_PROP_OR(inst, ati_target, 0),                                          \
        .ati_c = DT_INST_PROP_OR(inst, ati_c, -1),                                                   \
        .debug_report_registers = DT_INST_PROP(inst, debug_report_registers),                        \
        .finger_split = DT_INST_PROP_OR(inst, finger_split_factor, -1),                              \
        .enable_power_management = DT_INST_PROP_OR(inst, enable_power_management, true),             \
        .filter_settings = DT_INST_PROP_OR(inst, filter_settings, 0x0F),                             \
        .dyn_bottom_beta = DT_INST_PROP_OR(inst, dynamic_bottom_beta, -1),                           \
        .dyn_lower_speed = DT_INST_PROP_OR(inst, dynamic_lower_speed, -1),                           \
        .dyn_upper_speed = DT_INST_PROP_OR(inst, dynamic_upper_speed, -1),                           \
        .report_rate_active_ms = DT_INST_PROP_OR(inst, report_rate_active_ms, 0),                    \
        .touch_set_multiplier = DT_INST_PROP_OR(inst, touch_set_multiplier, -1),                     \
        .touch_clear_multiplier = DT_INST_PROP_OR(inst, touch_clear_multiplier, -1),                 \
        .touch_hysteresis = DT_INST_PROP_OR(inst, touch_hysteresis, -1),                             \
        .timeout_idle_s = DT_INST_PROP_OR(inst, timeout_idle_s, 0),                                  \
    };                                                                                               \
                                                                                                     \
    DEVICE_DT_INST_DEFINE(inst, tps43_init, NULL, &tps43_##inst##_drvdata, &tps43_##inst##_config,   \
                        POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);                              \
    BUILD_ASSERT(DT_INST_REG_ADDR(inst) == TPS43_I2C_ADDR, "Несоответствие адреса I2C");


DT_INST_FOREACH_STATUS_OKAY(TPS43_INIT)

/**
 * @brief Публичная функция для управления режимом сна тачпада
 * 
 * Эта функция используется системой управления питанием ZMK (через tps43_idle_sleeper)
 * для перевода тачпада в режим сна при переходе клавиатуры в состояние idle/sleep.
 * 
 * @param dev Указатель на устройство тачпада
 * @param sleep true - перевести в режим сна, false - пробудить
 * @return 0 при успехе, отрицательный код ошибки при неудаче
 */
int tps43_set_sleep(const struct device *dev, bool sleep) {
    if (dev == NULL) {
        return -EINVAL;
    }
    return tps43_set_suspend(dev, sleep);
}
