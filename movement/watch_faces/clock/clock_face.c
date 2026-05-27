/* SPDX-License-Identifier: MIT */

/*
 * MIT License
 *
 * Copyright © 2021-2023 Joey Castillo <joeycastillo@utexas.edu> <jose.castillo@gmail.com>
 * Copyright © 2022 David Keck <davidskeck@users.noreply.github.com>
 * Copyright © 2022 TheOnePerson <a.nebinger@web.de>
 * Copyright © 2023 Jeremy O'Brien <neutral@fastmail.com>
 * Copyright © 2023 Mikhail Svarichevsky <3@14.by>
 * Copyright © 2023 Wesley Aptekar-Cassels <me@wesleyac.com>
 * Copyright © 2024 Matheus Afonso Martins Moreira <matheus.a.m.moreira@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include "clock_face.h"
#include "watch.h"
#include "watch_utility.h"
#include "watch_private_display.h"

// 2.2 volts will happen when the battery has maybe 5-10% remaining?
// we can refine this later.
#ifndef CLOCK_FACE_LOW_BATTERY_VOLTAGE_THRESHOLD
#define CLOCK_FACE_LOW_BATTERY_VOLTAGE_THRESHOLD 2200
#endif

// Countdown cycle: 5 -> 10 -> 15 -> 30 -> 0 (cancel/off) -> 5 ...
static const uint8_t clock_countdown_cycle[] = { 5, 10, 15, 30, 0 };
#define CLOCK_COUNTDOWN_CYCLE_LEN (sizeof(clock_countdown_cycle) / sizeof(clock_countdown_cycle[0]))
#define CLOCK_COUNTDOWN_CANCEL_SECONDS 3    // how long to show "--" after a cancel
#define CLOCK_COUNTDOWN_ALARM_BEEPS 8       // ~8 seconds of buzzer when the timer fires

typedef struct {
    struct {
        watch_date_time previous;
    } date_time;
    uint8_t last_battery_check;
    uint8_t watch_face_index;
    bool time_signal_enabled;
    bool battery_low;
    struct {
        uint8_t cycle_index;       // index into clock_countdown_cycle of the *next* value to apply
        uint8_t minutes;           // 0 = inactive; otherwise the currently-running duration
        uint32_t target_ts;        // unix timestamp when the countdown fires
        uint32_t cancel_until_ts;  // 0 = no cancel banner; otherwise show "--" until this unix ts
    } countdown;
} clock_state_t;

static bool clock_is_in_24h_mode(movement_settings_t *settings) {
#ifdef CLOCK_FACE_24H_ONLY
    return true;
#else
    return settings->bit.clock_mode_24h;
#endif
}

static bool clock_should_set_leading_zero(movement_settings_t *settings) {
    return clock_is_in_24h_mode(settings) && settings->bit.clock_24h_leading_zero;
}

static void clock_indicate(WatchIndicatorSegment indicator, bool on) {
    if (on) {
        watch_set_indicator(indicator);
    } else {
        watch_clear_indicator(indicator);
    }
}

static void clock_indicate_alarm(movement_settings_t *settings) {
    clock_indicate(WATCH_INDICATOR_SIGNAL, settings->bit.alarm_enabled);
}

static void clock_indicate_time_signal(clock_state_t *clock) {
    clock_indicate(WATCH_INDICATOR_BELL, clock->time_signal_enabled);
}

static void clock_indicate_24h(movement_settings_t *settings) {
    clock_indicate(WATCH_INDICATOR_24H, clock_is_in_24h_mode(settings));
}

static bool clock_is_pm(watch_date_time date_time) {
    return date_time.unit.hour >= 12;
}

static void clock_indicate_pm(movement_settings_t *settings, watch_date_time date_time) {
    if (settings->bit.clock_mode_24h) { return; }
    clock_indicate(WATCH_INDICATOR_PM, clock_is_pm(date_time));
}

static void clock_indicate_low_available_power(clock_state_t *clock) {
    // Set the LAP indicator if battery power is low
    clock_indicate(WATCH_INDICATOR_LAP, clock->battery_low);
}

static watch_date_time clock_24h_to_12h(watch_date_time date_time) {
    date_time.unit.hour %= 12;

    if (date_time.unit.hour == 0) {
        date_time.unit.hour = 12;
    }

    return date_time;
}

static void clock_check_battery_periodically(clock_state_t *clock, watch_date_time date_time) {
    // check the battery voltage once a day
    if (date_time.unit.day == clock->last_battery_check) { return; }

    clock->last_battery_check = date_time.unit.day;

    watch_enable_adc();
    uint16_t voltage = watch_get_vcc_voltage();
    watch_disable_adc();

    clock->battery_low = voltage < CLOCK_FACE_LOW_BATTERY_VOLTAGE_THRESHOLD;

    clock_indicate_low_available_power(clock);
}

static void clock_toggle_time_signal(clock_state_t *clock) {
    clock->time_signal_enabled = !clock->time_signal_enabled;
    clock_indicate_time_signal(clock);
}

static void clock_display_all(watch_date_time date_time, bool leading_zero) {
    char buf[10 + 1];

    snprintf(
        buf,
        sizeof(buf),
        leading_zero? "%s%02d%02d%02d%02d" : "%s%2d%2d%02d%02d",
        watch_utility_get_weekday(date_time),
        date_time.unit.day,
        date_time.unit.hour,
        date_time.unit.minute,
        date_time.unit.second
    );

    watch_display_string(buf, 0);
}

static bool clock_display_some(watch_date_time current, watch_date_time previous) {
    if ((current.reg >> 6) == (previous.reg >> 6)) {
        // everything before seconds is the same, don't waste cycles setting those segments.

        watch_display_character_lp_seconds('0' + current.unit.second / 10, 8);
        watch_display_character_lp_seconds('0' + current.unit.second % 10, 9);

        return true;

    } else if ((current.reg >> 12) == (previous.reg >> 12)) {
        // everything before minutes is the same.

        char buf[4 + 1];

        snprintf(
            buf,
            sizeof(buf),
            "%02d%02d",
            current.unit.minute,
            current.unit.second
        );

        watch_display_string(buf, 6);

        return true;

    } else {
        // other stuff changed; let's do it all.
        return false;
    }
}

static void clock_display_clock(movement_settings_t *settings, clock_state_t *clock, watch_date_time current) {
    if (!clock_display_some(current, clock->date_time.previous)) {
        if (!clock_is_in_24h_mode(settings)) {
            // if we are in 12 hour mode, do some cleanup.
            clock_indicate_pm(settings, current);
            current = clock_24h_to_12h(current);
        }
        clock_display_all(current, clock_should_set_leading_zero(settings));
    }
}

static inline int32_t clock_tz_offset(movement_settings_t *settings) {
    return movement_timezone_offsets[settings->bit.time_zone] * 60;
}

static inline uint32_t clock_now_ts(movement_settings_t *settings) {
    return watch_utility_date_time_to_unix_time(watch_rtc_get_date_time(), clock_tz_offset(settings));
}

static void clock_countdown_overlay(clock_state_t *clock, watch_date_time current,
                                    movement_settings_t *settings) {
    if (clock->countdown.minutes != 0) {
        uint32_t now_ts = watch_utility_date_time_to_unix_time(current, clock_tz_offset(settings));
        uint32_t remaining_min;
        if (now_ts >= clock->countdown.target_ts) {
            remaining_min = 0;
        } else {
            // Round up so we show "1" during the final minute instead of "0".
            remaining_min = (clock->countdown.target_ts - now_ts + 59) / 60;
        }
        char buf[3];
        snprintf(buf, sizeof(buf), "%2lu", (unsigned long) remaining_min);
        watch_display_string(buf, 2);
        // Per timer_face's convention: blink the BELL indicator each second to
        // signal "countdown active", while keeping the digits stable and readable.
        clock_indicate(WATCH_INDICATOR_BELL, (current.unit.second % 2) == 0);
    } else if (clock->countdown.cancel_until_ts != 0) {
        // Position 2 (day-tens) shares top/middle/bottom segments on the F-91W LCD,
        // so a "-" there lights up as three bars. Use blank-then-dash instead.
        watch_display_string(" -", 2);
    }
}

static void clock_countdown_start(clock_state_t *clock, movement_settings_t *settings, uint8_t minutes) {
    clock->countdown.minutes = minutes;
    clock->countdown.cancel_until_ts = 0;
    uint32_t now_ts = clock_now_ts(settings);
    clock->countdown.target_ts = watch_utility_offset_timestamp(now_ts, 0, minutes, 0);
    watch_date_time target_dt = watch_utility_date_time_from_unix_time(clock->countdown.target_ts, clock_tz_offset(settings));
    movement_schedule_background_task_for_face(clock->watch_face_index, target_dt);
}

static void clock_countdown_cancel(clock_state_t *clock, movement_settings_t *settings) {
    clock->countdown.minutes = 0;
    clock->countdown.target_ts = 0;
    clock->countdown.cancel_until_ts = clock_now_ts(settings) + CLOCK_COUNTDOWN_CANCEL_SECONDS;
    movement_cancel_background_task_for_face(clock->watch_face_index);
    // Restore the BELL indicator to reflect the hourly-chime setting.
    clock_indicate_time_signal(clock);
    // Force a full redraw so the day position can be overlaid with "--".
    clock->date_time.previous.reg = 0xFFFFFFFF;
}

static void clock_countdown_advance(clock_state_t *clock, movement_settings_t *settings) {
    uint8_t next = clock_countdown_cycle[clock->countdown.cycle_index];
    clock->countdown.cycle_index = (clock->countdown.cycle_index + 1) % CLOCK_COUNTDOWN_CYCLE_LEN;

    if (next == 0) {
        clock_countdown_cancel(clock, settings);
    } else {
        clock_countdown_start(clock, settings, next);
    }
}

static void clock_display_low_energy(watch_date_time date_time) {
    char buf[10 + 1];

    snprintf(
        buf,
        sizeof(buf),
        "%s%2d%2d%02d  ",
        watch_utility_get_weekday(date_time),
        date_time.unit.day,
        date_time.unit.hour,
        date_time.unit.minute
    );

    watch_display_string(buf, 0);
}

static void clock_start_tick_tock_animation(void) {
    if (!watch_tick_animation_is_running()) {
        watch_start_tick_animation(500);
    }
}

static void clock_stop_tick_tock_animation(void) {
    if (watch_tick_animation_is_running()) {
        watch_stop_tick_animation();
    }
}

void clock_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void ** context_ptr) {
    (void) settings;
    (void) watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(clock_state_t));
        clock_state_t *state = (clock_state_t *) *context_ptr;
        state->time_signal_enabled = false;
        state->watch_face_index = watch_face_index;
        state->countdown.cycle_index = 0;
        state->countdown.minutes = 0;
        state->countdown.target_ts = 0;
        state->countdown.cancel_until_ts = 0;
    }
}

void clock_face_activate(movement_settings_t *settings, void *context) {
    clock_state_t *clock = (clock_state_t *) context;

    clock_stop_tick_tock_animation();

    clock_indicate_time_signal(clock);
    clock_indicate_alarm(settings);
    clock_indicate_24h(settings);

    watch_set_colon();

    // this ensures that none of the timestamp fields will match, so we can re-render them all.
    clock->date_time.previous.reg = 0xFFFFFFFF;
}

bool clock_face_loop(movement_event_t event, movement_settings_t *settings, void *context) {
    clock_state_t *state = (clock_state_t *) context;
    watch_date_time current;

    switch (event.event_type) {
        case EVENT_LOW_ENERGY_UPDATE:
            clock_start_tick_tock_animation();
            current = watch_rtc_get_date_time();
            clock_display_low_energy(current);
            clock_countdown_overlay(state, current, settings);
            break;
        case EVENT_TICK:
        case EVENT_ACTIVATE:
            current = watch_rtc_get_date_time();

            // Expire the post-cancel "--" banner so the date can come back.
            if (state->countdown.cancel_until_ts != 0) {
                uint32_t now_ts = watch_utility_date_time_to_unix_time(current, clock_tz_offset(settings));
                if (now_ts >= state->countdown.cancel_until_ts) {
                    state->countdown.cancel_until_ts = 0;
                    state->date_time.previous.reg = 0xFFFFFFFF;
                }
            }

            clock_display_clock(settings, state, current);
            clock_countdown_overlay(state, current, settings);

            clock_check_battery_periodically(state, current);

            state->date_time.previous = current;

            break;
        case EVENT_ALARM_BUTTON_UP:
            clock_countdown_advance(state, settings);
            // Redraw immediately so the new countdown value (or "--" banner) appears.
            current = watch_rtc_get_date_time();
            clock_display_clock(settings, state, current);
            clock_countdown_overlay(state, current, settings);
            state->date_time.previous = current;
            break;
        case EVENT_ALARM_LONG_PRESS:
            clock_toggle_time_signal(state);
            break;
        case EVENT_BACKGROUND_TASK:
            if (state->countdown.minutes != 0) {
                uint32_t now_ts = watch_utility_date_time_to_unix_time(watch_rtc_get_date_time(), clock_tz_offset(settings));
                if (now_ts >= state->countdown.target_ts) {
                    movement_play_alarm_beeps(CLOCK_COUNTDOWN_ALARM_BEEPS, BUZZER_NOTE_C8);
                    state->countdown.minutes = 0;
                    state->countdown.target_ts = 0;
                    state->countdown.cycle_index = 0;
                    state->countdown.cancel_until_ts = 0;
                    clock_indicate_time_signal(state);
                    state->date_time.previous.reg = 0xFFFFFFFF;
                    break;
                }
            }
            // uncomment this line to snap back to the clock face when the hour signal sounds:
            // movement_move_to_face(state->watch_face_index);
            movement_play_signal();
            break;
        default:
            return movement_default_loop_handler(event, settings);
    }

    return true;
}

void clock_face_resign(movement_settings_t *settings, void *context) {
    (void) settings;
    (void) context;
}

bool clock_face_wants_background_task(movement_settings_t *settings, void *context) {
    (void) settings;
    clock_state_t *state = (clock_state_t *) context;
    if (!state->time_signal_enabled) return false;

    watch_date_time date_time = watch_rtc_get_date_time();

    return date_time.unit.minute == 0;
}
