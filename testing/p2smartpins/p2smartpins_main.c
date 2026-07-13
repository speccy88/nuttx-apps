/****************************************************************************
 * apps/testing/p2smartpins/p2smartpins_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/ioexpander/gpio.h>

#ifdef CONFIG_TESTING_P2SMARTPINS_EDGE
#  include <signal.h>
#  include <time.h>
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_UART
#  include <termios.h>
#  include <nuttx/clock.h>
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_PWM_CAPTURE
#  include <nuttx/timers/capture.h>
#  include <nuttx/timers/pwm.h>
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_DAC_ADC
#  include <nuttx/analog/adc.h>
#  include <nuttx/analog/dac.h>
#  include <nuttx/analog/ioctl.h>
#  include <nuttx/clock.h>
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_SPI
#  include <nuttx/spi/spi_transfer.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2SMART_GPIO_SAMPLE_COUNT  8
#define P2SMART_PAYLOAD_SIZE       16

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const bool g_gpio_pattern[P2SMART_GPIO_SAMPLE_COUNT] =
{
  false, true, true, false, true, false, false, true
};

#ifdef CONFIG_TESTING_P2SMARTPINS_PWM_CAPTURE
static const uint8_t g_pwm_duty_targets[3] =
{
  25, 50, 75
};
#endif

#if defined(CONFIG_TESTING_P2SMARTPINS_UART) || \
    defined(CONFIG_TESTING_P2SMARTPINS_SPI)
static const uint8_t g_digital_payload[P2SMART_PAYLOAD_SIZE] =
{
  0x50, 0x32, 0x5a, 0xa5, 0x00, 0xff, 0x69, 0x96,
  0x11, 0x22, 0x44, 0x88, 0x7e, 0x81, 0x3c, 0xc3
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int p2smart_fail(FAR const char *stage, int error)
{
  printf("P2SMART:FAIL:%s:%d\n", stage, error);
  return EXIT_FAILURE;
}

#if defined(CONFIG_TESTING_P2SMARTPINS_UART) || \
    defined(CONFIG_TESTING_P2SMARTPINS_SPI)
static uint32_t p2smart_fnv1a(FAR const uint8_t *data, size_t length)
{
  uint32_t hash = UINT32_C(2166136261);
  size_t index;

  for (index = 0; index < length; index++)
    {
      hash ^= data[index];
      hash *= UINT32_C(16777619);
    }

  return hash;
}
#endif

static int p2smart_gpio_safe(int outfd, int infd)
{
  enum gpio_pintype_e outtype;
  enum gpio_pintype_e intype;

  if (ioctl(outfd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_INPUT_PIN) < 0 ||
      ioctl(infd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_INPUT_PIN) < 0 ||
      ioctl(outfd, GPIOC_PINTYPE,
            (unsigned long)((uintptr_t)&outtype)) < 0 ||
      ioctl(infd, GPIOC_PINTYPE,
            (unsigned long)((uintptr_t)&intype)) < 0)
    {
      return -errno;
    }

  if (outtype != GPIO_INPUT_PIN || intype != GPIO_INPUT_PIN)
    {
      return -EIO;
    }

  return OK;
}

static int p2smart_gpio_test(void)
{
  bool received;
  unsigned int index;
  int infd = -1;
  int outfd = -1;
  int ret = -EIO;

  printf("P2SMART:GPIO:BEGIN=0-1\n");

  /* Open and configure the receiver before the source can drive the link. */

  infd = open(CONFIG_TESTING_P2SMARTPINS_GPIO_IN_DEVPATH, O_RDWR);
  if (infd < 0 ||
      ioctl(infd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_INPUT_PIN) < 0)
    {
      ret = -errno;
      goto out;
    }

  outfd = open(CONFIG_TESTING_P2SMARTPINS_GPIO_OUT_DEVPATH, O_RDWR);
  if (outfd < 0 ||
      ioctl(outfd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_INPUT_PIN) < 0 ||
      ioctl(outfd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_OUTPUT_PIN) < 0)
    {
      ret = -errno;
      goto out;
    }

  for (index = 0; index < P2SMART_GPIO_SAMPLE_COUNT; index++)
    {
      if (ioctl(outfd, GPIOC_WRITE,
                (unsigned long)g_gpio_pattern[index]) < 0)
        {
          ret = -errno;
          goto out;
        }

      usleep(1000);
      if (ioctl(infd, GPIOC_READ,
                (unsigned long)((uintptr_t)&received)) < 0)
        {
          ret = -errno;
          goto out;
        }

      printf("P2SMART:GPIO:SAMPLE=%u:TX=%u:RX=%u\n",
             index, g_gpio_pattern[index] ? 1 : 0, received ? 1 : 0);
      if (received != g_gpio_pattern[index])
        {
          ret = -EIO;
          goto out;
        }
    }

  ret = OK;

out:
  if (outfd >= 0 && infd >= 0)
    {
      int saferet = p2smart_gpio_safe(outfd, infd);

      if (saferet < 0 && ret >= 0)
        {
          ret = saferet;
        }
    }
  else if (outfd >= 0)
    {
      ioctl(outfd, GPIOC_SETPINTYPE, (unsigned long)GPIO_INPUT_PIN);
    }

  if (outfd >= 0)
    {
      close(outfd);
    }

  if (infd >= 0)
    {
      close(infd);
    }

  if (ret < 0)
    {
      return ret;
    }

  printf("P2SMART:GPIO:SAFE=FLOAT\n");
  printf("P2SMART:GPIO:PASS\n");
  return OK;
}

#ifdef CONFIG_TESTING_P2SMARTPINS_EDGE
static int p2smart_edge_test(void)
{
  struct sigevent event;
  struct timespec timeout;
  sigset_t set;
  unsigned int index;
  int infd = -1;
  int outfd = -1;
  int ret = -EIO;
  bool registered = false;
  int saferet;

  printf("P2SMART:EDGE:BEGIN=0-1\n");
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
    {
      return -errno;
    }

  infd = open(CONFIG_TESTING_P2SMARTPINS_GPIO_IN_DEVPATH, O_RDWR);
  if (infd < 0 ||
      ioctl(infd, GPIOC_SETPINTYPE, (unsigned long)GPIO_INPUT_PIN) < 0)
    {
      ret = -errno;
      goto out;
    }

  /* Seed the source's retained output latch low while P1 is still a plain
   * input.  Float P0 again before arming P1, then every subsequent drive
   * transition has a deterministic starting level.
   */

  outfd = open(CONFIG_TESTING_P2SMARTPINS_GPIO_OUT_DEVPATH, O_RDWR);
  if (outfd < 0 ||
      ioctl(outfd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_OUTPUT_PIN) < 0 ||
      ioctl(outfd, GPIOC_WRITE, 0) < 0 ||
      ioctl(outfd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_INPUT_PIN) < 0 ||
      ioctl(infd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_INTERRUPT_BOTH_PIN) < 0)
    {
      ret = -errno;
      goto out;
    }

  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;
  if (ioctl(infd, GPIOC_REGISTER,
            (unsigned long)((uintptr_t)&event)) < 0)
    {
      ret = -errno;
      goto out;
    }

  registered = true;
  if (ioctl(outfd, GPIOC_SETPINTYPE,
            (unsigned long)GPIO_OUTPUT_PIN) < 0)
    {
      ret = -errno;
      goto out;
    }

  timeout.tv_sec = 0;
  timeout.tv_nsec = 100 * 1000 * 1000;
  for (index = 0; index < 6; index++)
    {
      bool level = (index & 1) == 0;

      if (ioctl(outfd, GPIOC_WRITE,
                (unsigned long)level) < 0)
        {
          ret = -errno;
          goto out;
        }

      if (sigtimedwait(&set, NULL, &timeout) != SIGUSR1)
        {
          ret = errno == EAGAIN ? -ETIMEDOUT : -errno;
          goto out;
        }
    }

  ret = OK;

out:
  saferet = OK;
  if (outfd >= 0)
    {
      if (ioctl(outfd, GPIOC_SETPINTYPE,
                (unsigned long)GPIO_INPUT_PIN) < 0)
        {
          saferet = -errno;
        }
    }

  if (registered)
    {
      if (ioctl(infd, GPIOC_UNREGISTER, 0) < 0)
        {
          saferet = -errno;
        }
    }

  if (infd >= 0)
    {
      if (ioctl(infd, GPIOC_SETPINTYPE,
                (unsigned long)GPIO_INPUT_PIN) < 0)
        {
          saferet = -errno;
        }
    }

  if (outfd >= 0)
    {
      close(outfd);
    }

  if (infd >= 0)
    {
      close(infd);
    }

  if (saferet < 0 && ret >= 0)
    {
      ret = saferet;
    }

  if (ret >= 0)
    {
      printf("P2SMART:EDGE:COUNT=6\n");
      printf("P2SMART:EDGE:SAFE=FLOAT\n");
      printf("P2SMART:EDGE:PASS\n");
    }

  return ret;
}
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_UART
static int p2smart_uart_test(void)
{
  struct termios term;
  uint8_t received[P2SMART_PAYLOAD_SIZE];
  clock_t deadline;
  size_t count = 0;
  ssize_t nbytes;
  int fd;
  int ret = -EIO;

  printf("P2SMART:UART:BEGIN=2-3\n");
  fd = open(CONFIG_TESTING_P2SMARTPINS_UART_DEVPATH,
            O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      return -errno;
    }

  if (tcgetattr(fd, &term) < 0)
    {
      ret = -errno;
      goto out;
    }

  term.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                    IGNCR | ICRNL | IXON);
  term.c_oflag &= ~OPOST;
  term.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  term.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
  term.c_cflag |= CS8 | CLOCAL | CREAD;
  cfsetispeed(&term, B115200);
  cfsetospeed(&term, B115200);
  if (tcsetattr(fd, TCSANOW, &term) < 0 || tcflush(fd, TCIOFLUSH) < 0)
    {
      ret = -errno;
      goto out;
    }

  nbytes = write(fd, g_digital_payload, sizeof(g_digital_payload));
  if (nbytes != sizeof(g_digital_payload))
    {
      ret = nbytes < 0 ? -errno : -EIO;
      goto out;
    }

  deadline = clock_systime_ticks() + MSEC2TICK(500);
  while (count < sizeof(received) &&
         clock_systime_ticks() < deadline)
    {
      nbytes = read(fd, &received[count], sizeof(received) - count);
      if (nbytes > 0)
        {
          count += nbytes;
        }
      else if (nbytes < 0 && errno != EAGAIN && errno != EINTR)
        {
          ret = -errno;
          goto out;
        }
      else
        {
          usleep(1000);
        }
    }

  if (count != sizeof(received) ||
      memcmp(received, g_digital_payload, sizeof(received)) != 0)
    {
      goto out;
    }

  printf("P2SMART:UART:COUNT=%u:FNV1A=%08" PRIX32 "\n",
         P2SMART_PAYLOAD_SIZE,
         p2smart_fnv1a(received, sizeof(received)));
  ret = OK;

out:
  if (close(fd) < 0 && ret >= 0)
    {
      ret = -errno;
    }

  if (ret >= 0)
    {
      printf("P2SMART:UART:SAFE=FLOAT\n");
      printf("P2SMART:UART:PASS\n");
    }

  return ret;
}
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_PWM_CAPTURE
static int p2smart_pwm_capture_test(void)
{
  struct pwm_info_s info;
  struct cap_all_s captured;
  unsigned int index;
  int capfd = -1;
  int pwmfd = -1;
  int ret = -EIO;
  bool pwm_started = false;

  printf("P2SMART:PWM_CAPTURE:BEGIN=4-5\n");
  capfd = open(CONFIG_TESTING_P2SMARTPINS_CAPTURE_DEVPATH, O_RDONLY);
  if (capfd < 0)
    {
      return -errno;
    }

  pwmfd = open(CONFIG_TESTING_P2SMARTPINS_PWM_DEVPATH, O_RDONLY);
  if (pwmfd < 0)
    {
      ret = -errno;
      goto out;
    }

  for (index = 0; index < 3; index++)
    {
      memset(&info, 0, sizeof(info));
      info.frequency = 1000;
      info.channels[0].channel = 1;
      info.channels[0].duty =
        (ub16_t)(((uint32_t)g_pwm_duty_targets[index] << 16) / 100);
      if (ioctl(pwmfd, PWMIOC_SETCHARACTERISTICS,
                (unsigned long)((uintptr_t)&info)) < 0 ||
          ioctl(pwmfd, PWMIOC_START, 0) < 0)
        {
          ret = -errno;
          goto out;
        }

      pwm_started = true;
      usleep(50000);
      memset(&captured, 0, sizeof(captured));
      if (ioctl(capfd, CAPIOC_ALL,
                (unsigned long)((uintptr_t)&captured)) < 0)
        {
          ret = -errno;
          goto out;
        }

      if (ioctl(pwmfd, PWMIOC_STOP, 0) < 0)
        {
          ret = -errno;
          goto out;
        }

      pwm_started = false;
      printf("P2SMART:PWM_CAPTURE:SAMPLE=%u:FREQ=%" PRIu32
             ":DUTY=%u:EDGES=%" PRIu32 "\n",
             index, captured.freq, captured.duty, captured.edges);
      if (captured.freq < 950 || captured.freq > 1050 ||
          captured.duty + 5 < g_pwm_duty_targets[index] ||
          captured.duty > g_pwm_duty_targets[index] + 5 ||
          captured.edges == 0)
        {
          goto out;
        }
    }

  ret = OK;

out:
  if (pwmfd >= 0)
    {
      if (pwm_started && ioctl(pwmfd, PWMIOC_STOP, 0) < 0 && ret >= 0)
        {
          ret = -errno;
        }

      if (close(pwmfd) < 0 && ret >= 0)
        {
          ret = -errno;
        }
    }

  if (close(capfd) < 0 && ret >= 0)
    {
      ret = -errno;
    }

  if (ret >= 0)
    {
      printf("P2SMART:PWM_CAPTURE:SAFE=FLOAT\n");
      printf("P2SMART:PWM_CAPTURE:PASS\n");
    }

  return ret;
}
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_DAC_ADC
static int p2smart_adc_sample(int fd, FAR struct adc_msg_s *sample)
{
  clock_t deadline = clock_systime_ticks() + MSEC2TICK(200);
  ssize_t nbytes;

  if (ioctl(fd, ANIOC_TRIGGER, 0) < 0)
    {
      return -errno;
    }

  do
    {
      nbytes = read(fd, sample, sizeof(*sample));
      if (nbytes == sizeof(*sample))
        {
          return OK;
        }

      if (nbytes < 0 && errno != EAGAIN && errno != EINTR)
        {
          return -errno;
        }

      usleep(1000);
    }
  while (clock_systime_ticks() < deadline);

  return -ETIMEDOUT;
}

static int p2smart_dac_adc_test(void)
{
  struct dac_msg_s output;
  struct adc_msg_s input;
  int32_t previous = INT32_MIN;
  unsigned int index;
  ssize_t nbytes;
  int adcfd = -1;
  int dacfd = -1;
  int ret = -EIO;

  printf("P2SMART:DAC_ADC:BEGIN=4-5\n");
  adcfd = open(CONFIG_TESTING_P2SMARTPINS_ADC_DEVPATH,
               O_RDONLY | O_NONBLOCK);
  if (adcfd < 0)
    {
      return -errno;
    }

  dacfd = open(CONFIG_TESTING_P2SMARTPINS_DAC_DEVPATH, O_WRONLY);
  if (dacfd < 0)
    {
      ret = -errno;
      goto out;
    }

  for (index = 0; index < 3; index++)
    {
      output.am_channel = 0;
      output.am_data = (int32_t)
        (((int64_t)CONFIG_TESTING_P2SMARTPINS_DAC_FULL_SCALE *
          (index + 1)) / 4);
      nbytes = write(dacfd, &output, sizeof(output));

      if (nbytes != sizeof(output))
        {
          ret = nbytes < 0 ? -errno : -EIO;
          goto out;
        }

      usleep(20000);
      memset(&input, 0, sizeof(input));
      ret = p2smart_adc_sample(adcfd, &input);
      if (ret < 0)
        {
          goto out;
        }

      printf("P2SMART:DAC_ADC:SAMPLE=%u:DAC=%" PRId32
             ":ADC=%" PRId32 "\n",
             index, output.am_data, input.am_data);
      if (index > 0 && input.am_data <= previous)
        {
          ret = -ERANGE;
          goto out;
        }

      previous = input.am_data;
    }

  ret = OK;

out:
  if (dacfd >= 0)
    {
      if (close(dacfd) < 0 && ret >= 0)
        {
          ret = -errno;
        }
    }

  if (close(adcfd) < 0 && ret >= 0)
    {
      ret = -errno;
    }

  if (ret >= 0)
    {
      printf("P2SMART:DAC_ADC:SAFE=FLOAT\n");
      printf("P2SMART:DAC_ADC:PASS\n");
    }

  return ret;
}
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_SPI
static int p2smart_spi_test(void)
{
  struct spi_sequence_s sequence;
  struct spi_trans_s transaction;
  uint8_t received[P2SMART_PAYLOAD_SIZE];
  uint32_t txhash;
  uint32_t rxhash;
  int closeret;
  int transfer_errno;
  int fd;
  int ret;

  printf("P2SMART:SPI:BEGIN=6-7\n");
  fd = open(CONFIG_TESTING_P2SMARTPINS_SPI_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&received, 0, sizeof(received));
  memset(&sequence, 0, sizeof(sequence));
  memset(&transaction, 0, sizeof(transaction));
  transaction.deselect = true;
  transaction.nwords = P2SMART_PAYLOAD_SIZE;
  transaction.txbuffer = g_digital_payload;
  transaction.rxbuffer = received;
  sequence.dev = SPIDEV_USER(0);
  sequence.mode = SPIDEV_MODE0;
  sequence.nbits = 8;
  sequence.ntrans = 1;
  sequence.frequency = 100000;
  sequence.trans = &transaction;

  ret = ioctl(fd, SPIIOC_TRANSFER,
              (unsigned long)((uintptr_t)&sequence));
  transfer_errno = errno;
  closeret = close(fd);
  if (ret < 0)
    {
      return -transfer_errno;
    }

  if (closeret < 0)
    {
      return -errno;
    }

  txhash = p2smart_fnv1a(g_digital_payload, sizeof(g_digital_payload));
  rxhash = p2smart_fnv1a(received, sizeof(received));
  printf("P2SMART:SPI:COUNT=%u:TX=%08" PRIX32 ":RX=%08" PRIX32 "\n",
         P2SMART_PAYLOAD_SIZE, txhash, rxhash);
  if (memcmp(received, g_digital_payload, sizeof(received)) != 0)
    {
      return -EIO;
    }

  printf("P2SMART:SPI:SAFE=FLOAT\n");
  printf("P2SMART:SPI:PASS\n");
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

  UNUSED(argc);
  UNUSED(argv);

  printf("P2SMART:BEGIN\n");
  printf("P2SMART:WIRING=P0-P1,P2-P3,P4-P5,P6-P7\n");
  printf("P2SMART:CAPS=GPIO");
#ifdef CONFIG_TESTING_P2SMARTPINS_EDGE
  printf(",EDGE");
#endif
#ifdef CONFIG_TESTING_P2SMARTPINS_UART
  printf(",UART");
#endif
#ifdef CONFIG_TESTING_P2SMARTPINS_PWM_CAPTURE
  printf(",PWM_CAPTURE");
#endif
#ifdef CONFIG_TESTING_P2SMARTPINS_DAC_ADC
  printf(",DAC_ADC");
#endif
#ifdef CONFIG_TESTING_P2SMARTPINS_SPI
  printf(",SPI");
#endif
  printf("\n");

  ret = p2smart_gpio_test();
  if (ret < 0)
    {
      return p2smart_fail("GPIO", ret);
    }

#ifdef CONFIG_TESTING_P2SMARTPINS_EDGE
  ret = p2smart_edge_test();
  if (ret < 0)
    {
      return p2smart_fail("EDGE", ret);
    }
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_UART
  ret = p2smart_uart_test();
  if (ret < 0)
    {
      return p2smart_fail("UART", ret);
    }
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_PWM_CAPTURE
  ret = p2smart_pwm_capture_test();
  if (ret < 0)
    {
      return p2smart_fail("PWM_CAPTURE", ret);
    }
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_DAC_ADC
  ret = p2smart_dac_adc_test();
  if (ret < 0)
    {
      return p2smart_fail("DAC_ADC", ret);
    }
#endif

#ifdef CONFIG_TESTING_P2SMARTPINS_SPI
  ret = p2smart_spi_test();
  if (ret < 0)
    {
      return p2smart_fail("SPI", ret);
    }
#endif

  printf("P2SMART:PASS\n");
  return EXIT_SUCCESS;
}
