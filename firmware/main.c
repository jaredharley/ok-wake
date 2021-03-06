/*
 * OK Wake
 * https://github.com/sowbug/ok-wake/
 *
 * Copyright (c) 2012 Mike Tsao.
 */

#include "attinyx5.h"
#include "bcd.h"
#include "eeprom.h"
#include "i2cmaster.h"

#include <avr/io.h>
#include <avr/sleep.h>
#include <inttypes.h>
#include <util/delay.h>

enum {
  STATE_INIT = 0,
  STATE_NIGHT,
  STATE_TWILIGHT,
  STATE_DAWN,
};

// How long before wake time we should start blinking lights.
const uint8_t TWILIGHT_MINUTES = 30;

// How long after wake time we should keep blinking lights.
const uint8_t DAWN_MINUTES = 30;

// The time when it's OK for kid to get up
uint16_t wake_time_bcd;

// The current time
uint16_t time_bcd;

// How many seconds before next blink
uint8_t twilight_counter_top;

// How long until halving counter_top
uint8_t twilight_next_minute_level;

// Side effect: clears value.
static uint8_t _was_button_pressed;
uint8_t was_button_pressed() {
  uint8_t result = _was_button_pressed;
  _was_button_pressed = 0;
  return result;
}

static void flicker_leds(uint8_t count) {
  while (count--) {
    quiet_on();
    _delay_ms(25);
    wake_on();
    _delay_ms(25);
  }

  leds_off();
}

static void flicker_quiet(uint8_t count) {
  while (count--) {
    quiet_on();
    _delay_ms(5);
    leds_off();
    _delay_ms(45);
  }
}

static void flicker_wake(uint8_t count) {
  while (count--) {
    wake_on();
    _delay_ms(5);
    leds_off();
    _delay_ms(45);
  }
}

static void breathe_wake() {
  uint8_t cycles = 2;
  uint8_t phase = 0;
  int8_t phase_direction = 4;

  while (cycles) {
    uint8_t sweep = 0;

    while (--sweep) {
      if (sweep < phase) {
        wake_on();
      } else {
        leds_off();
      }
    }

    phase += phase_direction;

    if (phase == 0) {
      phase_direction = -phase_direction;
      cycles--;
    }
  }

  leds_off();
}

static uint8_t maybe_set_rtc_time() {
  if (!eeprom_read_byte(&kShouldSetRTC)) {
    return 0;
  }

  // Update RTC.
  uint8_t registers[7];
  eeprom_read_block(registers, &kRTCTimeCircuits, 7);
  set_rtc_time(registers);

  // Mark the time set.
  eeprom_update_byte(&kShouldSetRTC, 0);
  return 1;
}

static void read_wake_time() {
  wake_time_bcd = eeprom_read_word(&kWakeTime);
}

static void set_alarm() {
  set_rtc_alarm(add_minutes_to_bcd_time(wake_time_bcd, -TWILIGHT_MINUTES));
}

// The idea is you push the button at 6pm, and it'll set the clock for a
// wakeup time of 6am.
static void set_wake_time() {
  wake_time_bcd = add_minutes_to_bcd_time(time_bcd, MINUTES_IN_HALF_DAY);
  eeprom_update_word(&kWakeTime, wake_time_bcd);
  set_alarm();
  flicker_leds(1);
}

static uint8_t start_NIGHT() {
  set_second_interrupt(0);
  return STATE_NIGHT;
}

static uint8_t start_TWILIGHT() {
  set_second_interrupt(1);
  twilight_counter_top = 10;
  twilight_next_minute_level = TWILIGHT_MINUTES >> 1;
  return STATE_TWILIGHT;
}

static uint8_t start_DAWN() {
  set_second_interrupt(1);
  return STATE_DAWN;
}

static void power_down() {
    leds_off();
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    enable_pin_interrupts(1);

    // Go to sleep.
    sleep_mode();

    // We've woken up.
    enable_pin_interrupts(0);
    i2c_init();

    // If we woke up and the '8523's SF and AF bits are clear, then it's
    // a safe guess the button was pressed (as opposed to the '8523's
    // /INT1 going active).
    _was_button_pressed = !clear_rtc_interrupt_flags();
}

static void handle_NIGHT() {
}

static void do_twilight_blink(int16_t minutes_left) {
  if (minutes_left <= 1) {
    flicker_quiet(1);
    flicker_wake(1);
  } else if (minutes_left <= 2) {
    flicker_quiet(2);
    flicker_wake(1);
  } else {
    flicker_quiet(1);
  }
}

static void handle_TWILIGHT(int16_t minutes_left) {
  static uint8_t counter;

  while (minutes_left < twilight_next_minute_level &&
         twilight_counter_top > 1) {
    twilight_next_minute_level >>= 1;
    twilight_counter_top >>= 1;
  }

  if (!counter--) {
    do_twilight_blink(minutes_left);
    counter = twilight_counter_top;
  }
}

static void handle_DAWN(int16_t minutes_elapsed) {
  if (minutes_elapsed < 3) {
    breathe_wake();
  } else {
    flicker_wake(1);
  }
}

static void do_i2c_diagnostics() {
  while (!is_rtc_connected()) {
    flicker_leds(2);
    _delay_ms(500);
  }

  while (!rtc_write_verify()) {
    flicker_quiet(2);
    _delay_ms(500);
  }

  breathe_wake();
}

static void init_system() {
  sei();
  init_ports();
  init_power_reduction_register();
  i2c_init();
  reset_rtc();
  stop_32768_clkout();
  do_i2c_diagnostics();

  if (maybe_set_rtc_time()) {
    flicker_leds(8);
  }

  refresh_time(&time_bcd);
  read_wake_time();
  set_alarm();
}

static uint8_t do_state_work(uint8_t state) {
  int16_t minutes_until_wake = smart_time_until_alarm(time_bcd, wake_time_bcd);
  uint8_t is_twilight = (minutes_until_wake <= TWILIGHT_MINUTES &&
                         minutes_until_wake > 0);
  uint8_t is_dawn = (minutes_until_wake > -DAWN_MINUTES &&
                     minutes_until_wake <= 0);

  if (minutes_until_wake < 0) {
    minutes_until_wake = -minutes_until_wake;
  }

  if (!is_twilight && !is_dawn && state != STATE_NIGHT) {
    state = start_NIGHT();
  }

  if (is_twilight && state != STATE_TWILIGHT) {
    state = start_TWILIGHT();
  }

  if (is_dawn && state != STATE_DAWN) {
    state = start_DAWN();
  }

  switch (state) {
  case STATE_NIGHT:
    handle_NIGHT();
    break;

  case STATE_TWILIGHT:
    handle_TWILIGHT(minutes_until_wake);
    break;

  case STATE_DAWN:
    handle_DAWN(minutes_until_wake);
    break;
  }

  return state;
}

int main(void) {
  init_system();
  uint8_t state = STATE_INIT;

  while (1) {
    if (was_button_pressed()) {
      set_wake_time();
      continue;
    }

    state = do_state_work(state);

    // We skip sleeping if the RTC is already trying to wake us up.
    if (!clear_rtc_interrupt_flags()) {
      // Wait for user to release button.
      while (is_button_pressed())
        ;

      // Power down, and we'll be woken up by /INT. This function will
      // return when we wake up again.
      power_down();

      // Now wait once again for any rising edge to pass.
      while (is_button_pressed())
        ;

      // We're awake and ready to go. Get the correct time copied locally.
      refresh_time(&time_bcd);
    }
  }
}
