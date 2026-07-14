/****************************************************************************
 * apps/examples/p2touchcal/p2touchcal.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/video/fb.h>

#define P2CAL_TARGET_COUNT 4
#define P2CAL_SAMPLE_COUNT 3
#define P2CAL_TARGET_MARGIN 24
#define P2CAL_CROSS_RADIUS  14

#define P2CAL_DIM_BLUE      0x18a3
#define P2CAL_WHITE         0xffff
#define P2CAL_RED           0xf800
#define P2CAL_GREEN         0x07e0

struct p2cal_target_s
{
  const char *name;
  int x;
  int y;
  int raw_x;
  int raw_y;
};

struct p2cal_fb_s
{
  int fd;
  FAR uint16_t *pixels;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
};

static void p2cal_setpixel(FAR struct p2cal_fb_s *fb, int x, int y,
                           uint16_t color)
{
  FAR uint16_t *row;

  if (x < 0 || y < 0 || x >= fb->vinfo.xres || y >= fb->vinfo.yres)
    {
      return;
    }

  row = (FAR uint16_t *)((FAR uint8_t *)fb->pixels +
                         y * fb->pinfo.stride);
  row[x] = color;
}

static void p2cal_update(FAR struct p2cal_fb_s *fb, int x, int y,
                         int width, int height)
{
  struct fb_area_s area;

  area.x = x;
  area.y = y;
  area.w = width;
  area.h = height;
  if (ioctl(fb->fd, FBIO_UPDATE,
            (unsigned long)(uintptr_t)&area) < 0)
    {
      printf("P2TOUCHCAL:WARN:FB_UPDATE=%d\n", errno);
    }
}

static void p2cal_draw_target(FAR struct p2cal_fb_s *fb, int x, int y,
                              uint16_t color)
{
  int d;

  for (d = -P2CAL_CROSS_RADIUS; d <= P2CAL_CROSS_RADIUS; d++)
    {
      p2cal_setpixel(fb, x + d, y, color);
      p2cal_setpixel(fb, x, y + d, color);
    }

  for (d = -3; d <= 3; d++)
    {
      p2cal_setpixel(fb, x + d, y - 3, color);
      p2cal_setpixel(fb, x + d, y + 3, color);
      p2cal_setpixel(fb, x - 3, y + d, color);
      p2cal_setpixel(fb, x + 3, y + d, color);
    }

  p2cal_update(fb, x - P2CAL_CROSS_RADIUS - 1,
               y - P2CAL_CROSS_RADIUS - 1,
               2 * P2CAL_CROSS_RADIUS + 3,
               2 * P2CAL_CROSS_RADIUS + 3);
}

static int p2cal_fb_open(FAR struct p2cal_fb_s *fb)
{
  memset(fb, 0, sizeof(*fb));
  fb->fd = open(CONFIG_EXAMPLES_P2TOUCHCAL_FB_DEVPATH, O_RDWR);
  if (fb->fd < 0)
    {
      return -errno;
    }

  if (ioctl(fb->fd, FBIOGET_VIDEOINFO,
            (unsigned long)(uintptr_t)&fb->vinfo) < 0 ||
      ioctl(fb->fd, FBIOGET_PLANEINFO,
            (unsigned long)(uintptr_t)&fb->pinfo) < 0)
    {
      int ret = -errno;
      close(fb->fd);
      return ret;
    }

  if (fb->pinfo.bpp != 16 || fb->vinfo.xres < 80 || fb->vinfo.yres < 80)
    {
      close(fb->fd);
      return -ENOTSUP;
    }

  fb->pixels = mmap(NULL, fb->pinfo.fblen, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FILE, fb->fd, 0);
  if (fb->pixels == MAP_FAILED)
    {
      int ret = -errno;
      close(fb->fd);
      return ret;
    }

  return 0;
}

static void p2cal_fb_close(FAR struct p2cal_fb_s *fb)
{
  munmap(fb->pixels, fb->pinfo.fblen);
  close(fb->fd);
}

static bool p2cal_raw_down(FAR const struct p2_touch_raw_s *sample)
{
  return sample->z1 > 100 && sample->z1 < 4000;
}

static int p2cal_read_target(FAR struct p2cal_target_s *target)
{
  struct p2_touch_raw_s sample;
  int64_t sum_x = 0;
  int64_t sum_y = 0;
  int last_down = -1;
  int report_count = 0;
  int count = 0;
  bool captured = false;

  for (;;)
    {
      bool down;
      int ret;

      ret = p2_touch_read_raw(&sample);
      if (ret < 0)
        {
          return ret;
        }

      down = p2cal_raw_down(&sample);
      if ((int)down != last_down || report_count++ >= 20)
        {
          printf("P2TOUCHCAL:SPI:PEN=%s:X=%u:Y=%u:Z1=%u:Z2=%u\n",
                 sample.pen_down ? "DOWN" : "UP", sample.x, sample.y,
                 sample.z1, sample.z2);
          last_down = down;
          report_count = 0;
        }

      if (down && count < P2CAL_SAMPLE_COUNT)
        {
          sum_x += sample.x;
          sum_y += sample.y;
          count++;
          printf("P2TOUCHCAL:RAW:TARGET=%s:SAMPLE=%d:X=%u:Y=%u:"
                 "Z1=%u:Z2=%u\n", target->name, count, sample.x,
                 sample.y, sample.z1, sample.z2);
          captured = count == P2CAL_SAMPLE_COUNT;
        }

      if (!down && captured)
        {
          target->raw_x = (int)(sum_x / P2CAL_SAMPLE_COUNT);
          target->raw_y = (int)(sum_y / P2CAL_SAMPLE_COUNT);
          return 0;
        }

      usleep(50000);
    }
}

static int p2cal_abs(int value)
{
  return value < 0 ? -value : value;
}

static int p2cal_pen_diag(void)
{
  bool last_high = false;
  bool have_last = false;
  uint32_t polls = 0;

  printf("P2TOUCHPEN:READY:PIN=P%d:MODE=GPIO_ONLY:BIAS=%s:SPI=OFF:"
         "SAMPLE_MS=5:CTRL_C_TO_STOP\n",
         CONFIG_P2_EC32MB_XPT2046_PEN_PIN,
#ifdef CONFIG_P2_EC32MB_TOUCHPEN_FLOAT_INPUT
         "FLOAT"
#else
         "PULL_UP"
#endif
         );

  for (;;)
    {
      bool high;
      int ret;

      ret = p2_touch_read_pen_level(&high);
      if (ret < 0)
        {
          printf("P2TOUCHPEN:FAIL:GPIO=%d\n", ret);
          return EXIT_FAILURE;
        }

      polls++;
      if (!have_last || high != last_high || polls % 50 == 0)
        {
          printf("P2TOUCHPEN:GPIO:P%d=%s:PEN=%s:POLLS=%lu\n",
                 CONFIG_P2_EC32MB_XPT2046_PEN_PIN,
                 high ? "HIGH" : "LOW", high ? "UP" : "DOWN",
                 (unsigned long)polls);
          have_last = true;
          last_high = high;
        }

      usleep(5000);
    }
}

int main(int argc, FAR char *argv[])
{
  struct p2cal_fb_s fb;
  struct p2cal_target_s targets[P2CAL_TARGET_COUNT];
  int x_left;
  int x_right;
  int y_top;
  int y_bottom;
  int ret;
  int i;

  if (argc == 2 && strcmp(argv[1], "--pen") == 0)
    {
      return p2cal_pen_diag();
    }

  ret = p2cal_fb_open(&fb);
  if (ret < 0)
    {
      printf("P2TOUCHCAL:FAIL:FRAMEBUFFER=%d\n", ret);
      return EXIT_FAILURE;
    }

  targets[0] = (struct p2cal_target_s)
    {"TOP_LEFT", P2CAL_TARGET_MARGIN, P2CAL_TARGET_MARGIN, 0, 0};
  targets[1] = (struct p2cal_target_s)
    {"TOP_RIGHT", fb.vinfo.xres - P2CAL_TARGET_MARGIN - 1,
     P2CAL_TARGET_MARGIN, 0, 0};
  targets[2] = (struct p2cal_target_s)
    {"BOTTOM_RIGHT", fb.vinfo.xres - P2CAL_TARGET_MARGIN - 1,
     fb.vinfo.yres - P2CAL_TARGET_MARGIN - 1, 0, 0};
  targets[3] = (struct p2cal_target_s)
    {"BOTTOM_LEFT", P2CAL_TARGET_MARGIN,
     fb.vinfo.yres - P2CAL_TARGET_MARGIN - 1, 0, 0};

  memset(fb.pixels, 0, fb.pinfo.fblen);
  for (i = 0; i < P2CAL_TARGET_COUNT; i++)
    {
      p2cal_draw_target(&fb, targets[i].x, targets[i].y,
                        P2CAL_DIM_BLUE);
    }

  p2cal_update(&fb, 0, 0, fb.vinfo.xres, fb.vinfo.yres);
  printf("P2TOUCHCAL:READY:METHOD=4_CORNERS:SAMPLES=3:INTERVAL_MS=50:"
         "SOURCE=XPT2046_Z1\n");

  for (i = 0; i < P2CAL_TARGET_COUNT; i++)
    {
      p2cal_draw_target(&fb, targets[i].x, targets[i].y, P2CAL_WHITE);
      printf("P2TOUCHCAL:TARGET=%s:X=%d:Y=%d:TOUCH_AND_HOLD\n",
             targets[i].name, targets[i].x, targets[i].y);

      ret = p2cal_read_target(&targets[i]);
      if (ret < 0)
        {
          printf("P2TOUCHCAL:FAIL:READ=%d\n", ret);
          p2cal_fb_close(&fb);
          return EXIT_FAILURE;
        }

      p2cal_draw_target(&fb, targets[i].x, targets[i].y, P2CAL_GREEN);
      printf("P2TOUCHCAL:CAPTURED=%s:X=%d:Y=%d\n", targets[i].name,
             targets[i].raw_x, targets[i].raw_y);
    }

  x_left = (targets[0].raw_x + targets[3].raw_x) / 2;
  x_right = (targets[1].raw_x + targets[2].raw_x) / 2;
  y_top = (targets[0].raw_y + targets[1].raw_y) / 2;
  y_bottom = (targets[2].raw_y + targets[3].raw_y) / 2;

  if (p2cal_abs(x_right - x_left) < 500 ||
      p2cal_abs(y_bottom - y_top) < 500)
    {
      for (i = 0; i < fb.vinfo.xres * fb.vinfo.yres; i++)
        {
          fb.pixels[i] = P2CAL_RED;
        }

      p2cal_update(&fb, 0, 0, fb.vinfo.xres, fb.vinfo.yres);
      printf("P2TOUCHCAL:FAIL:GEOMETRY:X_LEFT=%d:X_RIGHT=%d:Y_TOP=%d:Y_BOTTOM=%d\n",
             x_left, x_right, y_top, y_bottom);
      p2cal_fb_close(&fb);
      return EXIT_FAILURE;
    }

  memset(fb.pixels, 0, fb.pinfo.fblen);
  for (i = 0; i < P2CAL_TARGET_COUNT; i++)
    {
      p2cal_draw_target(&fb, targets[i].x, targets[i].y, P2CAL_GREEN);
    }

  p2cal_update(&fb, 0, 0, fb.vinfo.xres, fb.vinfo.yres);
  printf("P2TOUCHCAL:RESULT:X_LEFT=%d:X_RIGHT=%d:Y_TOP=%d:Y_BOTTOM=%d\n",
         x_left, x_right, y_top, y_bottom);
  printf("P2TOUCHCAL:OK:PASS_RESULT_TO_DEMO_BUILD\n");

  p2cal_fb_close(&fb);
  return EXIT_SUCCESS;
}
