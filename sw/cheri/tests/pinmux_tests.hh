// Copyright lowRISC Contributors.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "../../common/defs.h"
#include "../common/console.hh"
#include "../common/flash-utils.hh"
#include "../common/timer-utils.hh"
#include "../common/uart-utils.hh"
#include "../common/sonata-devices.hh"
#include "../common/platform-pinmux.hh"
#include "../common/block_tests.hh"
#include "test_runner.hh"
#include "i2c_tests.hh"
#include <cheri.hh>
#include <platform-uart.hh>
#include <ds/xoroshiro.h>

using namespace CHERI;

/**
 * Configures the number of test iterations to perform.
 * This can be overriden via a compilation flag.
 */
#ifndef PINMUX_TEST_ITERATIONS
#define PINMUX_TEST_ITERATIONS (1U)
#endif

/**
 * Configures whether cable connections required for the pinmux testing
 * are available. This includes cables between:
 *  - mikroBus       (P7) RX & TX
 *  - Arduino Shield (P4) D0 & D1
 *  - Arduino Shield (P4) D8 & D9
 * This can be overriden via a compilation flag.
 */
#ifndef PINMUX_CABLE_CONNECTIONS_AVAILABLE
#define PINMUX_CABLE_CONNECTIONS_AVAILABLE true
#endif

/**
 * Configures whether a Digilent PMOD SF3 is attached to the PMOD1
 * connector as is required for Pinmux SPI testing.
 * Ths can be overriden via a compilation flag.
 */
#ifndef PINMUX_PMOD1_PMODSF3_AVAILABLE
#define PINMUX_PMOD1_PMODSF3_AVAILABLE true
#endif

// Testing parameters
static constexpr uint32_t UartTimeoutUsec = 24;  // with 921,600 bps, this is > 25 bit times.
static constexpr uint32_t UartTestBytes   = 100;
static constexpr uint32_t GpioWaitUsec    = 20;  // short wire bridge between FGPA pins.
static constexpr uint32_t GpioTestLength  = 10;

static constexpr uint8_t PmxToDisabled = 0;
static constexpr uint8_t PmxToDefault  = 1;

/**
 * Test pinmux by enabling and disabling the UART1 TX pin output and UART1 RX
 * block input. Tests the UART itself by sending and receiving some data over
 * UART1; it is required that UART1 TX and RX are manually connected on Sonata
 * for this test (mikroBus P7 RX & TX).
 *
 * Tests UART1 normally, then disables TX and checks the test fails, then
 * disables RX and checks the test fails, and then re-enables the pins and
 * repeats the test, checking that it succeeds.
 *
 * Returns the number of failures during the test.
 */
static int pinmux_uart_test(SonataPinmux *pinmux, ds::xoroshiro::P32R8 &prng, UartPtr uart1) {
  constexpr uint8_t PmxMikroBusUartTransmitToUartTx1 = 1;
  constexpr uint8_t PmxUartReceive1ToMb8             = 4;

  int failures = 0;

  // Mux UART1 over mikroBus P7 RX & TX via default.
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::mb7, PmxMikroBusUartTransmitToUartTx1);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::uart_1_rx, PmxUartReceive1ToMb8);

  // Check that messages are sent and received via UART1
  failures += !uart_send_receive_test(prng, uart1, UartTimeoutUsec, UartTestBytes);

  // Disable UART1 TX through pinmux, and check the test now fails (no TX sent)
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::mb7, PmxToDisabled);
  failures += uart_send_receive_test(prng, uart1, UartTimeoutUsec, UartTestBytes);

  // Re-enable UART1 TX and disable UART1 RX through pinmux, and check that the test
  // still fails (no RX received)
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::mb7, PmxMikroBusUartTransmitToUartTx1);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::uart_1_rx, PmxToDisabled);
  failures += uart_send_receive_test(prng, uart1, UartTimeoutUsec, UartTestBytes);

  // Re-enable UART1 RX and check the test now passes again
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::uart_1_rx, PmxUartReceive1ToMb8);
  failures += !uart_send_receive_test(prng, uart1, UartTimeoutUsec, UartTestBytes);

  // Reset muxed pins to not interfere with future tests
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::mb7, PmxToDefault);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::uart_1_rx, PmxToDefault);

  return failures;
}

static void reset_spi_flash(SpiPtr spi) {
  spi->cs = (spi->cs & ~1u);  // Enable CS
  spi->blocking_write(&CmdEnableReset, 1);
  spi->wait_idle();
  spi->cs = (spi->cs | 1u);  // Disable CS

  spi->cs = (spi->cs & ~1u);  // Enable CS
  spi->blocking_write(&CmdReset, 1);
  spi->wait_idle();
  spi->cs = (spi->cs | 1u);  // Disable CS

  // Need to wait at least 30us for the reset to complete.
  wait_mcycle(2000);
}

/**
 * Test pinmux by enabling and disabling the SPI1 pins, which are used to communicate with a connected
 * PMOD SF3 Spi Flash (n25q256a) device. First reads the flash's JEDEC ID like normal, then disables
 * the pins via pinmux and repeats the JEDEC ID read, checking that it fails. Then re-enables the pins
 * via pinmux and repeats the JEDEC ID read, checking that it succeeds.
 * Returns the number of failures during the test.
 */
static int pinmux_spi_flash_test(SonataPinmux *pinmux, SpiPtr spi) {
  constexpr uint8_t PmxPmod1Io1ToSpi2Cs   = 2;
  constexpr uint8_t PmxPmod1Io2ToSpi2Copi = 2;
  constexpr uint8_t PmxPmod1Io4ToSpi2Sclk = 2;
  constexpr uint8_t PmxSpi2CipoToPmod1Io3 = 3;

  int failures = 0;

  // Ensure the SPI Flash pins are enabled using Pinmux
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_1, PmxPmod1Io1ToSpi2Cs);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::spi_2_cipo, PmxSpi2CipoToPmod1Io3);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_2, PmxPmod1Io2ToSpi2Copi);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_4, PmxPmod1Io4ToSpi2Sclk);

  // Configure the SPI to be MSB-first.
  spi->wait_idle();
  spi->init(false, false, true, 0);
  reset_spi_flash(spi);

  // Run the normal SPI Flash JEDEC ID Test; it should pass.
  failures += !spi_n25q256a_read_jedec_id(spi);

  // Disable the SPI Flash pins through pinmux
  spi->wait_idle();
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_1, PmxToDisabled);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::spi_2_cipo, PmxToDisabled);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_2, PmxToDisabled);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_4, PmxToDisabled);
  reset_spi_flash(spi);

  // Run the JEDEC ID Test again; we expect it to fail.
  failures += spi_n25q256a_read_jedec_id(spi);

  // Re-enable the SPI Flash pins through pinmux
  spi->wait_idle();
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_1, PmxPmod1Io1ToSpi2Cs);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::spi_2_cipo, PmxSpi2CipoToPmod1Io3);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_2, PmxPmod1Io2ToSpi2Copi);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_4, PmxPmod1Io4ToSpi2Sclk);
  reset_spi_flash(spi);

  // Run the JEDEC ID Test one more time; it should pass.
  failures += !spi_n25q256a_read_jedec_id(spi);

  // Disable specifically the Chip Select through Pinmux.
  spi->wait_idle();
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_1, PmxToDisabled);
  reset_spi_flash(spi);

  // Run the JEDEC ID Test again; we expect it to fail.
  failures += spi_n25q256a_read_jedec_id(spi);

  // Re-enable the Chip Select through Pinmux.
  spi->wait_idle();
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_1, PmxPmod1Io1ToSpi2Cs);
  reset_spi_flash(spi);

  // Run the JEDEC ID Test again; it should pass.
  failures += !spi_n25q256a_read_jedec_id(spi);

  // Reset muxed pins to not interfere with future tests
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_1, PmxToDefault);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::spi_2_cipo, PmxToDefault);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_2, PmxToDefault);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::pmod1_4, PmxToDefault);

  return failures;
}

/**
 * Test pinmux by enabling and disabling the I2C pins for the Rapberry Pi Sense
 * HAT. This requires the RPi Sense HAT to be connected to the Sonata board, otherwise the
 * test will fail.
 *
 * Runs the I2C Rpi Hat ID EEPROM and WHO_AM_I tests, enabling and disabling the
 * output pins via pinmux and checking that the respective tests fail/succeed
 * as expected according to our Pinmux configuration.
 *
 * Returns the number of failures during the test.
 */
static int pinmux_i2c_test(SonataPinmux *pinmux, I2cPtr i2c0, I2cPtr i2c1) {
  constexpr uint8_t PmxRPiHat27ToI2cSda0 = 1;
  constexpr uint8_t PmxRPiHat28ToI2cScl0 = 1;
  constexpr uint8_t PmxRPiHat3ToI2cSda1  = 1;
  constexpr uint8_t PmxRPiHat5ToI2cScl1  = 1;

  int failures = 0;

  // Ensure the RPI Hat I2C pins are enabled via Pinmux
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g0, PmxRPiHat27ToI2cSda0);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g1, PmxRPiHat28ToI2cScl0);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g2_sda, PmxRPiHat3ToI2cSda1);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g3_scl, PmxRPiHat5ToI2cScl1);

  // Run the normal I2C RPI Hat ID_EEPROM and WHO_AM_I tests
  failures += i2c_rpi_hat_id_eeprom_test(i2c0);
  failures += i2c_rpi_hat_imu_whoami_test(i2c1);

  // Disable the RPI Hat I2C0 output pins, and check that the ID EEPROM test
  // now fails (and the WHOAMI test still succeeds).
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g0, PmxToDisabled);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g1, PmxToDisabled);
  failures += (i2c_rpi_hat_id_eeprom_test(i2c0) == 0);
  failures += i2c_rpi_hat_imu_whoami_test(i2c1);

  // Re-enables the RPI Hat I2C0 pins and disables the I2C1 pins, and check that the
  // ID EEPROM test now passes. and the WHOAMI test now fails.
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g0, PmxRPiHat27ToI2cSda0);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g1, PmxRPiHat28ToI2cScl0);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g2_sda, PmxToDisabled);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g3_scl, PmxToDisabled);
  reset_i2c_controller(i2c0);
  failures += i2c_rpi_hat_id_eeprom_test(i2c0);
  failures += (i2c_rpi_hat_imu_whoami_test(i2c1) == 0);

  // Re-enables both the RPI Hat I2C0 and I2C1 pins via pinmux, and checks that both
  // tests now pass again.
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g2_sda, PmxRPiHat3ToI2cSda1);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g3_scl, PmxRPiHat5ToI2cScl1);
  reset_i2c_controller(i2c1);
  failures += i2c_rpi_hat_id_eeprom_test(i2c0);
  failures += i2c_rpi_hat_imu_whoami_test(i2c1);

  // Reset muxed pins to not interfere with future tests
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g0, PmxToDefault);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g1, PmxToDefault);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g2_sda, PmxToDefault);
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::rph_g3_scl, PmxToDefault);

  return failures;
}

/**
 * Test pinmux by enabling and disabling GPIO pins on the Arduino Shield header. This
 * requires that there is a cable connecting pins D8 and D9 on the Arduino Shield
 * header of the Sonata board. This tests writing to and reading from GPIO pins,
 * checking that the respective tests fail/succeed as expected according to our
 * pinmux configuration, enabling and disabling these GPIO via pinmux.
 *
 * Returns the number of failures durign the test.
 */
static int pinmux_gpio_test(SonataPinmux *pinmux, SonataGpioFull *gpio) {
  constexpr uint8_t PmxArduinoD8ToGpios_1_8   = 1;
  constexpr uint8_t PmxArduinoGpio9ToAhTmpio9 = 1;

  constexpr GpioPin GpioPinInput  = {GpioInstance::ArduinoShield, 9};
  constexpr GpioPin GpioPinOutput = {GpioInstance::ArduinoShield, 8};

  int failures = 0;

  // Configure the Arduino D9 GPIO as input and D8 as output.
  set_gpio_output_enable(gpio, GpioPinOutput, true);
  set_gpio_output_enable(gpio, GpioPinInput, false);

  // Ensure the GPIO (Arduino Shield D8 & D9) are enabled via Pinmux
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio8, PmxArduinoD8ToGpios_1_8);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::gpio_1_ios_9, PmxArduinoGpio9ToAhTmpio9);

  // Check that reading & writing from/to GPIO works as expected.
  failures += !gpio_write_read_test(gpio, GpioPinOutput, GpioPinInput, GpioWaitUsec, GpioTestLength);

  // Disable the GPIO via pinmux, and check that the test now fails.
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio8, PmxToDisabled);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::gpio_1_ios_9, PmxToDisabled);
  failures += gpio_write_read_test(gpio, GpioPinOutput, GpioPinInput, GpioWaitUsec, GpioTestLength);

  // Re-enable the GPIO via pinmux, and check that the test passes once more
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio8, PmxArduinoD8ToGpios_1_8);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::gpio_1_ios_9, PmxArduinoGpio9ToAhTmpio9);
  failures += !gpio_write_read_test(gpio, GpioPinOutput, GpioPinInput, GpioWaitUsec, GpioTestLength);

  // Reset muxed pins to not interfere with future tests
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio8, PmxToDefault);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::gpio_1_ios_9, PmxToDefault);

  return failures;
}

/**
 * Test the muxing capability of pinmux, by dynamically switching between using
 * (and testing) UART and pinmux on the same two pins - specifically the Arduino
 * Shield header D0 and D1. It is required that these two pins are manually
 * connected on the Sonata board for this test.
 *
 * This tests that when muxed to UART, the corresponding UART send/receive test
 * works (and GPIO does not), and when muxed to GPIO, the corresponding GPIO
 * write/read test works (and UART does not).
 *
 * Returns the number of failures during the test.
 */
static int pinmux_mux_test(SonataPinmux *pinmux, ds::xoroshiro::P32R8 &prng, UartPtr uart1, SonataGpioFull *gpio) {
  constexpr uint8_t PmxArduinoD1ToUartTx1     = 1;
  constexpr uint8_t PmxArduinoD1ToGpio_1_1    = 2;
  constexpr uint8_t PmxUartReceive1ToAhTmpio0 = 3;
  constexpr uint8_t PmxArduinoGpio0ToAhTmpio0 = 1;

  constexpr GpioPin GpioPinInput  = {GpioInstance::ArduinoShield, 0};
  constexpr GpioPin GpioPinOutput = {GpioInstance::ArduinoShield, 1};

  int failures = 0;

  // Set the Arduino GPIO D0 as input and D1 as output.
  set_gpio_output_enable(gpio, GpioPinOutput, true);
  set_gpio_output_enable(gpio, GpioPinInput, false);

  // Mux UART1 over Arduino Shield D0 (RX) & D1 (TX)
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio1, PmxArduinoD1ToUartTx1);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::uart_1_rx, PmxUartReceive1ToAhTmpio0);

  // Test that UART1 works over the muxed Arduino Shield D0 & D1 pins,
  // and that GPIO does not work, as these pins are not muxed for GPIO.
  failures += !uart_send_receive_test(prng, uart1, UartTimeoutUsec, UartTestBytes);
  failures += gpio_write_read_test(gpio, GpioPinOutput, GpioPinInput, GpioWaitUsec, GpioTestLength);

  // Mux GPIO over Arduino Shield D0 (GPIO input) & D1 (GPIO output)
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio1, PmxArduinoD1ToGpio_1_1);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::gpio_1_ios_0, PmxArduinoGpio0ToAhTmpio0);

  // Test that UART1 no longer works (no longer muxed over D0 & D1),
  // and that our muxed GPIO now works.
  failures += uart_send_receive_test(prng, uart1, UartTimeoutUsec, UartTestBytes);
  failures += !gpio_write_read_test(gpio, GpioPinOutput, GpioPinInput, GpioWaitUsec, GpioTestLength);

  // Mux back to UART1 again, and test that UART again passes and GPIO fails.
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio1, PmxArduinoD1ToUartTx1);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::uart_1_rx, PmxUartReceive1ToAhTmpio0);
  failures += !uart_send_receive_test(prng, uart1, UartTimeoutUsec, UartTestBytes);
  failures += gpio_write_read_test(gpio, GpioPinOutput, GpioPinInput, GpioWaitUsec, GpioTestLength);

  // Reset muxed pins to not interfere with future tests
  failures += !pinmux->output_pin_select(SonataPinmux::OutputPin::ah_tmpio1, PmxToDefault);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::uart_1_rx, PmxToDefault);
  failures += !pinmux->block_input_select(SonataPinmux::BlockInput::gpio_1_ios_0, PmxToDefault);

  return failures;
}

/**
 * Run the whole suite of pinmux tests.
 */
void pinmux_tests(CapRoot root, Log &log) {
  // Create a bounded capability for pinmux & initialise the driver
  Capability<volatile uint8_t> pinmux = root.cast<volatile uint8_t>();
  pinmux.address()                    = PINMUX_ADDRESS;
  pinmux.bounds()                     = PINMUX_BOUNDS;
  SonataPinmux Pinmux                 = SonataPinmux(pinmux);

  // Create bounded capabilities for other devices, to be used in testing.
  SpiPtr spi = spi_ptr(root, 4);

  UartPtr uart1 = uart_ptr(root, 1);

  I2cPtr i2c0 = i2c_ptr(root, 0);
  I2cPtr i2c1 = i2c_ptr(root, 1);

  // Create bounded capabilities for the full range of GPIO
  SonataGpioFull gpio_full = get_full_gpio_ptrs(root);

  // Initialise PRNG for use to create (pseudo-)random test data.
  ds::xoroshiro::P32R8 prng;
  prng.set_state(0xDEAD, 0xBEEF);

  // Execute the specified number of iterations of each test.
  for (size_t i = 0; i < PINMUX_TEST_ITERATIONS; i++) {
    log.println("\r\nrunning pinmux_test: {} \\ {}", i, PINMUX_TEST_ITERATIONS - 1);
    set_console_mode(log, CC_PURPLE);
    log.println("(may need manual pin connections to pass)");
    set_console_mode(log, CC_RESET);

    bool test_failed = false;
    int failures     = 0;

    if (PINMUX_PMOD1_PMODSF3_AVAILABLE) {
      log.print("  Running SPI Flash Pinmux test... ");
      failures = pinmux_spi_flash_test(&Pinmux, spi);
      test_failed |= (failures > 0);
      write_test_result(log, failures);
    }

    if (I2C_RPI_HAT_AVAILABLE) {
      log.print("  Running I2C Pinmux test... ");
      failures = pinmux_i2c_test(&Pinmux, i2c0, i2c1);
      test_failed |= (failures > 0);
      write_test_result(log, failures);
    }

    if (PINMUX_CABLE_CONNECTIONS_AVAILABLE) {
      log.print("  Running GPIO Pinmux test... ");
      failures = pinmux_gpio_test(&Pinmux, &gpio_full);
      test_failed |= (failures > 0);
      write_test_result(log, failures);

      log.print("  Running UART Pinmux test... ");
      failures = pinmux_uart_test(&Pinmux, prng, uart1);
      test_failed |= (failures > 0);
      write_test_result(log, failures);

      log.print("  Running Mux test... ");
      failures = pinmux_mux_test(&Pinmux, prng, uart1, &gpio_full);
      test_failed |= (failures > 0);
      write_test_result(log, failures);
    }

    check_result(log, !test_failed);
  }
}
