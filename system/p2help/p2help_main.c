/****************************************************************************
 * apps/system/p2help/p2help_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;

  printf("P2SHOWCASE:BOARD=%s:PROFILE=showcase\n", CONFIG_ARCH_BOARD);
#ifdef CONFIG_ARCH_BOARD_P2_EC32MB
  printf("Module: P2-EC32MB Rev B; LEDs P38/P39; PSRAM 32 MiB\n");
#elif defined(CONFIG_ARCH_BOARD_P2_EC)
  printf("Module: P2-EC Rev D; LEDs P56/P57; no onboard PSRAM\n");
#endif

  printf("\nDevices\n");
#ifdef CONFIG_USERLED_LOWER
  printf("  /dev/userleds  two active-high buffered LEDs (LED switch ON)\n");
#endif
#ifdef CONFIG_P2_EC32MB_GPIO
  printf("  /dev/gpio0     P0 output; /dev/gpio1 P1 input\n");
#endif
#ifdef CONFIG_P2_EC32MB_UART1
  printf("  /dev/ttyS1     P2 TX / P3 RX, 115200 baud\n");
#endif
#ifdef CONFIG_P2_EC32MB_PWM
  printf("  /dev/pwm0      P4 PWM output\n");
#endif
#ifdef CONFIG_P2_EC32MB_CAPTURE
  printf("  /dev/cap0      P5 period/duty/edge capture\n");
#endif
#ifdef CONFIG_P2_EC32MB_DAC
  printf("  /dev/dac0      P4 raw 16-bit dithered DAC\n");
#endif
#ifdef CONFIG_P2_EC32MB_ADC
  printf("  /dev/adc0      P5 raw uncalibrated SINC2 ADC\n");
#endif
#ifdef CONFIG_P2_EC32MB_SPI
  printf("  /dev/spi0      P6 MOSI / P7 MISO / P8 SCK / P9 CS\n");
#endif
#ifdef CONFIG_P2_EC32MB_I2C
  printf("  /dev/i2c0      P24 SDA / P25 SCL, open drain\n");
#endif
#ifdef CONFIG_P2_EC32MB_BMP180
  printf("  /dev/press0    optional BMP180 at I2C address 0x77\n");
#endif
#ifdef CONFIG_MTD_SMART
  printf("  /dev/smart0    protected onboard-flash data partition\n");
#endif
#ifdef CONFIG_MMCSD_SPI
  printf("  /dev/mmcsd0    onboard microSD slot\n");
#endif
#ifdef CONFIG_P2_EC32MB_PSRAM
  printf("  /dev/psram0    explicit 32-MiB serviced bulk store\n");
#endif

  printf("\nTry these\n");
  printf("  leds                         animate both board LEDs\n");
  printf("  gpio -o 1 /dev/gpio0         drive P0 high\n");
  printf("  gpio /dev/gpio1              read looped-back P1\n");
  printf("  p2smartpins gpio|edge|uart|spi test one digital fixture\n");
  printf("  p2smartpins analog            test the P4/P5 RC fixture\n");
  printf("  pwm -f 1000 -d 50 -t 5       output 1-kHz PWM on P4\n");
  printf("  dac put 32768                 set P4 DAC near mid-scale\n");
  printf("  adc -n 8                      collect 8 finite P5 samples\n");
  printf("  i2c bus; i2c dev 03 77        inspect the P24/P25 bus\n");
  printf("  spi help; p2i2c               SPI help / BMP180 proof\n");
  printf("  p2storage probe               inspect flash and microSD\n");
  printf("  hexdump FILE count=128        compact file/device dump\n");
#ifdef CONFIG_P2_EC32MB_PSRAM
  printf("  p2psram 12345678              full volatile-PSRAM proof\n");
#endif

  printf("\nP4/P5 sharing: PWM excludes DAC while open; "
         "capture excludes ADC.\n");
  printf("Digital PWM/capture wants a direct link; analog wants "
         "1k series\n");
  printf("with 100nF from P5 to GND.  Receivers are configured first.\n");
  printf("P2SHOWCASE:PASS\n");
  return 0;
}
