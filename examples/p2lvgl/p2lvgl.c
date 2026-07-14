/****************************************************************************
 * apps/examples/p2lvgl/p2lvgl.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <lvgl/lvgl.h>

#define P2LVGL_GRAPH_POINTS 12
#define P2LVGL_RAW_MAX      4095

struct p2lvgl_demo_s
{
  FAR lv_obj_t *bars[P2LVGL_GRAPH_POINTS];
  FAR lv_obj_t *meters[3];
  FAR lv_obj_t *pulse;
  FAR lv_indev_t *touch_indev;
  uint8_t values[P2LVGL_GRAPH_POINTS];
  uint32_t lfsr;
  uint32_t phase;
  int32_t touch_x;
  int32_t touch_y;
  bool touch_pressed;
};

static struct p2lvgl_demo_s g_demo;

static volatile int g_p2lvgl_jump_stage;

static void p2lvgl_do_longjmp(jmp_buf env, int value)
{
  g_p2lvgl_jump_stage = 2;
  longjmp(env, value);
}

static int p2lvgl_setjmp_test(void)
{
  jmp_buf env;
  int value;

  printf("P2SETJMP:START\n");
  g_p2lvgl_jump_stage = 1;
  value = setjmp(env);
  if (value == 0)
    {
      p2lvgl_do_longjmp(env, 37);
      printf("P2SETJMP:FAIL:LONGJMP_RETURNED\n");
      return 1;
    }

  if (value != 37 || g_p2lvgl_jump_stage != 2)
    {
      printf("P2SETJMP:FAIL:VALUE=%d:STAGE=%d\n", value,
             g_p2lvgl_jump_stage);
      return 1;
    }

  g_p2lvgl_jump_stage = 3;
  value = setjmp(env);
  if (value == 0 && g_p2lvgl_jump_stage == 3)
    {
      g_p2lvgl_jump_stage = 4;
      longjmp(env, 0);
    }

  if (value != 1 || g_p2lvgl_jump_stage != 4)
    {
      printf("P2SETJMP:FAIL:ZERO_VALUE=%d:STAGE=%d\n", value,
             g_p2lvgl_jump_stage);
      return 1;
    }

  printf("P2SETJMP:PASS:VALUE=37:ZERO_TO_ONE=PASS\n");
  return 0;
}

static int32_t p2lvgl_map_touch(uint16_t raw, int32_t raw_start,
                                int32_t raw_end, int32_t pixel_start,
                                int32_t pixel_end, int32_t pixel_limit)
{
  int32_t raw_span = raw_end - raw_start;
  int32_t pixel;

  if (raw_span == 0)
    {
      return 0;
    }

  pixel = pixel_start +
          ((int32_t)raw - raw_start) * (pixel_end - pixel_start) /
          raw_span;
  if (pixel < 0)
    {
      pixel = 0;
    }
  else if (pixel > pixel_limit)
    {
      pixel = pixel_limit;
    }

  return pixel;
}

static void p2lvgl_touch_read(FAR lv_indev_t *indev,
                              FAR lv_indev_data_t *data)
{
  FAR struct p2lvgl_demo_s *demo = lv_indev_get_driver_data(indev);
  FAR lv_display_t *display = lv_indev_get_display(indev);
  struct p2_touch_raw_s sample;
  int32_t width;
  int32_t height;
  bool pressed;

  if (p2_touch_read_raw(&sample) < 0)
    {
      data->state = LV_INDEV_STATE_RELEASED;
      data->point.x = demo->touch_x;
      data->point.y = demo->touch_y;
      demo->touch_pressed = false;
      return;
    }

  pressed = sample.z1 > CONFIG_EXAMPLES_P2LVGL_PRESSURE_MIN &&
            sample.z1 < P2LVGL_RAW_MAX;
  if (pressed)
    {
      width = lv_display_get_horizontal_resolution(display);
      height = lv_display_get_vertical_resolution(display);
      demo->touch_x = p2lvgl_map_touch(
        sample.x, CONFIG_EXAMPLES_P2LVGL_RAW_X_LEFT,
        CONFIG_EXAMPLES_P2LVGL_RAW_X_RIGHT,
        CONFIG_EXAMPLES_P2LVGL_CAL_MARGIN,
        width - CONFIG_EXAMPLES_P2LVGL_CAL_MARGIN - 1, width - 1);
      demo->touch_y = p2lvgl_map_touch(
        sample.y, CONFIG_EXAMPLES_P2LVGL_RAW_Y_TOP,
        CONFIG_EXAMPLES_P2LVGL_RAW_Y_BOTTOM,
        CONFIG_EXAMPLES_P2LVGL_CAL_MARGIN,
        height - CONFIG_EXAMPLES_P2LVGL_CAL_MARGIN - 1, height - 1);

      if (!demo->touch_pressed)
        {
          printf("P2LVGL:TOUCH:DOWN:X=%ld:Y=%ld:Z1=%u\n",
                 (long)demo->touch_x, (long)demo->touch_y, sample.z1);
        }

      data->state = LV_INDEV_STATE_PRESSED;
    }
  else
    {
      if (demo->touch_pressed)
        {
          printf("P2LVGL:TOUCH:UP:X=%ld:Y=%ld\n",
                 (long)demo->touch_x, (long)demo->touch_y);
        }

      data->state = LV_INDEV_STATE_RELEASED;
    }

  demo->touch_pressed = pressed;
  data->point.x = demo->touch_x;
  data->point.y = demo->touch_y;
}

static FAR lv_indev_t *p2lvgl_touch_create(FAR lv_display_t *display)
{
  FAR lv_indev_t *indev = lv_indev_create();

  if (indev != NULL)
    {
      lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
      lv_indev_set_display(indev, display);
      lv_indev_set_driver_data(indev, &g_demo);
      lv_indev_set_read_cb(indev, p2lvgl_touch_read);
    }

  return indev;
}

static void p2lvgl_flat_object(FAR lv_obj_t *obj, uint32_t color)
{
  lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void p2lvgl_activity_timer(FAR lv_timer_t *timer)
{
  FAR struct p2lvgl_demo_s *demo = lv_timer_get_user_data(timer);
  uint32_t wave;
  uint32_t inverse;
  uint32_t value;
  uint32_t height;
  int i;

  demo->lfsr = (demo->lfsr >> 1) ^
               (-(int32_t)(demo->lfsr & 1u) & 0xd0000001u);
  demo->phase = (demo->phase + 4) % 200;
  wave = demo->phase <= 100 ? demo->phase : 200 - demo->phase;
  inverse = 100 - wave;
  value = 10 + (wave * 3) / 4 + (demo->lfsr & 15u);
  if (value > 100)
    {
      value = 100;
    }

  for (i = 0; i < P2LVGL_GRAPH_POINTS - 1; i++)
    {
      demo->values[i] = demo->values[i + 1];
    }

  demo->values[P2LVGL_GRAPH_POINTS - 1] = value;
  for (i = 0; i < P2LVGL_GRAPH_POINTS; i++)
    {
      height = 6 + (demo->values[i] * 94) / 100;
      lv_obj_set_pos(demo->bars[i], 8 + i * 17, 106 - height);
      lv_obj_set_size(demo->bars[i], 11, height);
    }

  lv_obj_set_width(demo->meters[0], 1 + (wave * 182) / 100);
  lv_obj_set_width(demo->meters[1], 1 + (inverse * 182) / 100);
  lv_obj_set_width(demo->meters[2], 1 + (value * 182) / 100);
  lv_obj_set_x(demo->pulse, 18 + (wave * 174) / 100);
}

static FAR lv_obj_t *p2lvgl_track(FAR lv_obj_t *screen, int y,
                                  uint32_t color,
                                  FAR lv_obj_t **meter)
{
  FAR lv_obj_t *track = lv_obj_create(screen);

  lv_obj_set_pos(track, 28, y);
  lv_obj_set_size(track, 184, 18);
  p2lvgl_flat_object(track, 0x263f5a);

  *meter = lv_obj_create(track);
  lv_obj_set_pos(*meter, 1, 1);
  lv_obj_set_size(*meter, 91, 16);
  p2lvgl_flat_object(*meter, color);
  return track;
}

static void p2lvgl_create_ui(void)
{
  FAR struct p2lvgl_demo_s *demo = &g_demo;
  FAR lv_obj_t *screen = lv_screen_active();
  FAR lv_obj_t *graph;
  FAR lv_obj_t *header;
  int i;

  demo->lfsr = 0x7a5b3c1du;
  p2lvgl_flat_object(screen, 0x10243a);
  printf("P2LVGL:INIT:SCREEN\n");

  header = lv_obj_create(screen);
  lv_obj_set_pos(header, 10, 8);
  lv_obj_set_size(header, 220, 10);
  p2lvgl_flat_object(header, 0x6ed6ff);

  graph = lv_obj_create(screen);
  lv_obj_set_pos(graph, 10, 28);
  lv_obj_set_size(graph, 220, 114);
  p2lvgl_flat_object(graph, 0x0b1b2b);

  for (i = 0; i < P2LVGL_GRAPH_POINTS; i++)
    {
      demo->values[i] = 50;
      demo->bars[i] = lv_obj_create(graph);
      lv_obj_set_pos(demo->bars[i], 8 + i * 17, 58);
      lv_obj_set_size(demo->bars[i], 11, 48);
      p2lvgl_flat_object(demo->bars[i], 0xff5c8a);
    }

  printf("P2LVGL:INIT:GRAPH\n");
  p2lvgl_track(screen, 154, 0x31d17c, &demo->meters[0]);
  p2lvgl_track(screen, 180, 0x6ed6ff, &demo->meters[1]);
  p2lvgl_track(screen, 206, 0xffd166, &demo->meters[2]);
  printf("P2LVGL:INIT:METERS\n");

  demo->pulse = lv_obj_create(screen);
  lv_obj_set_pos(demo->pulse, 18, 244);
  lv_obj_set_size(demo->pulse, 30, 30);
  p2lvgl_flat_object(demo->pulse, 0xffffff);

  lv_timer_create(p2lvgl_activity_timer, 60, demo);
  printf("P2LVGL:INIT:ANIMATION\n");
}

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

  if (argc == 2 && strcmp(argv[1], "--setjmp-test") == 0)
    {
      return p2lvgl_setjmp_test();
    }

  printf("P2LVGL:INIT:BEGIN\n");
  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/lcd0";
  info.input_path = NULL;
  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      printf("P2LVGL:FAIL:DISPLAY\n");
      return 1;
    }

  g_demo.touch_indev = p2lvgl_touch_create(result.disp);
  if (g_demo.touch_indev == NULL)
    {
      printf("P2LVGL:FAIL:TOUCH_INPUT\n");
      return 1;
    }

  p2lvgl_create_ui();
  printf("P2LVGL:READY:BACKEND=/dev/lcd0:FB=DISABLED:"
         "TOUCH=XPT2046_PRESSURE:ANIMATION_MS=60\n");

  for (;;)
    {
      uint32_t idle = lv_timer_handler();
      usleep((idle != 0 ? idle : 1) * 1000);
    }

  return 0;
}
