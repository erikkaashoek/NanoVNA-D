/*
 * Copyright (c) 2019-2021, Dmitry (DiSlord) dislordlive@gmail.com
 * Based on TAKAHASHI Tomohiro (TTRFTECH) edy555@gmail.com
 * All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * The software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ch.h"
#include "hal.h"
#include "usbcfg.h"
#include "si5351.h"
#include "nanovna.h"

#include <chprintf.h>
#include <string.h>

/*
 *  Shell settings
 */
// If need run shell as thread (use more amount of memory fore stack), after
// enable this need reduce spi_buffer size, by default shell run in main thread
// #define VNA_SHELL_THREAD

static BaseSequentialStream *shell_stream = 0;
threads_queue_t shell_thread;

// Shell new line
#define VNA_SHELL_NEWLINE_STR    "\r\n"
// Shell command promt
#define VNA_SHELL_PROMPT_STR     "ch> "
// Shell max arguments
#define VNA_SHELL_MAX_ARGUMENTS   4
// Shell max command line size
#define VNA_SHELL_MAX_LENGTH     64
// Shell frequency printf format
//#define VNA_FREQ_FMT_STR         "%lu"
#define VNA_FREQ_FMT_STR         "%u"

// Shell command functions prototypes
typedef void (*vna_shellcmd_t)(int argc, char *argv[]);
#define VNA_SHELL_FUNCTION(command_name) \
      static void command_name(int argc, char *argv[])

// Shell command line buffer, args, nargs, and function ptr
static char shell_line[VNA_SHELL_MAX_LENGTH];
static char *shell_args[VNA_SHELL_MAX_ARGUMENTS + 1];
static uint16_t shell_nargs;
static volatile vna_shellcmd_t  shell_function = 0;

//#define ENABLED_DUMP_COMMAND
// Allow get threads debug info
#define ENABLE_THREADS_COMMAND
// Enable vbat_offset command, allow change battery voltage correction in config
#define ENABLE_VBAT_OFFSET_COMMAND
// Info about NanoVNA, need fore soft
#define ENABLE_INFO_COMMAND
// Enable color command, allow change config color for traces, grid, menu
#define ENABLE_COLOR_COMMAND
// Enable transform command
//#define ENABLE_TRANSFORM_COMMAND
// Enable sample command
//#define ENABLE_SAMPLE_COMMAND
// Enable I2C command for send data to AIC3204, used for debug
//#define ENABLE_I2C_COMMAND
// Enable LCD command for send data to LCD screen, used for debug
//#define ENABLE_LCD_COMMAND
// Enable output debug data on screen on hard fault
#define ENABLE_HARD_FAULT_HANDLER_DEBUG
// Enable test command, used for debug
//#define ENABLE_TEST_COMMAND
// Enable stat command, used for debug
//#define ENABLE_STAT_COMMAND
// Enable gain command, used for debug
//#define ENABLE_GAIN_COMMAND
// Enable port command, used for debug
//#define ENABLE_PORT_COMMAND
// Enable si5351 register write, used for debug
//#define ENABLE_SI5351_REG_WRITE
// Enable i2c timing command, used for debug
//#define ENABLE_I2C_TIMINGS
// Enable band setting command, used for debug
//#define ENABLE_BAND_COMMAND
// Enable scan_bin command (need use ex scan in future)
//#define ENABLE_SCANBIN_COMMAND
// Enable debug for console command
//#define DEBUG_CONSOLE_SHOW
// Enable usart command
#define ENABLE_USART_COMMAND
// Enable config command
#define ENABLE_CONFIG_COMMAND
#ifdef __USE_SD_CARD__
// Enable SD card console command
#define ENABLE_SD_CARD_COMMAND
#endif

static void transform_domain(uint16_t ch_mask);
static uint16_t get_sweep_mask(void);
void update_frequencies(void);
static int  set_frequency(freq_t freq);
static void set_frequencies(freq_t start, freq_t stop, uint16_t points);
static bool sweep(bool break_on_operation, uint16_t ch_mask);
int shell_printf(const char *fmt, ...);

uint8_t sweep_mode = SWEEP_ENABLE;
// current sweep point (used for continue sweep if user break)
volatile uint16_t p_sweep = 0;
volatile int requested_points;
uint16_t old_p_sweep;
// Sweep measured data
phase_t measured[SWEEP_POINTS_MAX][MAX_MEASURED];


#define TEMP_COUNT 64
#define TEMP_MASK 0x3f
phase_t temp_measured[TEMP_COUNT][MAX_MEASURED];
volatile int temp_input = 0;
volatile int temp_output = 0;

//uint16_t sample_count;
float aver_freq_a;
float aver_freq_b;
float aver_freq_delta;
uint16_t aver_freq_count_a;
float aver_freq_sum_a;
phase_t aver_phase_d;
float aver_freq_d;
phase_t last_phase_d;
float last_freq_d;
float level_a;
float level_b;
#ifdef SIDE_CHANNEL
float level_sa;
float level_sb;
#endif
double phase_wraps = 0;
double prev_phase = 0;
int missing_samples = 0;
uint32_t transform_count = 0;
uint32_t max_average_count = 5;

//#undef VERSION
//#define VERSION "1.2.15"
// Version text, displayed in Config->Version menu, also send by info command
const char *info_about[]={
  "Board: " BOARD_NAME,
  "2019-2022 Copyright @ErikKaashoek (based on @edy555/DiSlord source)",
  "Licensed under GPL.",
  "  https://github.com/erikkaashoek/NanoVNA-D",
  "Version: " VERSION " ["\
  "p:"define_to_STR(SWEEP_POINTS_MAX)", "\
  "IF:"define_to_STR(FREQUENCY_IF_K)"k, "\
  "ADC:"define_to_STR(AUDIO_ADC_FREQ_K1)"k, "\
  "Lcd:"define_to_STR(LCD_WIDTH)"x"define_to_STR(LCD_HEIGHT)\
  "]",  "Build Time: " __DATE__ " - " __TIME__,
//  "Kernel: " CH_KERNEL_VERSION,
//  "Compiler: " PORT_COMPILER_NAME,
  "Architecture: " PORT_ARCHITECTURE_NAME " Core Variant: " PORT_CORE_VARIANT_NAME,
//  "Port Info: " PORT_INFO,
  "Platform: " PLATFORM_NAME,
  0 // sentinel
};

// Allow draw some debug on LCD
#ifdef DEBUG_CONSOLE_SHOW
void my_debug_log(int offs, char *log){
  static uint16_t shell_line_y = 0;
  lcd_set_foreground(LCD_FG_COLOR);
  lcd_set_background(LCD_BG_COLOR);
  lcd_fill(FREQUENCIES_XPOS1, shell_line_y, LCD_WIDTH-FREQUENCIES_XPOS1, 2 * FONT_GET_HEIGHT);
  lcd_drawstring(FREQUENCIES_XPOS1 + offs, shell_line_y, log);
  shell_line_y+=FONT_STR_HEIGHT;
  if (shell_line_y >= LCD_HEIGHT - FONT_STR_HEIGHT*4) shell_line_y=0;
}
#define DEBUG_LOG(offs, text)    my_debug_log(offs, text);
#else
#define DEBUG_LOG(offs, text)
#endif

static THD_WORKING_AREA(waThread1, 1024);
static THD_FUNCTION(Thread1, arg)
{
  (void)arg;
  chRegSetThreadName("sweep");
#ifdef __FLIP_DISPLAY__
  if(VNA_MODE(VNA_MODE_FLIP_DISPLAY))
    lcd_set_flip(true);
#endif
/*
 * UI (menu, touch, buttons) and plot initialize
 */
  ui_init();
  //Initialize graph plotting
  plot_init();
  while (1) {
    bool completed = false;
    uint16_t mask = get_sweep_mask();
    if (sweep_mode&(SWEEP_ENABLE|SWEEP_ONCE)) {
      completed = sweep(true, mask);
      sweep_mode&=~SWEEP_ONCE;
    } else {
      __WFI();
    }
//    if (completed && (props_mode & TD_TRANSFORM)) {
//      transform_domain(mask);
//    }
    // Run Shell command in sweep thread
    while (shell_function) {
      shell_function(shell_nargs - 1, &shell_args[1]);
      shell_function = 0;
      osalThreadDequeueNextI(&shell_thread, MSG_OK);
    }
    // Process UI inputs
    sweep_mode|= SWEEP_UI_MODE;
    ui_process();
    sweep_mode&=~SWEEP_UI_MODE;
    // Process collected data, calculate trace coordinates and plot only if scan completed
    if (completed) {
#ifdef __USE_SMOOTH__
//    START_PROFILE;
      if (smooth_factor)
        measurementDataSmooth(mask);
//    STOP_PROFILE;
#endif
//      START_PROFILE
      if ((props_mode & DOMAIN_MODE) == DOMAIN_TIME) transform_domain(mask);
//      STOP_PROFILE;
      // Prepare draw graphics, cache all lines, mark screen cells for redraw
      request_to_redraw(REDRAW_PLOT);
    }
    request_to_redraw(REDRAW_BATTERY);
#ifndef DEBUG_CONSOLE_SHOW
    // plot trace and other indications as raster
    draw_all();
#endif
  }
}

void
pause_sweep(void)
{
  sweep_mode &= ~SWEEP_ENABLE;
}

void
resume_sweep(void)
{
  sweep_mode |= SWEEP_ENABLE;
}

void
toggle_sweep(void)
{
  sweep_mode ^= SWEEP_ENABLE;
}

#if 0
//
// Original kaiser_window functions
//
static float
bessel0(float x)
{
  const float eps = 0.0001f;

  float ret = 0;
  float term = 1;
  float m = 0;

  while (term  > eps * ret) {
    ret += term;
    ++m;
    term *= (x*x) / (4*m*m);
  }
  return ret;
}

static float
kaiser_window(float k, float n, float beta)
{
  if (beta == 0.0) return 1.0f;
  float r = (2 * k) / (n - 1) - 1;
  return bessel0(beta * vna_sqrtf(1 - r * r)) / bessel0(beta);
}
#else
//             Zero-order Bessel function
//     (x/2)^(2n)
// 1 + ----------
//       (n!)^2
// set input as (x/2)^2 (input range 0 .. beta*beta/4)
//       x^n           x       x^2      x^3      x^4     x^5
// 1 + ------ = 1 + ------ + ------ + ------ + ------ + ------  ......
//     (n!)^2          1        4       36       576    14400
// first 2 (0 and 1) precalculated as simple.

// Precalculated multiplier step for 1 / ((n!)^2)  max SIZE 16 (first 2 use as init)
static const float div[] = {/*0, 1,*/ 1/4.0f, 1/9.0f, 1/16.0f, 1/25.0f, 1/36.0f, 1/49.0f, 1/64.0f, 1/81.0f, 1/100.0f, 1/121.0f, 1/144.0f, 1/169.0f, 1/196.0f, 1/225.0f, 1/256.0f};
float bessel0_ext(float x_pow_2)
{
// set calculated count, more SIZE - less error but longer (bigger beta also need more size for less error)
// For beta =  6 SIZE =  (11-2) no error
// For beta = 13 SIZE =  (13-2) max error 0.0002 (use as default, use constant size faster then check every time limits in float)
#define SIZE     (13-2)
  int i = SIZE;
  float term = x_pow_2;
  float ret = 1.0f + term;
  do {
    term*= x_pow_2 * div[SIZE - i];
    ret += term;
  }while(--i);
  return ret;
}
// Move out constant divider:  bessel0(beta)
// Made calculation optimization (in integer)
// x = (2*k)/(n-1) - 1 = (set n=n-1) = 2*k/n - 1 = (2*k-n)/n
// calculate kaiser window vs bessel0(w) there:
//                                    n*n - (2*k-n)*(2*k-n)              4*k*(n-k)
// w = beta*sqrt(1 - x*x) = beta*sqrt(---------------------) = beta*sqrt(---------)
//                                           n*n                            n*n
// bessel0(w) = bessel0_ext(z) (there z = (w/2)^2 for speed)
// return = bessel0_ext(z)
static float
kaiser_window_ext(uint32_t k, uint32_t n, uint16_t beta)
{
  if (beta == 0) return 1.0f;
  n = n - 1;
  k = k * (n - k) * beta * beta;
  n = n * n;
  return bessel0_ext((float)k / n);
}
#endif

#ifdef USE_FFT_WINDOW_BUFFER
    // Cache window function data to static buffer
    static float kaiser_data[FFT_SIZE];
#endif

double fft_maximum;
int fft_maximum_index;

static void
transform_domain(uint16_t ch_mask)
{
  (void)ch_mask;
  int fft_points = FFT_SIZE;
  // use spi_buffer as temporary buffer and calculate ifft for time domain
  // Need 2 * sizeof(float) * FFT_SIZE bytes for work
#if 2*4*FFT_SIZE > (SPI_BUFFER_SIZE * LCD_PIXEL_SIZE)
#error "Need increase spi_buffer or use less FFT_SIZE value"
#endif
  int i;
  uint16_t offset = 0;
//  uint8_t is_lowpass = FALSE;
//  switch (domain_func) {
//  case TD_FUNC_BANDPASS:
//    break;
//    case TD_FUNC_LOWPASS_IMPULSE:
//      is_lowpass = TRUE;
//      offset = fft_points;
//      break;
//  }
  uint16_t window_size = fft_points + offset;
  uint16_t beta = 0;
  switch (domain_window) {
//    case TD_WINDOW_MINIMUM:
//    beta = 0;  // this is rectangular
//      break;
    case TD_WINDOW_NORMAL:
      beta = 6;
      break;
    case TD_WINDOW_MAXIMUM:
      beta = 13;
      break;
  }
  // Add amplitude correction for not full size FFT data and also add computed default scale
  // recalculate the scale factor if any window details are changed. The scale factor is to compensate for windowing.
  // Add constant multiplier for kaiser_window_ext use 1.0f / bessel0_ext(beta*beta/4.0f)
  // Add constant multiplier  1.0f / FFT_SIZE
  static float window_scale = 0.0f;
  static uint16_t td_cache = 0;
  // Check mode cache data
  uint16_t td_check = (props_mode & (TD_WINDOW|TD_FUNC))|(fft_points<<5);
  if (td_cache!=td_check){
    td_cache = td_check;
    if (domain_func == TD_FUNC_LOWPASS_STEP)
      window_scale = 1.0f / (FFT_SIZE * bessel0_ext(beta*beta/4.0f));
    else {
      window_scale = 0.0f;
      for (int i = 0; i < fft_points; i++)
        window_scale += kaiser_window_ext(i + offset, window_size, beta);
      if (domain_func == TD_FUNC_BANDPASS) window_scale = 1.0f / (       window_scale);
      else                                 window_scale = 1.0f / (2.0f * window_scale);
//    window_scale*= FFT_SIZE               // add correction from kaiser_window
//    window_scale/= FFT_SIZE               // add defaut from FFT_SIZE
//    window_scale*= bessel0_ext(beta*beta/4.0f) // for get result as kaiser_window
//    window_scale/= bessel0_ext(beta*beta/4.0f) // for set correction on calculated kaiser_window for value
    }
#ifdef USE_FFT_WINDOW_BUFFER
    // Cache window function data to static buffer
    static float kaiser_data[FFT_SIZE];
    for (i = 0; i < fft_points; i++)
      kaiser_data[i] = kaiser_window_ext(i + offset, window_size, beta) * window_scale;
#endif
  }
  // Made Time Domain Calculations
//  for (int ch = 0; ch < 2; ch++,ch_mask>>=1) {
  {
    // Prepare data in tmp buffer (use spi_buffer), apply window function and constant correction factor
    float* tmp  = (float*)spi_buffer;
    phase_t *data = measured[0];
#if 1
    for (i = 0; i < FFT_SIZE; i++) {
#ifdef USE_FFT_WINDOW_BUFFER
      float w = kaiser_data[i];
#else
      float w = kaiser_window_ext(i + offset, window_size, beta) * window_scale;
#endif
      tmp[i * 2 + 0] *= w;
      tmp[i * 2 + 1] *= w;
//      shell_printf("%d %f\r\n", i, tmp[i * 2 + 0]);

    }
#else
    for (i = 0; i < fft_points; i++) {
#ifdef USE_FFT_WINDOW_BUFFER
      float w = kaiser_data[i];
#else
      float w = kaiser_window_ext(i + offset, window_size, beta) * window_scale;
#endif
      tmp[i * 2 + 0] = data[i * MAX_MEASURED + 3] * w;
      tmp[i * 2 + 1] = 0; //data[i * MAX_MEASURED + 1] * w;
    }
    // Fill zeroes last
    for (; i < FFT_SIZE; i++) {
      tmp[i * 2 + 0] = 0.0f;
      tmp[i * 2 + 1] = 0.0f;
    }
    // For lowpass mode swap
    if (is_lowpass) {
      for (i = 1; i < fft_points; i++) {
        tmp[(FFT_SIZE - i) * 2 + 0] =  tmp[i * 4 + 3];
        tmp[(FFT_SIZE - i) * 2 + 1] = 0; //-tmp[i * 4 + 1];
      }
    }
#endif
    // Made FFT in temp buffer
    fft_forward((float(*)[2])tmp);
    fft_maximum = -200;
    fft_maximum_index = 0;

    // Copy data back
    if (current_props._fft_mode == FFT_AMP || current_props._fft_mode == FFT_B || current_props._fft_mode == FFT_PHASE ) {
      fft_points = FFT_SIZE/2;
      if (fft_points > sweep_points/2)
        fft_points = sweep_points/2;
      data = measured[sweep_points/2];
      int fft_step = 4;
      if (VNA_MODE(VNA_MODE_WIDE))
        fft_step = 2;
      for (i = 0; i < fft_points; i++) {
        float re = tmp[i * fft_step + 0];
        float im = tmp[i * fft_step + 1];
        volatile float f =  vna_sqrtf(re*re+im*im);
        if (!VNA_MODE(VNA_MODE_WIDE)) {
          re = tmp[i * fft_step + 2];
          im = tmp[i * fft_step + 3];
          volatile float f2 =  vna_sqrtf(re*re+im*im);
          if (f < f2) f = f2;
        }
        f = (data[i * MAX_MEASURED + 1] * transform_count + f) / ( transform_count+1);
        data[i * MAX_MEASURED + 1] =  f;
        if (fft_maximum < f) {
          fft_maximum = f;
          fft_maximum_index = sweep_points/2 + i;
        }
      }
      data = measured[sweep_points/2];
      tmp = &tmp[FFT_SIZE*2];
      for (i = 0; i < fft_points; i++) {
        float re = tmp[i * -fft_step + -1];
        float im = tmp[i * -fft_step + -2];
        volatile float f =  vna_sqrtf(re*re+im*im);
        if (!VNA_MODE(VNA_MODE_WIDE)) {
          re = tmp[i * -fft_step + -3];
          im = tmp[i * -fft_step + -4];
          volatile float f2 =  vna_sqrtf(re*re+im*im);
          if (f < f2) f = f2;
        }
        f = (data[(i+1) * -MAX_MEASURED + 1] * transform_count + f) / ( transform_count+1);
        data[(i+1) * -MAX_MEASURED + 1] =  f;
        if (fft_maximum < f) {
          fft_maximum = f;
          fft_maximum_index = sweep_points/2 - i;
        }
      }
      if (VNA_MODE(VNA_MODE_WIDE)) {
        data = measured[0];
        if (fft_maximum_index == 0 || fft_maximum_index == fft_points-1)
          aver_freq_a = get_fft_marker_freq(fft_maximum_index);
        else {
          const int idx        = fft_maximum_index;
          const float delta_Hz = get_fft_marker_freq(idx + 1) - get_fft_marker_freq(idx + 0);
          const float y1 = data[(idx - 1) * MAX_MEASURED + 1];
          const float y2 = data[(idx - 0) * MAX_MEASURED + 1];
          const float y3 = data[(idx + 1) * MAX_MEASURED + 1];
          aver_freq_a = 0.5 * (y1 - y3) / ((y1 - (2 * y2) + y3) + 1e-12);
          aver_freq_b =  aver_freq_a = aver_freq_a * delta_Hz + get_fft_marker_freq(idx);
          set_marker_index(active_marker, idx);
        }
      }

    } else {
      fft_points = FFT_SIZE/2;
      if (fft_points > sweep_points)
        fft_points = sweep_points;
      for (i = 0; i < fft_points; i++) {
        float re = tmp[i * 2 + 0];
        float im = tmp[i * 2 + 1];
        volatile float f =  vna_sqrtf(re*re+im*im);
        f = (data[i * MAX_MEASURED + 1] * transform_count + f) / ( transform_count+1);
        data[i * MAX_MEASURED + 1] =  f;
        if (fft_maximum < f) {
          fft_maximum = f;
          fft_maximum_index = i;
        }
      }
      if (fft_maximum_index == 0 || fft_maximum_index == fft_points-1)
        aver_freq_a = get_fft_marker_freq(fft_maximum_index);
      else {
        const int idx        = fft_maximum_index;
        const float delta_Hz = get_fft_marker_freq(idx + 0) - get_fft_marker_freq(idx + 1);
        const float y1         = data[(idx - 1) * MAX_MEASURED + 1];
        const float y2         = data[(idx - 0) * MAX_MEASURED + 1];
        const float y3         = data[(idx + 1) * MAX_MEASURED + 1];
        aver_freq_a            = fabs(delta_Hz) * 0.5 * (y1 - y3) / ((y1 - (2 * y2) + y3) + 1e-12) + get_fft_marker_freq(idx);
        set_marker_index(active_marker, idx);
      }
    }
    if ((props_mode & TD_AVERAGE) && transform_count < max_average_count)
      transform_count++;
  }
}

// Shell commands output
int shell_printf(const char *fmt, ...)
{
  if (shell_stream == NULL) return 0;
  va_list ap;
  int formatted_bytes;
  va_start(ap, fmt);
  formatted_bytes = chvprintf(shell_stream, fmt, ap);
  va_end(ap);
  return formatted_bytes;
}

static void shell_write(const void *buf, uint32_t size) {streamWrite(shell_stream, buf, size);}
static int  shell_read(void *buf, uint32_t size)        {return streamRead(shell_stream, buf, size);}
//static void shell_put(uint8_t c)                      {streamPut(shell_stream, c);}
//static uint8_t shell_getc(void)                       {return streamGet(shell_stream);}

#ifdef __USE_SERIAL_CONSOLE__
// Serial Shell commands output
int serial_shell_printf(const char *fmt, ...)
{
  va_list ap;
  int formatted_bytes;
  va_start(ap, fmt);
  formatted_bytes = chvprintf((BaseSequentialStream *)&SD1, fmt, ap);
  va_end(ap);
  return formatted_bytes;
}
#endif

//
// Function used for search substring v in list
// Example need search parameter "center" in "start|stop|center|span|cw" getStringIndex return 2
// If not found return -1
// Used for easy parse command arguments
static int get_str_index(const char *v, const char *list)
{
  int i = 0;
  while (1) {
    const char *p = v;
    while (1) {
      char c = *list;
      if (c == '|') c = 0;
      if (c == *p++) {
        // Found, return index
        if (c == 0) return i;
        list++;    // Compare next symbol
        continue;
      }
      break;  // Not equal, break
    }
    // Set new substring ptr
    while (1) {
      // End of string, not found
      if (*list == 0) return -1;
      if (*list++ == '|') break;
    }
    i++;
  }
  return -1;
}

VNA_SHELL_FUNCTION(cmd_pause)
{
  (void)argc;
  (void)argv;
  pause_sweep();
}

VNA_SHELL_FUNCTION(cmd_resume)
{
  (void)argc;
  (void)argv;

  // restore frequencies array and cal
  update_frequencies();
  resume_sweep();
}

VNA_SHELL_FUNCTION(cmd_reset)
{
  (void)argc;
  (void)argv;
#ifdef __DFU_SOFTWARE_MODE__
  if (argc == 1) {
    if (get_str_index(argv[0], "dfu") == 0) {
      shell_printf("Performing reset to DFU mode" VNA_SHELL_NEWLINE_STR);
      enter_dfu();
      return;
    }
  }
#endif
  shell_printf("Performing reset" VNA_SHELL_NEWLINE_STR);
  NVIC_SystemReset();
}

// Use macro, std isdigit more big
#define _isdigit(c) (c >= '0' && c <= '9')
// Rewrite universal standart str to value functions to more compact
//
// Convert string to int32
int32_t my_atoi(const char *p)
{
  int32_t value = 0;
  uint32_t c;
  bool neg = false;

  if (*p == '-') {neg = true; p++;}
  if (*p == '+') p++;
  while ((c = *p++ - '0') < 10)
    value = value * 10 + c;
  return neg ? -value : value;
}

// Convert string to uint32
//  0x - for hex radix
//  0o - for oct radix
//  0b - for bin radix
//  default dec radix
uint32_t my_atoui(const char *p)
{
  uint32_t value = 0, radix = 10, c;
  if (*p == '+') p++;
  if (*p == '0') {
    switch (p[1]) {
      case 'x': radix = 16; break;
      case 'o': radix =  8; break;
      case 'b': radix =  2; break;
      default:  goto calculate;
    }
    p+=2;
  }
calculate:
  while (1) {
    c = *p++ - '0';
    // c = to_upper(*p) - 'A' + 10
    if (c >= 'A' - '0') c = (c&(~0x20)) - ('A' - '0') + 10;
    if (c >= radix) return value;
    value = value * radix + c;
  }
}

float
my_atof(const char *p)
{
  int neg = FALSE;
  if (*p == '-')
    neg = TRUE;
  if (*p == '-' || *p == '+')
    p++;
  float x = my_atoi(p);
  while (_isdigit((int)*p))
    p++;
  if (*p == '.' || *p == ',') {
    float d = 1.0f;
    p++;
    while (_isdigit((int)*p)) {
      d /= 10.0f;
      x += d * (*p - '0');
      p++;
    }
  }
  if (*p == 'e' || *p == 'E') {
    p++;
    int exp = my_atoi(p);
    while (exp > 0) {
      x *= 10;
      exp--;
    }
    while (exp < 0) {
      x /= 10;
      exp++;
    }
  } else if (*p) {
       /*if (*p == 'G') x*= 1e+9;  // Giga
    else if (*p == 'M') x*= 1e+6;  // Mega
    else if (*p == 'k') x*= 1e+3;  // kilo
    else */ if (*p == 'm') x*= 1e-3;  // milli
    else if (*p == 'u') x*= 1e-6;  // micro
    else if (*p == 'n') x*= 1e-9;  // nano
    else if (*p == 'p') x*= 1e-12; // pico
  }
  if (neg)
    x = -x;
  return x;
}

#ifdef __USE_SMOOTH__
VNA_SHELL_FUNCTION(cmd_smooth)
{
  if (argc == 1) {
    set_smooth_factor(my_atoui(argv[0]));
    return;
  }
  shell_printf("smooth = %d" VNA_SHELL_NEWLINE_STR, smooth_factor);
}
#endif

#ifdef ENABLE_CONFIG_COMMAND
VNA_SHELL_FUNCTION(cmd_config) {
  static const char cmd_mode_list[] =
    "auto"
#ifdef __USE_SMOOTH__
    "|avg"
#endif
#ifdef __USE_SERIAL_CONSOLE__
    "|connection"
#endif
    "|mode"
    "|grid"
    "|dot"
#ifdef __USE_BACKUP__
    "|bk"
#endif
#ifdef __FLIP_DISPLAY__
    "|flip"
#endif
#ifdef __DIGIT_SEPARATOR__
    "|separator"
#endif
  ;
  int idx;
  if (argc == 2 && (idx = get_str_index(argv[0], cmd_mode_list)) >= 0) {
    apply_VNA_mode(idx, my_atoui(argv[1]));
  }
  else
    shell_printf("usage: config {%s} [0|1]" VNA_SHELL_NEWLINE_STR, cmd_mode_list);
}
#endif

#ifdef __VNA_MEASURE_MODULE__
  VNA_SHELL_FUNCTION(cmd_measure) {
    static const char cmd_measure_list[] =
    "none"
#ifdef __USE_LC_MATCHING__
    "|lc"        // Add LC match function
#endif
#ifdef __S21_MEASURE__
    "|lcshunt"   // Enable LC shunt measure option
    "|lcseries"  // Enable LC series  measure option
    "|xtal"      // Enable XTAL measure option
#endif
#ifdef __S11_CABLE_MEASURE__
    "|cable"     // Enable S11 cable measure option
#endif
#ifdef __S11_RESONANCE_MEASURE__
    "|resonance" // Enable S11 resonance search option
#endif
    ;
    int idx;
    if (argc == 1 && (idx = get_str_index(argv[0], cmd_measure_list)) >= 0)
      plot_set_measure_mode(idx);
    else
      shell_printf("usage: measure {%s}" VNA_SHELL_NEWLINE_STR, cmd_measure_list);
  }
#endif

#ifdef USE_VARIABLE_OFFSET
VNA_SHELL_FUNCTION(cmd_offset)
{
  if (argc != 1) {
    shell_printf("usage: offset {frequency offset(Hz)}" VNA_SHELL_NEWLINE_STR \
                 "current: %u" VNA_SHELL_NEWLINE_STR, IF_OFFSET);
    return;
  }
  si5351_set_frequency_offset(my_atoi(argv[0]));
}
#endif

VNA_SHELL_FUNCTION(cmd_freq)
{
  if (argc != 1) {
    shell_printf("usage: freq {frequency(Hz)}" VNA_SHELL_NEWLINE_STR);
    return;
  }
  uint32_t freq = my_atoui(argv[0]);
  pause_sweep();
  set_frequency(freq);
}

VNA_SHELL_FUNCTION(cmd_decimation)
{
  if (argc != 1) {
    shell_printf("usage: decimation {factor}" VNA_SHELL_NEWLINE_STR);
    return;
  }
  uint32_t decm = my_atoui(argv[0]);
  current_props.decimation = decm;
  set_tau(get_tau());   // Increase Tau if needed for decimation.
}

VNA_SHELL_FUNCTION(cmd_tau)
{
  if (argc != 1) {
    shell_printf("usage: tau {time(s)}" VNA_SHELL_NEWLINE_STR);
    return;
  }
  float freq = my_atof(argv[0]);
  set_tau(freq);
}

VNA_SHELL_FUNCTION(cmd_log_type)
{
  if (argc != 1) goto usage;
  //                              0   1
  static const char cmd_list[] = "phase|unwrapped|frequency";
  int index = get_str_index(argv[0], cmd_list);
  if (index >= 0) {
    current_props.log_type = index;
    return;
  }
usage:
  shell_printf("usage: log_type {%s}" VNA_SHELL_NEWLINE_STR, cmd_list);
}


void set_power(uint8_t value){
  request_to_redraw(REDRAW_CAL_STATUS);
  if (value > SI5351_CLK_DRIVE_STRENGTH_8MA) value = SI5351_CLK_DRIVE_STRENGTH_AUTO;
  if (current_props._power == value) return;
  current_props._power = value;
  // Update power if pause, need for generation in CW mode
  if (!(sweep_mode&SWEEP_ENABLE)) si5351_set_power(value);
}
#if 0
VNA_SHELL_FUNCTION(cmd_power)
{
  if (argc != 1) {
    shell_printf("usage: power {0-3}|{255 - auto}" VNA_SHELL_NEWLINE_STR \
                 "power: %d" VNA_SHELL_NEWLINE_STR, current_props._power);
    return;
  }
  set_power(my_atoi(argv[0]));
}

VNA_SHELL_FUNCTION(cmd_pull)
{
  if (argc == 0) {
    for (int i=0;i<MAX_PULL;i++) {
      shell_printf("pull: %d %f" VNA_SHELL_NEWLINE_STR, i, config.pull[i]);
    }
    return;
  }
  if (argc == 2) {
    int i = my_atoi(argv[0]);
    config.pull[i] = my_atof(argv[1]);
    shell_printf("pull: %d %f" VNA_SHELL_NEWLINE_STR,i , config.pull[i]);
    return;
  }
//  set_frequency(frequency);
}
#endif
#ifdef __USE_RTC__
VNA_SHELL_FUNCTION(cmd_time)
{
  (void)argc;
  (void)argv;
  uint32_t  dt_buf[2];
  dt_buf[0] = rtc_get_tr_bcd(); // TR should be read first for sync
  dt_buf[1] = rtc_get_dr_bcd(); // DR should be read second
  static const uint8_t idx_to_time[] = {6,5,4,2,  1,  0};
  static const char       time_cmd[] = "y|m|d|h|min|sec";
  //            0    1   2       4      5     6
  // time[] ={sec, min, hr, 0, day, month, year, 0}
  uint8_t   *time = (uint8_t*)dt_buf;
  if (argc == 3 &&  get_str_index(argv[0], "b") == 0){
    rtc_set_time(my_atoui(argv[1]), my_atoui(argv[2]));
    return;
  }
  if (argc!=2) goto usage;
  int idx = get_str_index(argv[0], time_cmd);
  uint32_t val = my_atoui(argv[1]);
  if (idx < 0 || val > 99)
    goto usage;
  // Write byte value in struct
  time[idx_to_time[idx]] = ((val/10)<<4)|(val%10); // value in bcd format
  rtc_set_time(dt_buf[1], dt_buf[0]);
  return;
usage:
  shell_printf("20%02x/%02x/%02x %02x:%02x:%02x" VNA_SHELL_NEWLINE_STR \
               "usage: time {[%s] 0-99} or {b 0xYYMMDD 0xHHMMSS}" VNA_SHELL_NEWLINE_STR,
                time[6], time[5], time[4], time[2], time[1], time[0], time_cmd);
}
#endif

#ifdef __VNA_ENABLE_DAC__
VNA_SHELL_FUNCTION(cmd_dac)
{
  if (argc != 1) {
    shell_printf("usage: dac {value(0-4095)}" VNA_SHELL_NEWLINE_STR \
                 "current: %d" VNA_SHELL_NEWLINE_STR, config._dac_value);
    return;
  }
  dac_setvalue_ch2(my_atoui(argv[0])&0xFFF);
}
#endif
#if 0
VNA_SHELL_FUNCTION(cmd_threshold)
{
  uint32_t value;
  if (argc != 1) {
    shell_printf("usage: threshold {frequency in harmonic mode}" VNA_SHELL_NEWLINE_STR \
                 "current: %d" VNA_SHELL_NEWLINE_STR, config._harmonic_freq_threshold);
    return;
  }
  value = my_atoui(argv[0]);
  config._harmonic_freq_threshold = value;
}
#endif
VNA_SHELL_FUNCTION(cmd_saveconfig)
{
  (void)argc;
  (void)argv;
  config_save();
  shell_printf("Config saved" VNA_SHELL_NEWLINE_STR);
}

VNA_SHELL_FUNCTION(cmd_clearconfig)
{
  if (argc != 1) {
    shell_printf("usage: clearconfig {protection key}" VNA_SHELL_NEWLINE_STR);
    return;
  }

  if (get_str_index(argv[0], "1234") != 0) {
    shell_printf("Key unmatched." VNA_SHELL_NEWLINE_STR);
    return;
  }

  clear_all_config_prop_data();
  shell_printf("Config and all cal data cleared." VNA_SHELL_NEWLINE_STR \
               "Do reset manually to take effect. Then do touch cal and save." VNA_SHELL_NEWLINE_STR);
}

VNA_SHELL_FUNCTION(cmd_data)
{
  int i;
  int sel = 0;
  phase_t (*array)[MAX_MEASURED];
  if (argc == 1)
    sel = my_atoi(argv[0]);
  if (sel < 0 || sel >=7)
    goto usage;

  array = &measured[sel][0];

  for (i = 0; i < sweep_points; i++)
    shell_printf("%f %f" VNA_SHELL_NEWLINE_STR, array[i][0], array[i][1]);
  return;
usage:
  shell_printf("usage: data [array]" VNA_SHELL_NEWLINE_STR);
}

VNA_SHELL_FUNCTION(cmd_capture)
{
// read pixel count at one time (PART*2 bytes required for read buffer)
  (void)argc;
  (void)argv;
  int y;
// Check buffer limits, if less possible reduce rows count
#define READ_ROWS 2
#if (SPI_BUFFER_SIZE*LCD_PIXEL_SIZE) < (LCD_RX_PIXEL_SIZE*LCD_WIDTH*READ_ROWS)
#error "Low size of spi_buffer for cmd_capture"
#endif
  // Text on screenshot
  if (argc > 0) {
    lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
    for (int i = 0; i < argc; i++)
      lcd_printf(OFFSETX + CELLOFFSETX + 2, AREA_HEIGHT_NORMAL - (argc - i) * FONT_STR_HEIGHT - 2, argv[i]);
    request_to_redraw(REDRAW_AREA);
  }
  // read 2 row pixel time
  for (y = 0; y < LCD_HEIGHT; y += READ_ROWS) {
    // use uint16_t spi_buffer[2048] (defined in ili9341) for read buffer
    lcd_read_memory(0, y, LCD_WIDTH, READ_ROWS, (uint16_t *)spi_buffer);
    shell_write(spi_buffer, READ_ROWS * LCD_WIDTH * sizeof(uint16_t));
  }
}

#if 0
VNA_SHELL_FUNCTION(cmd_gamma)
{
  float gamma[2];
  (void)argc;
  (void)argv;
  
  pause_sweep();
  chMtxLock(&mutex);
  wait_dsp(4);  
  calculate_gamma(gamma);
  chMtxUnlock(&mutex);

  shell_printf("%d %d" VNA_SHELL_NEWLINE_STR, gamma[0], gamma[1]);
}
#endif

#if 0
static void (*sample_func)(float *gamma) = calculate_gamma;
#ifdef ENABLE_SAMPLE_COMMAND
VNA_SHELL_FUNCTION(cmd_sample)
{
  if (argc != 1) goto usage;
  //                                         0    1   2
  static const char cmd_sample_list[] = "gamma|ampl|ref";
  switch (get_str_index(argv[0], cmd_sample_list)) {
    case 0:
      sample_func = calculate_gamma;
      return;
    case 1:
      sample_func = fetch_amplitude;
      return;
    case 2:
      sample_func = fetch_amplitude_ref;
      return;
    default:
      break;
  }
usage:
  shell_printf("usage: sample {%s}" VNA_SHELL_NEWLINE_STR, cmd_sample_list);
}
#endif
#endif
config_t config = {
  .magic       = CONFIG_MAGIC,
  ._harmonic_freq_threshold = FREQUENCY_THRESHOLD,
  ._IF_freq    = FREQUENCY_OFFSET,
  ._touch_cal  = DEFAULT_TOUCH_CONFIG,
  ._vna_mode   = VNA_MODE_USB | VNA_MODE_SEARCH_MAX | VNA_MODE_PLL_ON | VNA_MODE_SCROLLING_ON,
  ._brightness = DEFAULT_BRIGHTNESS,
  ._dac_value   = 1922,
  ._vbat_offset = 420,
  ._bandwidth = 2,
  ._lcd_palette = LCD_DEFAULT_PALETTE,
  ._serial_speed = SERIAL_DEFAULT_BITRATE,
  ._xtal_freq = XTALFREQ,
  .xtal_offset = 0,
  ._measure_r = MEASURE_DEFAULT_R,
  . pull = {2.2, 1.423e-5, -0.379, 1.188e-6 },
  ._lever_mode = LM_MARKER,
  ._band_mode = 0,
};

properties_t current_props;

// NanoVNA Default settings
static const trace_t def_trace[TRACES_MAX] =
{//enable, type, auto_scale, scale, refpos, min, max
  { TRUE, TRC_DPHASE, true, 90.0, 0,0,0 },
  { TRUE, TRC_AFREQ, true, 0.01, 0,0,0 },
  { TRUE, TRC_DFREQ, true, 0.01, 0,0,0 },
  { TRUE, TRC_RESIDUE,  true, 0.00001,  0,0,0 },
};

static const marker_t def_markers[MARKERS_MAX] = {
  { TRUE, 0, 10*SWEEP_POINTS_MAX/100-1, 0 },
#if MARKERS_MAX > 1
  {FALSE, 0, 20*SWEEP_POINTS_MAX/100-1, 0 },
#endif
#if MARKERS_MAX > 2
  {FALSE, 0, 30*SWEEP_POINTS_MAX/100-1, 0 },
#endif
#if MARKERS_MAX > 3
  {FALSE, 0, 40*SWEEP_POINTS_MAX/100-1, 0 },
#endif
#if MARKERS_MAX > 4
  {FALSE, 0, 50*SWEEP_POINTS_MAX/100-1, 0 },
#endif
#if MARKERS_MAX > 5
  {FALSE, 0, 60*SWEEP_POINTS_MAX/100-1, 0 },
#endif
#if MARKERS_MAX > 6
  {FALSE, 0, 70*SWEEP_POINTS_MAX/100-1, 0 },
#endif
#if MARKERS_MAX > 7
  {FALSE, 0, 80*SWEEP_POINTS_MAX/100-1, 0 },
#endif
};

// Load propeties default settings
void load_default_properties(void)
{
//Magic add on caldata_save
//current_props.magic = CONFIG_MAGIC;
  current_props._frequency0       = 10000000;    // start =  50kHz
  current_props._frequency1       = 10000000;    // end   = 900MHz
  current_props._var_freq         = 0;
  current_props._sweep_points     = POINTS_COUNT_DEFAULT; // Set default points count
  current_props._cal_frequency0   =     50000;    // calibration start =  50kHz
  current_props._cal_frequency1   = 900000000;    // calibration end   = 900MHz
  current_props._cal_sweep_points = POINTS_COUNT_DEFAULT; // Set calibration default points count
  current_props._cal_status   = 0;
//=============================================
  memcpy(current_props._trace, def_trace, sizeof(def_trace));
  memcpy(current_props._markers, def_markers, sizeof(def_markers));
//=============================================
  current_props._electrical_delay = 0.0f;
  current_props._var_delay = 0.0f;
  current_props._s21_offset       = 0.0f;
  current_props._portz = 50.0f;
  current_props._velocity_factor = 70;
  current_props._current_trace   = 0;
  current_props._active_marker   = 0;
  current_props._previous_marker = MARKER_INVALID;
  current_props._mode            = TD_WINDOW_MAXIMUM|TD_CENTER_SPAN;
  current_props._reserved = 0;
  current_props._power     = SI5351_CLK_DRIVE_STRENGTH_AUTO;
  current_props._cal_power = SI5351_CLK_DRIVE_STRENGTH_AUTO;
  current_props._measure   = 0;
  current_props._fft_mode  = FFT_OFF;
  current_props.tau = 50;
  current_props.decimation = 10;

//This data not loaded by default
//current_props._cal_data[5][POINTS_COUNT][2];
//Checksum add on caldata_save
//current_props.checksum = 0;
}

//
// Backup registers support, allow save data on power off (while vbat power enabled)
//
#ifdef __USE_BACKUP__
#if SWEEP_POINTS_MAX > 511 || SAVEAREA_MAX > 15
#error "Check backup data limits!!"
#endif

// backup_0 bitfield
typedef union {
  struct {
    uint32_t points     : 9; //  9 !! limit 511 points!!
    uint32_t bw         : 9; // 18 !! limit 511
    uint32_t id         : 4; // 22 !! 15 save slots
    uint32_t leveler    : 3; // 25
    uint32_t brightness : 7; // 32
  };
  uint32_t v;
} backup_0;

void update_backup_data(void) {
  backup_0 bk = {
    .points     = sweep_points,
    .bw         = config._bandwidth,
    .id         = lastsaveid,
    .leveler    = lever_mode,
    .brightness = config._brightness
  };
  set_backup_data32(0, bk.v);
  set_backup_data32(1, frequency0);
  set_backup_data32(2, frequency1);
  set_backup_data32(3, var_freq);
  set_backup_data32(4, config._vna_mode);
}

static void load_settings(void) {
  if (config_recall() == 0 && VNA_MODE(VNA_MODE_BACKUP)) { // Config loaded ok and need restore backup
    backup_0 bk = {.v = get_backup_data32(0)};
    if (bk.v != 0) {                                             // if backup data valid
      if (bk.id < SAVEAREA_MAX && caldata_recall(bk.id) == 0) {  // Slot valid and Load ok
        sweep_points = bk.points;                                // Restore settings depend from calibration data
        frequency0 = get_backup_data32(1);
        frequency1 = get_backup_data32(2);
        var_freq   = get_backup_data32(3);
      } else
        caldata_recall(0);
      // Here need restore settings not depend from cal data
      config._brightness = bk.brightness;
      lever_mode         = bk.leveler;
      config._vna_mode   = get_backup_data32(4) | (1<<VNA_MODE_BACKUP); // refresh backup settings
      set_bandwidth(bk.bw);
    }
  }
  else
    caldata_recall(0);
  update_frequencies();
#ifdef __VNA_MEASURE_MODULE__
  plot_set_measure_mode(current_props._measure);
#endif
}
#else
static void load_settings(void) {
  config_recall();
  load_properties(0);
}
#endif

int load_properties(uint32_t id) {
  int r = caldata_recall(id);
  update_frequencies();
#ifdef __VNA_MEASURE_MODULE__
  plot_set_measure_mode(current_props._measure);
#endif
  return r;
}

#ifdef ENABLED_DUMP_COMMAND
audio_sample_t *dump_buffer;
volatile int16_t dump_len = 0;
int16_t dump_selection = 0;
static void
duplicate_buffer_to_dump(audio_sample_t *p, size_t n)
{
  p+=dump_selection;
  while (n) {
    if (dump_len == 0) return;
    dump_len--;
    *dump_buffer++ = *p;
    p+=2;
    n-=2;
  }
}
#endif

// DMA i2s callback function, called on get 'half' and 'full' buffer size data need for process data, while DMA fill next buffer
static systime_t ready_time = 0;
// sweep operation variables
volatile uint16_t wait_count = 0;
volatile uint16_t dirty_count = 0;
//volatile uint16_t tau_count = 0;
volatile uint16_t tau_current = 0;
//uint16_t decimation_current = 0;
// i2s buffer must be 2x size (for process one while next buffer filled by DMA)
static audio_sample_t rx_buffer[AUDIO_BUFFER_LEN * 2];
static audio_sample_t *filled_buffer;
volatile uint16_t dsp_ready = 0;

#if 1
#define LOG_SIZE 256
int speed_index = 0;
systime_t speed_log[LOG_SIZE];
systime_t log_prev_time;
float log_prev_phase;
#endif



void i2s_lld_serve_rx_interrupt(uint32_t flags) {
#if 0
  systime_t cur_time = chVTGetSystemTimeX();
  systime_t delta_time = cur_time - log_prev_time;
  log_prev_time = cur_time;
  float cur_phase, delta_phase;
  speed_log[speed_index++] =  delta_time;
  log_prev_time = cur_time;
  if (speed_index >= LOG_SIZE) speed_index = 0;
  if (delta_time > 1000 && p_sweep >0) {
    volatile int kk;
    kk++;
  }
#endif
  if (wait_count == 0 || chVTGetSystemTimeX() < ready_time) {
    reset_dsp_accumerator();
    wait_count = config._bandwidth;
    if (tau_current == 0) {
      tau_current = current_props.tau;
      reset_averaging();
    }
  }
  uint16_t count = AUDIO_BUFFER_LEN;
  audio_sample_t *p = (flags & STM32_DMA_ISR_TCIF) ? rx_buffer + AUDIO_BUFFER_LEN : rx_buffer; // Full or Half transfer complete
  dsp_process(p, count);            // Accumulate each audio buffer in downconverted I and Q
#ifdef ENABLED_DUMP_COMMAND
  duplicate_buffer_to_dump(p, count);
#endif
  filled_buffer = p;
  --wait_count;

  if (wait_count == 0) {
    if (!(props_mode & TD_SAMPLE) && !(props_mode & TD_PNA)) {
      calculate_vectors();      // Convert I/Q into angle and sum angles
    }
    -- tau_current;
    if (tau_current == 0) {
      if (props_mode & TD_SAMPLE){
        dsp_ready = true;
      } else if (props_mode & TD_PNA) {
        calculate_subsamples(temp_measured[temp_input++], current_props.tau);
        temp_input &= TEMP_MASK;
        if (temp_input == temp_output) {
          temp_output = temp_input;   // Remove all cached samples
          missing_samples = true;
        }
        dsp_ready = true;

      } else {
//        aver_freq_a = get_freq_a();
//        aver_freq_b = get_freq_b();
//        aver_freq_delta = get_freq_delta();
        if (!VNA_MODE(VNA_MODE_WIDE)) {
        calculate_gamma(temp_measured[temp_input], current_props.tau);              // Calculate average angles and store in temp_measured
        aver_freq_a = temp_measured[temp_input][A_FREQ];
        aver_freq_b = temp_measured[temp_input][B_FREQ];
        aver_freq_delta = temp_measured[temp_input][D_FREQ];
        }
        temp_input++;
        temp_input &= TEMP_MASK;
        if (temp_input == temp_output) {
#if 0
          temp_input--;
          temp_input &= TEMP_MASK; // Delete sample.
#else
          temp_output = temp_input;   // Remove all cached samples
          missing_samples = true;
#endif
        }
        dsp_ready = true;
      }
    }
  }
//  palClearPad(GPIOA, GPIOA_PA4);
}

#ifdef ENABLE_SI5351_TIMINGS
extern uint16_t timings[16];
#undef DELAY_CHANNEL_CHANGE
#undef DELAY_SWEEP_START
#define DELAY_CHANNEL_CHANGE  timings[3]
#define DELAY_SWEEP_START     timings[4]
#endif

//#define DSP_START(delay) {ready_time = chVTGetSystemTimeX() + delay; wait_count = config._bandwidth+2;}
//#define DSP_PREWAIT         while (wait_count == 0 && !(operation_requested && break_on_operation)) {__WFI();}
//#define DSP_WAIT         while (wait_count && !(operation_requested && break_on_operation)) {__WFI();}
#define DSP_PREWAIT      dsp_ready = false
#define DSP_WAIT         while ((!dsp_ready) && temp_input == temp_output && !(operation_requested && break_on_operation)) {  /*  palClearPad(GPIOA, GPIOA_PA4); */ __WFI(); /*  palSetPad(GPIOA, GPIOA_PA4); */ }
#define RESET_SWEEP      {p_sweep = 0; temp_output = temp_input;}

#define SWEEP_CH0_MEASURE           0x01
#define SWEEP_CH1_MEASURE           0x02
#define SWEEP_APPLY_EDELAY          0x04
#define SWEEP_APPLY_S21_OFFSET      0x08
#define SWEEP_APPLY_CALIBRATION     0x10
#define SWEEP_USE_INTERPOLATION     0x20
#define SWEEP_USE_RENORMALIZATION   0x40

static uint16_t get_sweep_mask(void){
  uint16_t ch_mask = 0;
#if 0
  // Sweep only used channels (FIXME strange bug in 400-500M on switch channels if only 1 Trace)
  int t;
  for (t = 0; t < TRACES_MAX; t++) {
    if (!trace[t].enabled)
      continue;
    if ((trace[t].channel&1) == 0) ch_mask|= SWEEP_CH0_MEASURE;
    else/*if (trace[t].channel == 1)*/ ch_mask|= SWEEP_CH1_MEASURE;
  }
#else
  // sweep 2 channels in any case
  ch_mask|= SWEEP_CH0_MEASURE|SWEEP_CH1_MEASURE;
#endif

#ifdef __VNA_MEASURE_MODULE__
  // For measure calculations need data
  ch_mask|= plot_get_measure_channels();
#endif
#ifdef __VNA_Z_RENORMALIZATION__
  if (current_props._portz != 50.0f)
    ch_mask|= SWEEP_USE_RENORMALIZATION;
#endif
  if (cal_status & CALSTAT_APPLY)        ch_mask|= SWEEP_APPLY_CALIBRATION;
  if (cal_status & CALSTAT_INTERPOLATED) ch_mask|= SWEEP_USE_INTERPOLATION;
  if (electrical_delay)                  ch_mask|= SWEEP_APPLY_EDELAY;
  if (s21_offset)                        ch_mask|= SWEEP_APPLY_S21_OFFSET;
  return ch_mask;
}

#if 1
float prev_v;

#define PHASE_BUCKETS  50
phase_t phase_correction[PHASE_BUCKETS];

void add_correction(phase_t phase, phase_t value)
{
  int index = (phase+0.5) * PHASE_BUCKETS;
  if (index > PHASE_BUCKETS-1)
    index = PHASE_BUCKETS -1;
  if (index < 0)
    index = 0;
//  if (phase_correction[index] == 0)
//    phase_correction[index] = value;
//  else
    phase_correction[index] = (phase_correction[index]*9 + value)/10;
}

void clear_correction(void)
{
  for (int i=0;i<PHASE_BUCKETS;i++)
    phase_correction[i] = 0;
}

phase_t get_correction(phase_t phase)
{
  int index = (phase+0.5) * PHASE_BUCKETS;
  if (index > PHASE_BUCKETS-1)
    index = PHASE_BUCKETS -1;
  if (index < 0)
    index = 0;
  return(phase_correction[index]);
}
#endif

uint32_t reg_n;
int32_t reg_xi;
phase_t reg_yi;
int32_t reg_sum_xi;
double reg_sum_yi;
double reg_sum_xiyi;
int32_t reg_sum_xi2;
double reg_sum_yi2;
volatile double res_freq, res_phase;
//double freq;

//int wrap;
phase_t reg_prev_phase, reg_start_phase;

void reset_regression(void) {
  reg_n = 0;
  reg_xi = reg_yi = 0;
  reg_sum_xi = reg_sum_yi = 0;
  reg_sum_xiyi = reg_sum_xi2 = reg_sum_yi2 = 0;
}

void add_regression(phase_t v) {
  phase_t delta_phase;
  reg_n++;
  if (reg_n != 1) {
    if (v> reg_prev_phase + 0.5) {          // Unwrap
      delta_phase = v - reg_prev_phase - 1;
//      wrap--;
    } else if (v < reg_prev_phase - 0.5) {
      delta_phase = v - reg_prev_phase + 1;
//      wrap++;
    } else
      delta_phase = v - reg_prev_phase;
  } else {
    reg_start_phase = v;
    reg_prev_phase = v;
    return;
  }
  reg_prev_phase = v;
//  v += wrap;
//  v -= reg_start_phase;

  reg_xi++;
  reg_yi += delta_phase;
  reg_sum_xi += reg_xi;
  reg_sum_yi += reg_yi;
  reg_sum_xiyi += ((double)reg_xi)*(double)reg_yi;
  reg_sum_xi2  += reg_xi*reg_xi;
  reg_sum_yi2  += ((double)reg_yi)*(double)reg_yi;
//  shell_printf("%d %d %f %d %f %f %d %f\r\n", reg_n, reg_xi, reg_yi, reg_sum_xi, reg_sum_yi, reg_sum_xiyi, reg_sum_xi2, reg_sum_yi2 );

}

phase_t finalize_regression(void) {
  volatile double freq = AUDIO_ADC_FREQ / (current_props.tau * (config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT );
  if(reg_n >=10) {
    reg_n--;
    int32_t reg_divisor = reg_n*((double)reg_sum_xi2) - ((double)reg_sum_xi)*((double)reg_sum_xi);
    if (reg_divisor == 0.0) goto no_regression;

    double fraction = (((double)reg_n)*(double)reg_sum_xiyi - ((double)reg_sum_xi) * (double)reg_sum_yi) / reg_divisor;
    res_freq = freq * fraction;
#if 0
    double stderryxn2 = ((double)reg_sum_yi2) - ((double)reg_sum_xiyi)/((double)reg_sum_xi2) *((double)reg_sum_xiyi);
    res_stderryx = sqrt(stderryxn2/(double)(reg_n-2)) * ((double) freq) / (double) reg_xi;
    //          double cycles = ( abs_time - reg_yi) * res_freq / ref_freq; // cycles till start of measurement
#endif
    res_phase = ((double)reg_sum_yi* (double) reg_sum_xi2 - ((double)reg_sum_xi) * (double)reg_sum_xiyi)/ reg_divisor;
  } else {
no_regression:
    res_freq = ((double)reg_xi)/ freq;
    res_freq = (double)reg_yi / res_freq;
    res_phase = 0;
  }
  return(res_freq);
}

#define abs_diff(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))

void do_agc(void)
{

  if (current_props._fft_mode  != FFT_OFF) {
    l_gain = 0;
    r_gain = 0;
    tlv320aic3204_set_gain(l_gain, r_gain);
    return;
  }
  int old_l_gain = l_gain, old_r_gain = r_gain;
  // Get level excluding gain.
  l_gain = 0;
  r_gain = 0;
  get_value_cb_t calc;
  calc = trace_info_list[TRC_ALOGMAG].get_value_cb;
  level_a = calc(p_sweep, measured[0]);                                          // Get value
  calc = trace_info_list[TRC_BLOGMAG].get_value_cb;
  level_b = calc(p_sweep, measured[0]);                                          // Get value
  l_gain = old_l_gain;
  r_gain = old_r_gain;
#ifdef NANOVNA_F303
#define ADC_TARGET_LEVEL    12
#else
#define ADC_TARGET_LEVEL    -30
#endif
  int new_l_gain = l_gain - 2.0*(level_a - (ADC_TARGET_LEVEL));
  int new_r_gain = r_gain - 2.0*(level_b - (ADC_TARGET_LEVEL));
  if (new_l_gain < 0) new_l_gain = 0;
  if (new_l_gain > 90) new_l_gain = 90;
  if (new_r_gain < 0) new_r_gain = 0;
  if (new_r_gain > 90) new_r_gain = 90;
  if (abs_diff(old_l_gain, new_l_gain) > 2 || abs_diff(old_r_gain, new_r_gain) > 2) {
    l_gain = new_l_gain;
    r_gain = new_r_gain;
    tlv320aic3204_set_gain(l_gain, r_gain);
  }
  // Get level including gain
  calc = trace_info_list[TRC_ALOGMAG].get_value_cb;
  level_a = calc(p_sweep, measured[p_sweep-1]);                                          // Get value
  calc = trace_info_list[TRC_BLOGMAG].get_value_cb;
  level_b = calc(p_sweep, measured[p_sweep-1]);                                          // Get value
#ifdef SIDE_CHANNEL
  calc = trace_info_list[TRC_SALOGMAG].get_value_cb;
  level_sa = calc(p_sweep, measured[p_sweep-1]);                                          // Get value
  calc = trace_info_list[TRC_SBLOGMAG].get_value_cb;
  level_sb = calc(p_sweep, measured[p_sweep-1]);                                          // Get value
#endif
}

void reset_phase_unwrap(void) {
  phase_wraps = 0;
  prev_phase = 0;
}

// main loop for measurement
static bool sweep(bool break_on_operation, uint16_t mask)
{
//  palSetPad(GPIOA, GPIOA_PA4);
  if (VNA_MODE(VNA_MODE_USB_LOG) && VNA_MODE(VNA_MODE_FREEZE_DISPLAY)) {
    RESET_SWEEP;
//    int dirty = 2;
//    float aver_freq=0;
//    float phase, prev_phase=0, freq=0, residue=0, aver_residue=0;
//    int wraps = 0;
//    int sample_count=0;
    clear_correction();
    while (1) {
//      palClearPad(GPIOC, GPIOC_LED);
      DSP_PREWAIT;
      DSP_WAIT;
      if (operation_requested && break_on_operation)
        break;

//      palSetPad(GPIOC, GPIOC_LED);
      // sample_count =
//      calculate_gamma(measured[p_sweep]);              // Measure transmission coefficient
//      palClearPad(GPIOC, GPIOC_LED);
//      int t = 0;
//      uint8_t type = trace[t].type;
      phase_t *array = &temp_measured[temp_output++][0];     // p_sweep is always zero
 //     shell_printf("out %d\r\n", temp_output);

      temp_output &= TEMP_MASK;
      //  const char *format = index_ref >= 0 ? trace_info_list[type].dformat : trace_info_list[type].format; // Format string
      get_value_cb_t calc = trace_info_list[GET_DPHASE].get_value_cb;
      if (calc){                                           // Run standard get value function from table
        //      float v = 0;
        //      for (int i =0; i<sweep_points; i++) {
        phase_t v = calc(0, array);                                          // Get value
        v /= 360;

#if 0
        systime_t cur_time = chVTGetSystemTimeX();
        systime_t delta_time = cur_time - log_prev_time;
        speed_log[speed_index++] =  delta_time;
         log_prev_time = cur_time;
         if (speed_index >= LOG_SIZE) speed_index = 0;
         if (delta_time > 1000 && p_sweep >0) {
      volatile int kk;
           kk++;
         }
#endif

//#define  RUNTIME_RESIDUE

#ifdef RUNTIME_RESIDUE
         phase = v;
         if (dirty == 2) {
           wraps = 0;
         } else {
           if (phase + wraps > res_prev_phase + 0.5) {          // Unwrap
             wraps--;
           }
           if (phase + wraps < res_prev_phase - 0.5) {
             wraps++;
           }
           phase += wraps;

           freq =  (phase - res_prev_phase) * AUDIO_ADC_FREQ;
           freq /= current_props.tau*(config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT;

           if (dirty == 1) {
             aver_freq = freq;
           } else {
             aver_freq = (aver_freq * 199 + freq)/200;
           }


           residue = phase - (sample_count++) * aver_freq * current_props.tau*(config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT / AUDIO_ADC_FREQ;
           if (sample_count == 190)
             aver_residue = residue;
#if 0
           if (dirty == 1) {
             aver_residue = residue;
           } else {
             aver_residue = (aver_residue * 199.0 + residue ) / 200.0;
           }

           if (sample_count == 100) {
             aver_aver_residue = residue-aver_residue;
           } else if (sample_count > 100){
             aver_aver_residue = ((aver_aver_residue) * 99.0 + residue-aver_residue ) / 100.0;
           }
#endif

         }
         if (dirty >0)
           dirty--;
         if (sample_count > 200)
           add_correction(v,residue-aver_residue);
         res_prev_phase = phase;
#endif
        //      }
        //      v = v/sweep_points;

//#ifdef SIDE_CHANNEL
//        shell_printf("%f ChA\r\n", v);
//        float v2 = temp_measured[temp_output][0]/2;
//        shell_printf("%f ChB\r\n", v-v2);
//#else
        shell_printf("%f\r\n", v);
//#endif
      }
      if (operation_requested && break_on_operation)
        break;
    }
    goto return_false;

  } else {
fetch_next:
  if (break_on_operation && mask == 0)
    goto return_false;
//  START_PROFILE;
  lcd_set_background(LCD_SWEEP_LINE_COLOR);
  int bar_start = 0;
//  freq_t frequency = getFrequency(p_sweep);
//  set_frequency(frequency);
//  tlv320aic3204_select(1);
//  chThdSleepMilliseconds(100);
//  DSP_START(delay+st_delay);


  if (! VNA_MODE(VNA_MODE_SCROLLING)) {
    RESET_SWEEP;
  }
  requested_points = sweep_points;
  if (current_props._fft_mode != FFT_OFF)
    requested_points = FFT_SIZE;
//  float res_prev_phase = 0;

  for (; p_sweep < requested_points; /* p_sweep++ */) {
//     palSetPad(GPIOC, GPIOC_LED);
//     DSP_START(0);
     DSP_PREWAIT;
      DSP_WAIT;
      if (operation_requested && break_on_operation)
        break;

      if (props_mode & TD_SAMPLE) {
//        p_sweep = 0;
        while (p_sweep < sweep_points /* && p_sweep < AUDIO_SAMPLES_COUNT */ ) {
          measured[p_sweep][0] = (float)*filled_buffer++;
          measured[p_sweep][1] = (float)*filled_buffer++;
          p_sweep++;
        }
        if (p_sweep == sweep_points) {
          do_agc();
          goto return_true;
        }
        continue;
      } else if (props_mode & TD_PNA) {
        while (p_sweep < sweep_points /* && p_sweep < AUDIO_SAMPLES_COUNT */ ) {
          measured[p_sweep][0] = temp_measured[temp_output][2];
          measured[p_sweep][1] = temp_measured[temp_output++][3];
          temp_output &= TEMP_MASK;
          p_sweep++;
        }
        if (p_sweep == sweep_points) {
          goto return_true;
        }
        continue;
      }

      if (VNA_MODE(VNA_MODE_DISK_LOG) || VNA_MODE(VNA_MODE_USB_LOG)) {
        double log_output;
        if (current_props.log_type == LOG_FREQUENCY) {
          log_output = temp_measured[temp_output][D_FREQ];
        } else if (current_props.log_type == LOG_PHASE) {
          log_output = temp_measured[temp_output][D_PHASE]/2;
        } else {            // Unwrapped phase
          double phase = temp_measured[temp_output][D_PHASE];
          double unwrapped_phase = phase + phase_wraps;     // Unwrap
          double delta_phase = unwrapped_phase - prev_phase; // Update phase_wraps
          if (delta_phase > HALF_PHASE) {
            phase_wraps -= FULL_PHASE;
            unwrapped_phase -= FULL_PHASE;
          }
          if (delta_phase < -HALF_PHASE) {
            phase_wraps += FULL_PHASE;
            unwrapped_phase += FULL_PHASE;
          }
          prev_phase = unwrapped_phase;
          freq_t f = get_sweep_frequency(ST_START);
          log_output = unwrapped_phase / 2 / f;       // scaled to frequency
        }


        //      float v = temp_measured[temp_output][D_PHASE]/2;
        if (VNA_MODE(VNA_MODE_DISK_LOG))
          disk_log(log_output);
        if (VNA_MODE(VNA_MODE_USB_LOG)) {
          if (current_props.log_type == LOG_UNWRAPPED_PHASE)
            shell_printf("%.12e ChA\r\n", log_output);
          else
            shell_printf("%f ChA\r\n", (float)log_output);
#ifdef SIDE_CHANNEL
          if (VNA_MODE(VNA_MODE_DUMP_SIDE)) {
            float v2 = temp_measured[temp_output][S_PHASE]/2;
            shell_printf("%e ChB\r\n", v2);
          }
#endif
        }
      }

      if (current_props._fft_mode == FFT_PHASE) {
 //       float* tmp  = (float*)spi_buffer;
 //       tmp[p_sweep * 2 + 0] = sinf(phase * VNA_PI / 2);
 //       tmp[p_sweep * 2 + 1] = cosf(phase * VNA_PI / 2);
//        shell_printf("%d %f %f %f\r\n", p_sweep,  tmp[p_sweep * 2 + 0]);
      }
      else if (current_props._fft_mode == FFT_AMP || current_props._fft_mode == FFT_B) {
#if 0
        float* tmp  = (float*)spi_buffer;
        tmp[p_sweep * 2 + 0] = temp_measured[temp_output][1];
        tmp[p_sweep * 2 + 1] = 0;
#endif
//        shell_printf("%d %f %f %f\r\n", p_sweep,  tmp[p_sweep * 2 + 0]);
      } else
      if (p_sweep < sweep_points) {
#ifdef SIDE_CHANNEL
        measured[p_sweep][S_PHASE] = temp_measured[temp_output][S_PHASE];
#endif
        measured[p_sweep][A_PHASE] = temp_measured[temp_output][A_PHASE];
        measured[p_sweep][D_PHASE] = temp_measured[temp_output][D_PHASE];
        measured[p_sweep][A_FREQ] = temp_measured[temp_output][A_FREQ];
        measured[p_sweep][B_FREQ] = temp_measured[temp_output][B_FREQ];
        measured[p_sweep][D_FREQ] = temp_measured[temp_output][D_FREQ];
      }
      if (current_props._fft_mode != FFT_AMP && current_props._fft_mode != FFT_B  && current_props._fft_mode != FFT_PHASE) p_sweep++;
      temp_output++;
      temp_output &= TEMP_MASK;
//      shell_printf("out %d\r\n", temp_output);
      if (VNA_MODE(VNA_MODE_SCROLLING))
        if (temp_input != temp_output && !(operation_requested && break_on_operation))
          goto do_compress;

//      shell_printf("%d %f %f %f\r\n", p_sweep,  measured[p_sweep-1][1],  measured[p_sweep-1][2],  measured[p_sweep-1][3]);

      //================================================
      // Place some code thats need execute while delay
      //================================================

      // else
      // sample_count =
      //        calculate_gamma(measured[p_sweep]);              // Measure all


      if (VNA_MODE(VNA_MODE_SCROLLING) && p_sweep >=2) break;
      if (operation_requested && break_on_operation) break;
  }
  requested_points = 0; // Stop filling of SPI buffer
  if (p_sweep >= sweep_points)
    p_sweep = sweep_points;
  if (bar_start){
    lcd_set_background(LCD_GRID_COLOR);
    lcd_fill(OFFSETX+CELLOFFSETX, OFFSETY, bar_start, 1);
  }
  if (operation_requested && break_on_operation) goto return_false;
//  palSetPad(GPIOA, GPIOA_PA4);
  // -------------------- Average D freq  ----------------------

  if (current_props.tau <= 10) {
    get_value_cb_t calc = trace_info_list[GET_DFREQ].get_value_cb; // dfreq port 1
    phase_t (*array)[MAX_MEASURED] = measured;
    //  const char *format = index_ref >= 0 ? trace_info_list[type].dformat : trace_info_list[type].format; // Format string
    phase_t v = 0;
    for (int i =0; i<sweep_points-1; i++) {
      v += calc(i, array[i]);                                          // Get value
    }
    v = v/(sweep_points-1);

    aver_freq_d = v;
    calc = trace_info_list[GET_DFREQ].get_value_cb; // dfreq port 1
    last_freq_d = calc(sweep_points-2, array[sweep_points-2]);
  }
  else
  {
    reset_regression();
    get_value_cb_t calc = trace_info_list[GET_DPHASE].get_value_cb; // dfreq port 1
    phase_t (*array)[MAX_MEASURED] = measured;
    //  const char *format = index_ref >= 0 ? trace_info_list[type].dformat : trace_info_list[type].format; // Format string
    for (int i =0; i<p_sweep; i++) {
      phase_t v = calc(i, array[i])/360;                                          // Get value
      add_regression(v);
    }
    aver_freq_d = finalize_regression();
    //   shell_printf("%f\r\n", aver_freq_d);

    calc = trace_info_list[GET_DFREQ].get_value_cb; // dfreq port 1
    last_freq_d = calc(p_sweep-2, array[p_sweep-2]);
  }



  // -------------------- Auto set CW frequency ----------------------
  phase_t (*array)[MAX_MEASURED];
  get_value_cb_t calc;
#if 0
  calc = trace_info_list[GET_AFREQ].get_value_cb; // dfreq port 1
  array = measured;
  //  const char *format = index_ref >= 0 ? trace_info_list[type].dformat : trace_info_list[type].format; // Format string
  float v = 0;
  for (int i = (VNA_MODE(VNA_MODE_SCROLLING) ?  p_sweep-2: 0); i<p_sweep-1; i++) {
    v += calc(i, array[i]);                                          // Get value
  }
  if (!VNA_MODE(VNA_MODE_SCROLLING))
    v = v/(p_sweep-1);
  aver_freq_a = v;
#endif
  float v = aver_freq_a;
  if (current_props._fft_mode != FFT_OFF) {
    current_props.pll = 0;
    set_frequency(get_sweep_frequency(ST_START));       // This will update using the new pll value
  } else if (VNA_MODE(VNA_MODE_PLL)) {
    if (level_a > MIN_LEVEL && level_b > MIN_LEVEL /* && !(VNA_MODE(VNA_MODE_DISK_LOG) || VNA_MODE(VNA_MODE_USB_LOG))*/ ) {
      volatile float new_pll;
      float factor = PLL_SCALE;
//    if (VNA_MODE(VNA_MODE_SCROLLING))
//      factor *= get_tau();
      if (-0.05 < v && v < 0.05)
        v /= 3;       // Slow speed when close
      new_pll = current_props.pll - v * factor * 10000000.0 / (float) get_sweep_frequency(ST_CENTER); // Scale is normalized to 10MHz
      if (new_pll < 30000 && new_pll > -30000 && ((current_props.pll - new_pll) > 0.5 || (current_props.pll - new_pll) < -0.5)) {
        current_props.pll = new_pll;
        set_frequency(get_sweep_frequency(ST_START));       // This will update using the new pll value
//        shell_printf("<>%.3f %.2f\r\n", 1000*aver_freq_a, new_pll);
      }
//      else shell_printf("==%.3f %.2f\r\n", 1000*aver_freq_a, new_pll);
    } else {
      current_props.pll = 0;
      set_frequency(get_sweep_frequency(ST_START));       // This will update using the new pll value
    }
  }
  if (!(operation_requested && break_on_operation))
  {
    do_agc();
  }



#if 1
  calc = trace_info_list[GET_DPHASE].get_value_cb; // dfreq port 1
  array = measured;
  v = 0;
  for (int i=0; i<p_sweep; i++) {
    v += calc(i, array[i]);                                          // Get value
  }
  v = v/p_sweep;
  aver_phase_d = v;
  last_phase_d = calc(p_sweep-1, array[p_sweep-1]);
#endif

  if (VNA_MODE(VNA_MODE_SCROLLING)) {
  do_compress:
    if (p_sweep == sweep_points) {
      for (int i=0;i<sweep_points-1;i++) {
#ifdef SIDE_CHANNEL
        measured[i][S_PHASE] = measured[i+1][S_PHASE];
#endif
//        measured[i][B_PHASE] = measured[i+1][B_PHASE];
        measured[i][A_PHASE] = measured[i+1][A_PHASE];
        measured[i][D_PHASE] = measured[i+1][D_PHASE];
        measured[i][A_FREQ] = measured[i+1][A_FREQ];
        measured[i][B_FREQ] = measured[i+1][B_FREQ];
        measured[i][D_FREQ] = measured[i+1][D_FREQ];
      }
      p_sweep--;
    }
    if (temp_input != temp_output && !(operation_requested && break_on_operation))
      goto fetch_next;
    goto return_true;
  }

  //  STOP_PROFILE;
  // blink LED while scanning
//  palClearPad(GPIOA, GPIOA_PA4);
  return p_sweep == sweep_points;
  }
return_false:
//  palClearPad(GPIOA, GPIOA_PA4);
  return false;
return_true:
  if (old_p_sweep != p_sweep) {
    old_p_sweep = p_sweep;
    request_to_redraw(REDRAW_FREQUENCY);
  }
//  palClearPad(GPIOA, GPIOA_PA4);
  return true;
}

#ifdef ENABLED_DUMP_COMMAND
VNA_SHELL_FUNCTION(cmd_dump)
{
  int i, j;
  audio_sample_t dump[96*2];
  dump_buffer = dump;
  dump_len = ARRAY_COUNT(dump);
  int len = dump_len;
  if (argc == 1)
    dump_selection = my_atoi(argv[0]) == 1 ? 0 : 1;

//  tlv320aic3204_select(0);
  DSP_START(DELAY_SWEEP_START);
  while (dump_len > 0) {__WFI();}
  for (i = 0, j = 0; i < len; i++) {
    shell_printf("%6d ", dump[i]);
    if (++j == 12) {
      shell_printf(VNA_SHELL_NEWLINE_STR);
      j = 0;
    }
  }
}
#endif

#ifdef ENABLE_GAIN_COMMAND
VNA_SHELL_FUNCTION(cmd_gain)
{
  int rvalue = 0;
  int lvalue = 0;
  if (argc == 0 && argc > 2) {
    shell_printf("usage: gain {lgain(0-95)} [rgain(0-95)]" VNA_SHELL_NEWLINE_STR);
    return;
  };
  lvalue = rvalue = my_atoui(argv[0]);
  if (argc == 3)
    rvalue = my_atoui(argv[1]);
  tlv320aic3204_set_gain(lvalue, rvalue);
}
#endif

static int set_frequency(freq_t freq)
{
  return si5351_set_frequency(freq, current_props._power);
}


void set_bandwidth(uint16_t bw_count){
  float old_tau = get_tau();
  config._bandwidth = bw_count;
  set_tau(old_tau);
  request_to_redraw(REDRAW_BACKUP | REDRAW_FREQUENCY);
}

uint32_t get_bandwidth_frequency(uint16_t bw_freq){
  return (AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT)/(bw_freq+SAMPLE_OVERHEAD);
}

#define MIN_SAMPLES 1
#define TAU_SCALE 100000
void set_tau(float tau){
#if 0
  config._bandwidth = (tau * (float) AUDIO_ADC_FREQ / (float) AUDIO_SAMPLES_COUNT);
  if (config._bandwidth < MIN_SAMPLES + SAMPLE_OVERHEAD)
    config._bandwidth = MIN_SAMPLES;
  else
    config._bandwidth -= SAMPLE_OVERHEAD;
#else
  current_props.tau = (tau * (float) AUDIO_ADC_FREQ / (float) AUDIO_SAMPLES_COUNT + (config._bandwidth-1)) / config._bandwidth;
  if (current_props.tau < 1)
    current_props.tau = 1;
  if (current_props.decimation > current_props.tau)
    current_props.decimation = current_props.tau;
#endif
  RESET_SWEEP
#if 0
  if (! VNA_MODE(VNA_MODE_SCROLLING)) {
    sweep_points = (int) (1/tau);
    if (sweep_points < 2) sweep_points = 2;
    if (sweep_points > POINTS_COUNT) sweep_points = POINTS_COUNT;
    set_sweep_points(sweep_points);
  }
  else {
    if (sweep_points < 20) {
      set_sweep_points(POINTS_COUNT);
      request_to_redraw(REDRAW_BACKUP | REDRAW_FREQUENCY);
    }
  }
#endif
}

float get_tau(void){
  return ( current_props.tau * (config._bandwidth + SAMPLE_OVERHEAD)) * (float)AUDIO_SAMPLES_COUNT / (float) AUDIO_ADC_FREQ;
}

void reset_sweep(void) {
  RESET_SWEEP
}

#define MAX_BANDWIDTH      (AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT)
#define MIN_BANDWIDTH      ((AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT)/512 + 1)

VNA_SHELL_FUNCTION(cmd_bandwidth)
{
  uint16_t user_bw;
  if (argc == 1)
    user_bw = my_atoui(argv[0]);
  else if (argc == 2){
    uint16_t f = my_atoui(argv[0]);
         if (f > MAX_BANDWIDTH) user_bw = 0;
    else if (f < MIN_BANDWIDTH) user_bw = 511;
    else user_bw = ((AUDIO_ADC_FREQ+AUDIO_SAMPLES_COUNT/2)/AUDIO_SAMPLES_COUNT)/f - 1;
  }
  else
    goto result;
  set_bandwidth(user_bw);
result:
  shell_printf("bandwidth %d (%uHz)" VNA_SHELL_NEWLINE_STR, config._bandwidth, get_bandwidth_frequency(config._bandwidth));
}

void set_sweep_points(uint16_t points) {
  if (points > SWEEP_POINTS_MAX) points = SWEEP_POINTS_MAX;
  if (points < SWEEP_POINTS_MIN) points = SWEEP_POINTS_MIN;
  if (points == sweep_points)
    return;
  sweep_points = points;
  RESET_SWEEP;
  request_to_redraw(REDRAW_FREQUENCY);
  update_frequencies();
}

/*
 * Frequency list functions
 */
#ifdef __USE_FREQ_TABLE__
static freq_t frequencies[SWEEP_POINTS_MAX];
static void
set_frequencies(freq_t start, freq_t stop, uint16_t points)
{
  uint32_t i;
  freq_t step = (points - 1);
  freq_t span = stop - start;
  freq_t delta = span / step;
  freq_t error = span % step;
  freq_t f = start, df = step>>1;
  for (i = 0; i <= step; i++, f+=delta) {
    frequencies[i] = f;
    if ((df+=error) >= step) {f++; df-= step;}
  }
  // disable at out of sweep range
  for (; i < SWEEP_POINTS_MAX; i++)
    frequencies[i] = 0;
}
#define _c_start    frequencies[0]
#define _c_stop     frequencies[sweep_points-1]
#define _c_points   (sweep_points)

freq_t getFrequency(uint16_t idx) {return frequencies[idx];}
#else
static freq_t   _f_start;
static freq_t   _f_delta;
static freq_t   _f_error;
static uint16_t _f_points;

static void
set_frequencies(freq_t start, freq_t stop, uint16_t points)
{
  freq_t span = stop - start;
  _f_start  = start;
  _f_points = (points - 1);
  _f_delta  = span / _f_points;
  _f_error  = span % _f_points;
}
freq_t getFrequency(uint16_t idx) {return _f_start + _f_delta * idx + (_f_points / 2 + _f_error * idx) / _f_points;}
freq_t getFrequencyStep(void) {return _f_delta;}
#endif

static bool needInterpolate(freq_t start, freq_t stop, uint16_t points){
  return start != cal_frequency0 || stop != cal_frequency1 || points != cal_sweep_points;
}

#if 0
#define SCAN_MASK_OUT_FREQ       0b00000001
#define SCAN_MASK_OUT_DATA0      0b00000010
#define SCAN_MASK_OUT_DATA1      0b00000100
#define SCAN_MASK_NO_CALIBRATION 0b00001000
#define SCAN_MASK_NO_EDELAY      0b00010000
#define SCAN_MASK_NO_S21OFFS     0b00100000
#define SCAN_MASK_BINARY         0b10000000

VNA_SHELL_FUNCTION(cmd_scan)
{
  freq_t start, stop;
  uint16_t points = sweep_points;
  if (argc < 2 || argc > 4) {
    shell_printf("usage: scan {start(Hz)} {stop(Hz)} [points] [outmask]" VNA_SHELL_NEWLINE_STR);
    return;
  }

  start = my_atoui(argv[0]);
  stop = my_atoui(argv[1]);
  if (start == 0 || stop == 0 || start > stop) {
      shell_printf("frequency range is invalid" VNA_SHELL_NEWLINE_STR);
      return;
  }
  if (argc >= 3) {
    points = my_atoui(argv[2]);
    if (points == 0 || points > SWEEP_POINTS_MAX) {
      shell_printf("sweep points exceeds range " define_to_STR(SWEEP_POINTS_MAX) VNA_SHELL_NEWLINE_STR);
      return;
    }
    sweep_points = points;
  }
  uint16_t mask = 0;
  uint16_t sweep_ch = SWEEP_CH0_MEASURE|SWEEP_CH1_MEASURE;

#ifdef ENABLE_SCANBIN_COMMAND
  if (argc == 4) {
    mask = my_atoui(argv[3]);
    if (sweep_mode&SWEEP_BINARY) mask|=SCAN_MASK_BINARY;
    sweep_ch = (mask>>1)&3;
  }
  sweep_mode&=~(SWEEP_BINARY);
#else
  if (argc == 4) {
    mask = my_atoui(argv[3]);
    sweep_ch = (mask>>1)&3;
  }
#endif

  if ((cal_status & CALSTAT_APPLY) && !(mask&SCAN_MASK_NO_CALIBRATION)) sweep_ch|= SWEEP_APPLY_CALIBRATION;
  if (electrical_delay             && !(mask&SCAN_MASK_NO_EDELAY     )) sweep_ch|= SWEEP_APPLY_EDELAY;
  if (s21_offset                   && !(mask&SCAN_MASK_NO_S21OFFS    )) sweep_ch|= SWEEP_APPLY_S21_OFFSET;

  if (needInterpolate(start, stop, sweep_points))
    sweep_ch|= SWEEP_USE_INTERPOLATION;

  sweep_points = points;
  set_frequencies(start, stop, points);
  if (sweep_ch & (SWEEP_CH0_MEASURE|SWEEP_CH1_MEASURE))
    sweep(false, sweep_ch);
  pause_sweep();
  // Output data after if set (faster data receive)
  if (mask) {
    if (mask&SCAN_MASK_BINARY){
      shell_write(&mask, sizeof(uint16_t));
      shell_write(&points, sizeof(uint16_t));
      for (int i = 0; i < points; i++) {
        if (mask & SCAN_MASK_OUT_FREQ ) {freq_t f = getFrequency(i); shell_write(&f, sizeof(freq_t));} // 4 bytes .. frequency
        if (mask & SCAN_MASK_OUT_DATA0) shell_write(&measured[i][0], sizeof(float)* 2);             // 4+4 bytes .. S11 real/imag
      }
    } else {
      for (int i = 0; i < points; i++) {
        if (mask & SCAN_MASK_OUT_FREQ ) shell_printf(VNA_FREQ_FMT_STR " ", getFrequency(i));
        if (mask & SCAN_MASK_OUT_DATA0) shell_printf("%f %f ", measured[i][0], measured[i][1]);
         shell_printf(VNA_SHELL_NEWLINE_STR);
      }
    }
  }
}
#endif

#ifdef ENABLE_SCANBIN_COMMAND
VNA_SHELL_FUNCTION(cmd_scan_bin)
{
  (void)argc;
  (void)argv;
  sweep_mode|= SWEEP_BINARY;
//  cmd_scan(argc, argv);
  sweep_mode&=~(SWEEP_BINARY);
}
#endif

VNA_SHELL_FUNCTION(cmd_tcxo)
{
  if (argc != 1) {
    shell_printf("usage: tcxo {TCXO frequency(Hz)}" VNA_SHELL_NEWLINE_STR \
                 "current: %u" VNA_SHELL_NEWLINE_STR, config._xtal_freq);
    return;
  }
  si5351_set_tcxo(my_atoui(argv[0]));
}

void set_marker_index(int m, int idx)
{
  if (m == MARKER_INVALID || (uint32_t)idx >= sweep_points) return;
  markers[m].frequency = getFrequency(idx);
  if (markers[m].index == idx) return;
  request_to_draw_marker(markers[m].index); // Mark old marker position for erase
  markers[m].index = idx;                   // Set new position
  request_to_redraw(REDRAW_MARKER);
}

freq_t get_marker_frequency(int marker)
{
  if ((uint32_t)marker >= MARKERS_MAX)
    return 0;
  return markers[marker].frequency;
}
#if 0
static void
update_marker_index(freq_t fstart, freq_t fstop, uint16_t points)
{
  int m, idx;
  for (m = 0; m < MARKERS_MAX; m++) {
    // Update index for all markers !!
    freq_t f = markers[m].frequency;
    if (f == 0) idx = markers[m].index; // Not need update index in no freq
    else if (f <= fstart) idx = 0;
    else if (f >= fstop ) idx = points-1;
    else { // Search frequency index for marker frequency
#if 0
      for (idx = 1; idx < points; idx++) {
        if (frequencies[idx] <= f) continue;
        if (f < (frequencies[idx-1]/2 + frequencies[idx]/2)) idx--; // Correct closest idx
        break;
      }
#else
      float r = ((float)(f - fstart))/(fstop - fstart);
      idx = r * (points-1);
#endif
    }
    set_marker_index(m, idx);
  }
}
#endif
void
update_frequencies(void)
{
  freq_t start = get_sweep_frequency(ST_START);
  freq_t stop  = get_sweep_frequency(ST_STOP);

  set_frequencies(start, stop, sweep_points);

  set_frequency(start);
  chThdSleepMilliseconds(100);

  //update_marker_index();
  // set grid layout
  update_grid(start, stop);
  // Update interpolation flag
  if (needInterpolate(start, stop, sweep_points))
    cal_status|= CALSTAT_INTERPOLATED;
  else
    cal_status&= ~CALSTAT_INTERPOLATED;

  request_to_redraw(REDRAW_BACKUP | REDRAW_PLOT | REDRAW_CAL_STATUS | REDRAW_FREQUENCY | REDRAW_AREA);
//  RESET_SWEEP;
}

void
set_sweep_frequency(uint16_t type, freq_t freq)
{
  // Check frequency for out of bounds (minimum SPAN can be any value)
  if (type < ST_SPAN && freq < FREQUENCY_MIN)
    freq = FREQUENCY_MIN;
  if (freq > FREQUENCY_MAX)
    freq = FREQUENCY_MAX;
  freq_t center, span;
  switch (type) {
    case ST_START:
      FREQ_STARTSTOP();
      frequency0 = freq;
      // if start > stop then make start = stop
      if (frequency1 < freq) frequency1 = freq;
      break;
    case ST_STOP:
      FREQ_STARTSTOP()
      frequency1 = freq;
        // if start > stop then make start = stop
      if (frequency0 > freq) frequency0 = freq;
      break;
    case ST_CENTER:
      FREQ_CENTERSPAN();
      center = freq;
      span   = (frequency1 - frequency0)>>1;
      if (span > center - FREQUENCY_MIN)
        span = (center - FREQUENCY_MIN);
      if (span > FREQUENCY_MAX - center)
        span = (FREQUENCY_MAX - center);
      frequency0 = center - span;
      frequency1 = center + span;
      break;
    case ST_SPAN:
      FREQ_CENTERSPAN();
      center = (frequency0>>1) + (frequency1>>1);
      span = freq>>1;
      if (center < FREQUENCY_MIN + span)
        center = FREQUENCY_MIN + span;
      if (center > FREQUENCY_MAX - span)
        center = FREQUENCY_MAX - span;
      frequency0 = center - span;
      frequency1 = center + span;
      break;
    case ST_CW:
      FREQ_CENTERSPAN();
      frequency0 = freq;
      frequency1 = freq;
      break;
    case ST_VAR:
      var_freq = freq;
      request_to_redraw(REDRAW_BACKUP);
      return;
  }
  update_frequencies();
}

void reset_sweep_frequency(void){
  frequency0 = cal_frequency0;
  frequency1 = cal_frequency1;
  sweep_points = cal_sweep_points;
  update_frequencies();
}

VNA_SHELL_FUNCTION(cmd_sweep)
{
  if (argc == 0) {
    shell_printf(VNA_FREQ_FMT_STR " " VNA_FREQ_FMT_STR " %d" VNA_SHELL_NEWLINE_STR, get_sweep_frequency(ST_START), get_sweep_frequency(ST_STOP), sweep_points);
    return;
  } else if (argc > 3) {
    goto usage;
  }
  freq_t   value0 = 0;
  freq_t   value1 = 0;
  uint32_t value2 = 0;
  if (argc >= 1) value0 = my_atoui(argv[0]);
  if (argc >= 2) value1 = my_atoui(argv[1]);
  if (argc >= 3) value2 = my_atoui(argv[2]);
#if MAX_FREQ_TYPE != 5
#error "Sweep mode possibly changed, check cmd_sweep function"
#endif
  // Parse sweep {start|stop|center|span|cw} {freq(Hz)}
  // get enum ST_START, ST_STOP, ST_CENTER, ST_SPAN, ST_CW, ST_VAR
  static const char sweep_cmd[] = "start|stop|center|span|cw|var";
  if (argc == 2 && value0 == 0) {
    int type = get_str_index(argv[0], sweep_cmd);
    if (type == -1)
      goto usage;
    set_sweep_frequency(type, value1);
    return;
  }
  //  Parse sweep {start(Hz)} [stop(Hz)]
  if (value0)
    set_sweep_frequency(ST_START, value0);
  if (value1)
    set_sweep_frequency(ST_STOP, value1);
  if (value2)
    set_sweep_points(value2);
  return;
usage:
  shell_printf("usage: sweep {start(Hz)} [stop(Hz)] [points]" VNA_SHELL_NEWLINE_STR \
               "\tsweep {%s} {freq(Hz)}" VNA_SHELL_NEWLINE_STR, sweep_cmd);
}

VNA_SHELL_FUNCTION(cmd_save)
{
  if (argc != 1)
    goto usage;

  int id = my_atoi(argv[0]);
  if (id < 0 || id >= SAVEAREA_MAX)
    goto usage;
  caldata_save(id);
  request_to_redraw(REDRAW_CAL_STATUS);
  return;

 usage:
  shell_printf("save {id}" VNA_SHELL_NEWLINE_STR);
}

VNA_SHELL_FUNCTION(cmd_recall)
{
  if (argc != 1)
    goto usage;

  int id = my_atoi(argv[0]);
  if (id < 0 || id >= SAVEAREA_MAX)
    goto usage;
  // Check for success
  if (load_properties(id))
    shell_printf("Err, default load" VNA_SHELL_NEWLINE_STR);
  return;
 usage:
  shell_printf("recall {id}" VNA_SHELL_NEWLINE_STR);
}
#if 0
static const char * const trc_channel_name[] = {
  "S11", "S21"
};
const char *get_trace_chname(int t)
{
  return trc_channel_name[trace[t].channel&1];
}
#endif

void set_trace_type(int t, int type, int channel)
{
  channel&= 1;
  bool update = trace[t].type != type; // || trace[t].channel != channel;
  if (!update) return;
  if (trace[t].type != type) {
    trace[t].type = type;
    trace[t].auto_scale = (type != TRC_TRANSFORM && type != TRC_FFT_AMP && type != TRC_FFT_B);
    // Set default trace refpos
    set_trace_refpos(t, trace_info_list[type].refpos);
    // Set default trace scale
    set_trace_scale(t, trace_info_list[type].scale_unit);
    request_to_redraw(REDRAW_AREA); // need for update grid
  }
//  set_trace_channel(t, channel);
}
#if 0
void set_trace_channel(int t, int channel)
{
  channel&= 1;
  if (trace[t].channel != channel) {
    trace[t].channel = channel;
    request_to_redraw(REDRAW_MARKER | REDRAW_PLOT);
  }
}
#endif

void set_active_trace(int t) {
  if (current_trace == t) return;
  current_trace = t;
  request_to_redraw(REDRAW_MARKER | REDRAW_GRID_VALUE);
}

void set_trace_scale(int t, float scale)
{
  if (trace[t].scale != scale) {
    trace[t].scale = scale;
    request_to_redraw(REDRAW_MARKER | REDRAW_GRID_VALUE | REDRAW_PLOT);
  }
}

void set_trace_refpos(int t, float refpos)
{
  if (trace[t].refpos != refpos) {
    trace[t].refpos = refpos;
    request_to_redraw(REDRAW_REFERENCE | REDRAW_GRID_VALUE | REDRAW_PLOT);
  }
}

void set_trace_enable(int t, bool enable)
{
  trace[t].enabled = enable;
  current_trace = enable ? t : TRACE_INVALID;
  if (!enable) {
    for (int i = 0; i < TRACES_MAX; i++)  // set first enabled as current trace
      if (trace[i].enabled) {set_active_trace(i); break;}
  }
  request_to_redraw(REDRAW_AREA);
}

void set_electrical_delay(float seconds)
{
  if (electrical_delay != seconds) {
    electrical_delay = seconds;
    request_to_redraw(REDRAW_MARKER);
  }
}

void set_s21_offset(float offset)
{
  if (s21_offset != offset) {
    s21_offset = offset;
    request_to_redraw(REDRAW_MARKER);
  }
}
#if 0
VNA_SHELL_FUNCTION(cmd_trace)
{
  uint32_t t;
  if (argc == 0) {
    for (t = 0; t < TRACES_MAX; t++) {
      if (trace[t].enabled) {
        const char *type = get_trace_typename(trace[t].type, 0);
        const char *channel = get_trace_chname(t);
        float scale = get_trace_scale(t);
        float refpos = get_trace_refpos(t);
        shell_printf("%d %s %s %f %f" VNA_SHELL_NEWLINE_STR, t, type, channel, scale, refpos);
      }
    }
    return;
  }

  if (get_str_index(argv[0], "all") == 0 &&
      argc > 1 && get_str_index(argv[1], "off") == 0) {
    for (t = 0; t < TRACES_MAX; t++)
      set_trace_enable(t, false);
    return;
  }

  t = (uint32_t)my_atoi(argv[0]);
  if (t >= TRACES_MAX)
    goto usage;
  if (argc == 1) {
    const char *type = get_trace_typename(trace[t].type, 0);
    const char *channel = get_trace_chname(t);
    shell_printf("%d %s %s" VNA_SHELL_NEWLINE_STR, t, type, channel);
    return;
  }
  if (get_str_index(argv[1], "off") == 0) {
    set_trace_enable(t, false);
    return;
  }
#if MAX_TRACE_TYPE != 30
#error "Trace type enum possibly changed, check cmd_trace function"
#endif
  // enum TRC_LOGMAG, TRC_PHASE, TRC_DELAY, TRC_SMITH, TRC_POLAR, TRC_LINEAR, TRC_SWR, TRC_REAL, TRC_IMAG, TRC_R, TRC_X, TRC_Z, TRC_ZPHASE,
  //      TRC_G, TRC_B, TRC_Y, TRC_Rp, TRC_Xp, TRC_sC, TRC_sL, TRC_pC, TRC_pL, TRC_Q, TRC_Rser, TRC_Xser, TRC_Zser, TRC_Rsh, TRC_Xsh, TRC_Zsh, TRC_Qs21
  static const char cmd_type_list[] = "logmag|phase|delay|smith|polar|linear|swr|real|imag|r|x|z|zp|g|b|y|rp|xp|sc|sl|pc|pl|q|rser|xser|zser|rsh|xsh|zsh|q21";
  int type = get_str_index(argv[1], cmd_type_list);
  if (type >= 0) {
    int src = trace[t].channel;
    if (argc > 2) {
      src = my_atoi(argv[2]);
      if ((uint32_t)src > 1)
        goto usage;
    }
    set_trace_type(t, type, src);
    set_trace_enable(t, true);
    return;
  }

  static const char cmd_marker_smith[] = "lin|log|ri|rx|rlc|gb|glc|rpxp|rplc|rxsh|rxser";
  // Set marker smith format
  int format = get_str_index(argv[1], cmd_marker_smith);
  if (format >=0) {
    trace[t].smith_format = format;
    return;
  }
  //                                            0      1
  static const char cmd_scale_ref_list[] = "scale|refpos";
  if (argc >= 3) {
    switch (get_str_index(argv[1], cmd_scale_ref_list)) {
      case 0: set_trace_scale(t, my_atof(argv[2])); break;
      case 1: set_trace_refpos(t, my_atof(argv[2])); break;
      default:
        goto usage;
    }
  }
  return;
usage:
  shell_printf("trace {0|1|2|3|all} [%s] [src]" VNA_SHELL_NEWLINE_STR \
               "trace {0|1|2|3} [%s]" VNA_SHELL_NEWLINE_STR\
               "trace {0|1|2|3} {%s} {value}" VNA_SHELL_NEWLINE_STR, cmd_type_list, cmd_marker_smith, cmd_scale_ref_list);
}


VNA_SHELL_FUNCTION(cmd_edelay)
{
  if (argc != 1) {
    shell_printf("%f" VNA_SHELL_NEWLINE_STR, electrical_delay * (1.0f / 1e-12f)); // return in picoseconds
    return;
  }
  set_electrical_delay(my_atof(argv[0]) * 1e-12); // input value in seconds
}

VNA_SHELL_FUNCTION(cmd_s21offset)
{
  if (argc != 1) {
    shell_printf("%f" VNA_SHELL_NEWLINE_STR, s21_offset); // return in dB
    return;
  }
  set_s21_offset(my_atof(argv[0]));     // input value in dB
}
#endif

VNA_SHELL_FUNCTION(cmd_marker)
{
  static const char cmd_marker_list[] = "on|off";
  int t;
  if (argc == 0) {
    for (t = 0; t < MARKERS_MAX; t++) {
      if (markers[t].enabled) {
        shell_printf("%d %d " VNA_FREQ_FMT_STR "" VNA_SHELL_NEWLINE_STR, t+1, markers[t].index, markers[t].frequency);
      }
    }
    return;
  }
  request_to_redraw(REDRAW_MARKER|REDRAW_AREA);
  // Marker on|off command
  int enable = get_str_index(argv[0], cmd_marker_list);
  if (enable >= 0) { // string found: 0 - on, 1 - off
    active_marker = enable == 1 ? MARKER_INVALID : 0;
    for (t = 0; t < MARKERS_MAX; t++)
      markers[t].enabled = enable == 0;
    return;
  }

  t = my_atoi(argv[0])-1;
  if (t < 0 || t >= MARKERS_MAX)
    goto usage;
  if (argc == 1) {
    shell_printf("%d %d " VNA_FREQ_FMT_STR "" VNA_SHELL_NEWLINE_STR, t+1, markers[t].index, markers[t].frequency);
    active_marker = t;
    // select active marker
    markers[t].enabled = TRUE;
    return;
  }

  switch (get_str_index(argv[1], cmd_marker_list)) {
    case 0: markers[t].enabled = TRUE; active_marker = t; return;
    case 1: markers[t].enabled =FALSE; if (active_marker == t) active_marker = MARKER_INVALID; return;
    default:
      // select active marker and move to index
      markers[t].enabled = TRUE;
      int index = my_atoi(argv[1]);
      set_marker_index(t, index);
      active_marker = t;
      return;
  }
 usage:
  shell_printf("marker [n] [%s|{index}]" VNA_SHELL_NEWLINE_STR, cmd_marker_list);
}

VNA_SHELL_FUNCTION(cmd_touchcal)
{
  (void)argc;
  (void)argv;
  shell_printf("first touch upper left, then lower right...");
  touch_cal_exec();
  shell_printf("done" VNA_SHELL_NEWLINE_STR \
               "touch cal params: %d %d %d %d" VNA_SHELL_NEWLINE_STR,
               config._touch_cal[0], config._touch_cal[1], config._touch_cal[2], config._touch_cal[3]);
  request_to_redraw(REDRAW_CLRSCR | REDRAW_AREA | REDRAW_BATTERY | REDRAW_CAL_STATUS | REDRAW_FREQUENCY);
}

VNA_SHELL_FUNCTION(cmd_touchtest)
{
  (void)argc;
  (void)argv;
  touch_draw_test();
}

VNA_SHELL_FUNCTION(cmd_frequencies)
{
  int i;
  (void)argc;
  (void)argv;
  for (i = 0; i < sweep_points; i++) {
    shell_printf(VNA_FREQ_FMT_STR VNA_SHELL_NEWLINE_STR, getFrequency(i));
  }
}

#ifdef ENABLE_TRANSFORM_COMMAND
static void
set_domain_mode(int mode) // accept DOMAIN_FREQ or DOMAIN_TIME
{
  if (mode != (props_mode & DOMAIN_MODE)) {
    props_mode = (props_mode & ~DOMAIN_MODE) | (mode & DOMAIN_MODE);
    request_to_redraw(REDRAW_FREQUENCY | REDRAW_MARKER);
    lever_mode = LM_MARKER;
  }
}

static inline void
set_timedomain_func(uint32_t func) // accept TD_FUNC_LOWPASS_IMPULSE, TD_FUNC_LOWPASS_STEP or TD_FUNC_BANDPASS
{
  props_mode = (props_mode & ~TD_FUNC) | func;
}

static inline void
set_timedomain_window(uint32_t func) // accept TD_WINDOW_MINIMUM/TD_WINDOW_NORMAL/TD_WINDOW_MAXIMUM
{
  props_mode = (props_mode & ~TD_WINDOW) | func;
}

VNA_SHELL_FUNCTION(cmd_transform)
{
  int i;
  if (argc == 0) {
    goto usage;
  }
  //                                         0   1       2    3        4       5      6       7
  static const char cmd_transform_list[] = "on|off|impulse|step|bandpass|minimum|normal|maximum";
  for (i = 0; i < argc; i++) {
    switch (get_str_index(argv[i], cmd_transform_list)) {
      case 0: set_domain_mode(DOMAIN_TIME); break;
      case 1: set_domain_mode(DOMAIN_FREQ); break;
      case 2: set_timedomain_func(TD_FUNC_LOWPASS_IMPULSE); break;
      case 3: set_timedomain_func(TD_FUNC_LOWPASS_STEP); break;
      case 4: set_timedomain_func(TD_FUNC_BANDPASS); break;
      case 5: set_timedomain_window(TD_WINDOW_MINIMUM); break;
      case 6: set_timedomain_window(TD_WINDOW_NORMAL); break;
      case 7: set_timedomain_window(TD_WINDOW_MAXIMUM); break;
      default:
        goto usage;
    }
  }
  return;
usage:
  shell_printf("usage: transform {%s} [...]" VNA_SHELL_NEWLINE_STR, cmd_transform_list);
}
#endif

#ifdef ENABLE_TEST_COMMAND
VNA_SHELL_FUNCTION(cmd_test)
{
  (void)argc;
  (void)argv;

#if 0
  int i;
  for (i = 0; i < 100; i++) {
    palClearPad(GPIOC, GPIOC_LED);
    set_frequency(10000000);
    palSetPad(GPIOC, GPIOC_LED);
    chThdSleepMilliseconds(50);

    palClearPad(GPIOC, GPIOC_LED);
    set_frequency(90000000);
    palSetPad(GPIOC, GPIOC_LED);
    chThdSleepMilliseconds(50);
  }
#endif

#if 0
  int i;
  int mode = 0;
  if (argc >= 1)
    mode = my_atoi(argv[0]);

  for (i = 0; i < 20; i++) {
    palClearPad(GPIOC, GPIOC_LED);
    ili9341_test(mode);
    palSetPad(GPIOC, GPIOC_LED);
    chThdSleepMilliseconds(50);
  }
#endif

#if 0
  //extern adcsample_t adc_samples[2];
  //shell_printf("adc: %d %d" VNA_SHELL_NEWLINE_STR, adc_samples[0], adc_samples[1]);
  int i;
  int x, y;
  for (i = 0; i < 50; i++) {
    test_touch(&x, &y);
    shell_printf("adc: %d %d" VNA_SHELL_NEWLINE_STR, x, y);
    chThdSleepMilliseconds(200);
  }
  //extern int touch_x, touch_y;
  //shell_printf("adc: %d %d" VNA_SHELL_NEWLINE_STR, touch_x, touch_y);
#endif
#if 0
  while (argc > 1) {
    int16_t x, y;
    touch_position(&x, &y);
    shell_printf("touch: %d %d" VNA_SHELL_NEWLINE_STR, x, y);
    chThdSleepMilliseconds(200);
  }
#endif
}
#endif

#ifdef ENABLE_PORT_COMMAND
VNA_SHELL_FUNCTION(cmd_port)
{
  int port;
  if (argc != 1) {
    shell_printf("usage: port {0:TX 1:RX}" VNA_SHELL_NEWLINE_STR);
    return;
  }
  port = my_atoi(argv[0]);
  tlv320aic3204_select(port);
}
#endif

#ifdef ENABLE_STAT_COMMAND
static struct {
  int16_t rms[2];
  int16_t ave[2];
#if 0
  int callback_count;
  int32_t last_counter_value;
  int32_t interval_cycles;
  int32_t busy_cycles;
#endif
} stat;

VNA_SHELL_FUNCTION(cmd_stat)
{
  int16_t *p = &rx_buffer[0];
  int32_t acc0, acc1;
  int32_t ave0, ave1;
//  float sample[2], ref[2];
//  minr, maxr,  mins, maxs;
  int32_t count = AUDIO_BUFFER_LEN;
  int i;
  (void)argc;
  (void)argv;
  for (int ch=0;ch<2;ch++){
    tlv320aic3204_select(ch);
    DSP_START(4);
    DSP_WAIT;
//    reset_dsp_accumerator();
//    dsp_process(&p[               0], AUDIO_BUFFER_LEN);
//    dsp_process(&p[AUDIO_BUFFER_LEN], AUDIO_BUFFER_LEN);

    acc0 = acc1 = 0;
    for (i = 0; i < AUDIO_BUFFER_LEN*2; i += 2) {
      acc0 += p[i  ];
      acc1 += p[i+1];
    }
    ave0 = acc0 / count;
    ave1 = acc1 / count;
    acc0 = acc1 = 0;
//    minr  = maxr = 0;
//    mins  = maxs = 0;
    for (i = 0; i < AUDIO_BUFFER_LEN*2; i += 2) {
      acc0 += (p[i  ] - ave0)*(p[i  ] - ave0);
      acc1 += (p[i+1] - ave1)*(p[i+1] - ave1);
//      if (minr < p[i  ]) minr = p[i  ];
//      if (maxr > p[i  ]) maxr = p[i  ];
//      if (mins < p[i+1]) mins = p[i+1];
//      if (maxs > p[i+1]) maxs = p[i+1];
    }
    stat.rms[0] = vna_sqrtf(acc0 / count);
    stat.rms[1] = vna_sqrtf(acc1 / count);
    stat.ave[0] = ave0;
    stat.ave[1] = ave1;
    shell_printf("Ch: %d" VNA_SHELL_NEWLINE_STR, ch);
    shell_printf("average:   r: %6d s: %6d" VNA_SHELL_NEWLINE_STR, stat.ave[0], stat.ave[1]);
    shell_printf("rms:       r: %6d s: %6d" VNA_SHELL_NEWLINE_STR, stat.rms[0], stat.rms[1]);
//    shell_printf("min:     ref %6d ch %6d" VNA_SHELL_NEWLINE_STR, minr, mins);
//    shell_printf("max:     ref %6d ch %6d" VNA_SHELL_NEWLINE_STR, maxr, maxs);
  }
  //shell_printf("callback count: %d" VNA_SHELL_NEWLINE_STR, stat.callback_count);
  //shell_printf("interval cycle: %d" VNA_SHELL_NEWLINE_STR, stat.interval_cycles);
  //shell_printf("busy cycle: %d" VNA_SHELL_NEWLINE_STR, stat.busy_cycles);
  //shell_printf("load: %d" VNA_SHELL_NEWLINE_STR, stat.busy_cycles * 100 / stat.interval_cycles);
//  extern int awd_count;
//  shell_printf("awd: %d" VNA_SHELL_NEWLINE_STR, awd_count);
}
#endif

#ifndef VERSION
#define VERSION "unknown"
#endif

const char NANOVNA_VERSION[] = VERSION;

VNA_SHELL_FUNCTION(cmd_version)
{
  (void)argc;
  (void)argv;
  shell_printf("%s" VNA_SHELL_NEWLINE_STR, NANOVNA_VERSION);
}

VNA_SHELL_FUNCTION(cmd_vbat)
{
  (void)argc;
  (void)argv;
  shell_printf("%d m" S_VOLT VNA_SHELL_NEWLINE_STR, adc_vbat_read());
}

#ifdef ENABLE_VBAT_OFFSET_COMMAND
VNA_SHELL_FUNCTION(cmd_vbat_offset)
{
  if (argc != 1) {
    shell_printf("%d" VNA_SHELL_NEWLINE_STR, config._vbat_offset);
    return;
  }
  config._vbat_offset = (int16_t)my_atoi(argv[0]);
}
#endif

#ifdef ENABLE_SI5351_TIMINGS
VNA_SHELL_FUNCTION(cmd_si5351time)
{
  (void)argc;
  int idx = my_atoui(argv[0]);
  uint16_t value = my_atoui(argv[1]);
  si5351_set_timing(idx, value);
}
#endif

#ifdef ENABLE_SI5351_REG_WRITE
VNA_SHELL_FUNCTION(cmd_si5351reg)
{
  if (argc != 2) {
    shell_printf("usage: si reg data" VNA_SHELL_NEWLINE_STR);
    return;
  }
  uint8_t reg = my_atoui(argv[0]);
  uint8_t dat = my_atoui(argv[1]);
  uint8_t buf[] = { reg, dat };
  si5351_bulk_write(buf, 2);
}
#endif

#ifdef ENABLE_I2C_TIMINGS
VNA_SHELL_FUNCTION(cmd_i2ctime)
{
  (void)argc;
  uint32_t tim =  STM32_TIMINGR_PRESC(0U)  |
                  STM32_TIMINGR_SCLDEL(my_atoui(argv[0])) | STM32_TIMINGR_SDADEL(my_atoui(argv[1])) |
                  STM32_TIMINGR_SCLH(my_atoui(argv[2])) | STM32_TIMINGR_SCLL(my_atoui(argv[3]));
  set_I2C_timings(tim);
}
#endif

#ifdef ENABLE_INFO_COMMAND
VNA_SHELL_FUNCTION(cmd_info)
{
  (void)argc;
  (void)argv;
  int i = 0;
  while (info_about[i])
    shell_printf("%s" VNA_SHELL_NEWLINE_STR, info_about[i++]);
}
#endif

#ifdef ENABLE_COLOR_COMMAND
VNA_SHELL_FUNCTION(cmd_color)
{
  uint32_t color;
  uint16_t i;
  if (argc != 2) {
    shell_printf("usage: color {id} {rgb24}" VNA_SHELL_NEWLINE_STR);
    for (i=0; i < MAX_PALETTE; i++) {
      color = GET_PALTETTE_COLOR(i);
      color = HEXRGB(color);
      shell_printf(" %2d: 0x%06x" VNA_SHELL_NEWLINE_STR, i, color);
    }
    return;
  }
  i = my_atoui(argv[0]);
  if (i >= MAX_PALETTE)
    return;
  color = RGBHEX(my_atoui(argv[1]));
  config._lcd_palette[i] = color;
  // Redraw all
  request_to_redraw(REDRAW_CLRSCR | REDRAW_AREA | REDRAW_CAL_STATUS | REDRAW_BATTERY | REDRAW_FREQUENCY);
}
#endif

#ifdef ENABLE_I2C_COMMAND
VNA_SHELL_FUNCTION(cmd_i2c){
  if (argc != 3) {
    shell_printf("usage: i2c page reg data" VNA_SHELL_NEWLINE_STR);
    return;
  }
  uint8_t page = my_atoui(argv[0]);
  uint8_t reg  = my_atoui(argv[1]);
  uint8_t data = my_atoui(argv[2]);
  tlv320aic3204_write_reg(page, reg, data);
}
#endif

#ifdef ENABLE_BAND_COMMAND
VNA_SHELL_FUNCTION(cmd_band){
  static const char cmd_sweep_list[] = "mode|freq|div|mul|omul|pow|opow|l|r|lr|adj";
  if (argc != 3){
    shell_printf("cmd error" VNA_SHELL_NEWLINE_STR);
    return;
  }
  int idx = my_atoui(argv[0]);
  int pidx = get_str_index(argv[1], cmd_sweep_list);
  si5351_update_band_config(idx, pidx, my_atoui(argv[2]));
}
#endif

#ifdef ENABLE_LCD_COMMAND
VNA_SHELL_FUNCTION(cmd_lcd){
  uint8_t d[VNA_SHELL_MAX_ARGUMENTS];
  if (argc == 0) return;
  for (int i=0;i<argc;i++)
    d[i] =  my_atoui(argv[i]);
  uint32_t ret = lcd_send_command(d[0], argc-1, &d[1]);
  shell_printf("ret = 0x%08X" VNA_SHELL_NEWLINE_STR, ret);
  chThdSleepMilliseconds(5);
}
#endif

#ifdef ENABLE_THREADS_COMMAND
#if CH_CFG_USE_REGISTRY == FALSE
#error "Threads Requite enabled CH_CFG_USE_REGISTRY in chconf.h"
#endif
VNA_SHELL_FUNCTION(cmd_threads) 
{
  static const char *states[] = {CH_STATE_NAMES};
  thread_t *tp;
  (void)argc;
  (void)argv;
  shell_printf("stklimit|   stack|stk free|    addr|refs|prio|    state|        name" VNA_SHELL_NEWLINE_STR);
  tp = chRegFirstThread();
  do {
    uint32_t max_stack_use = 0U;
#if (CH_DBG_ENABLE_STACK_CHECK == TRUE) || (CH_CFG_USE_DYNAMIC == TRUE)
    uint32_t stklimit = (uint32_t)tp->wabase;
#if CH_DBG_FILL_THREADS == TRUE
    uint8_t *p = (uint8_t *)tp->wabase; while(p[max_stack_use]==CH_DBG_STACK_FILL_VALUE) max_stack_use++;
#endif
#else
    uint32_t stklimit = 0U;
#endif
    shell_printf("%08x|%08x|%08x|%08x|%4u|%4u|%9s|%12s" VNA_SHELL_NEWLINE_STR,
             stklimit, (uint32_t)tp->ctx.sp, max_stack_use, (uint32_t)tp,
             (uint32_t)tp->refs - 1, (uint32_t)tp->prio, states[tp->state],
             tp->name == NULL ? "" : tp->name);
    tp = chRegNextThread(tp);
  } while (tp != NULL);
}
#endif

#ifdef __USE_SERIAL_CONSOLE__
#ifdef ENABLE_USART_COMMAND
VNA_SHELL_FUNCTION(cmd_usart_cfg)
{
  if (argc != 1) goto result;
  uint32_t speed = my_atoui(argv[0]);
  if (speed < 300) speed = 300;
  shell_update_speed(speed);
result:
  shell_printf("Serial: %u baud" VNA_SHELL_NEWLINE_STR, config._serial_speed);
}

VNA_SHELL_FUNCTION(cmd_usart)
{
  uint32_t time = MS2ST(200); // 200ms wait answer by default
  if (argc == 0 || argc > 2 || VNA_MODE(VNA_MODE_CONNECTION)) return; // Not work in serial mode
  if (argc == 2) time = MS2ST(my_atoui(argv[1]));
  sdWriteTimeout(&SD1, (uint8_t *)argv[0], strlen(argv[0]), time);
  sdWriteTimeout(&SD1, (uint8_t *)VNA_SHELL_NEWLINE_STR, sizeof(VNA_SHELL_NEWLINE_STR)-1, time);
  uint32_t size;
  uint8_t buffer[64];
  while ((size = sdReadTimeout(&SD1, buffer, sizeof(buffer), time)))
    streamWrite(&SDU1, buffer, size);
}
#endif
#endif

#ifdef __REMOTE_DESKTOP__
void send_region(remote_region_t *rd, uint8_t * buf, uint16_t size)
{
  if (SDU1.config->usbp->state == USB_ACTIVE) {
    shell_write(rd, sizeof(remote_region_t));
    shell_write(buf, size);
    shell_write(VNA_SHELL_PROMPT_STR VNA_SHELL_NEWLINE_STR, 6);
  }
  else
    sweep_mode&=~SWEEP_REMOTE;
}

VNA_SHELL_FUNCTION(cmd_refresh)
{
  static const char cmd_enable_list[] = "on|off";
  if (argc != 1) return;
  int enable = get_str_index(argv[0], cmd_enable_list);
       if (enable == 0) sweep_mode|= SWEEP_REMOTE;
  else if (enable == 1) sweep_mode&=~SWEEP_REMOTE;
  // redraw all on screen
  request_to_redraw(REDRAW_FREQUENCY | REDRAW_CAL_STATUS | REDRAW_AREA | REDRAW_BATTERY);
}

VNA_SHELL_FUNCTION(cmd_touch)
{
  if (argc != 2) return;
  remote_touch_set(REMOTE_PRESS, my_atoi(argv[0]), my_atoi(argv[1]));
}

VNA_SHELL_FUNCTION(cmd_release)
{
  int16_t x = -1, y = -1;
  if (argc == 2) {
    x = my_atoi(argv[0]);
    y = my_atoi(argv[1]);
  }
  remote_touch_set(REMOTE_RELEASE, x, y);
}
#endif

#ifdef ENABLE_SD_CARD_COMMAND
#ifndef __USE_SD_CARD__
#error "Need enable SD card support __USE_SD_CARD__ in nanovna.h, for use ENABLE_SD_CARD_COMMAND"
#endif

static FRESULT cmd_sd_card_mount(void){
  const FRESULT res = f_mount(fs_volume, "", 1);
  if (res != FR_OK)
    shell_printf("err: no card" VNA_SHELL_NEWLINE_STR);
  return res;
}

VNA_SHELL_FUNCTION(cmd_sd_list)
{
  (void)argc;
  (void)argv;

  DIR dj;
  FILINFO fno;
  FRESULT res;
  if (cmd_sd_card_mount() != FR_OK)
    return;
  char *search;
  switch (argc){
    case 0: search =   "*.*";break;
    case 1: search = argv[0];break;
    default: shell_printf("usage: sd_list {pattern}" VNA_SHELL_NEWLINE_STR); return;
  }
  res = f_findfirst(&dj, &fno, "", search);
  while (res == FR_OK && fno.fname[0])
  {
    shell_printf("%s %u" VNA_SHELL_NEWLINE_STR, fno.fname, fno.fsize);
    res = f_findnext(&dj, &fno);
  }
  f_closedir(&dj);
}

VNA_SHELL_FUNCTION(cmd_sd_read)
{
  char *buf = (char *)spi_buffer;
  if (argc != 1)
  {
     shell_printf("usage: sd_read {filename}" VNA_SHELL_NEWLINE_STR);
     return;
  }
  const char *filename = argv[0];
  if (cmd_sd_card_mount() != FR_OK)
    return;

  if (f_open(fs_file, filename, FA_OPEN_EXISTING | FA_READ) != FR_OK){
    shell_printf("err: no file" VNA_SHELL_NEWLINE_STR);
    return;
  }
  // shell_printf("sd_read: %s" VNA_SHELL_NEWLINE_STR, filename);
  // number of bytes to follow (file size)
  uint32_t filesize = f_size(fs_file);
  shell_write(&filesize, 4);
  UINT size = 0;
  // file data (send all data from file)
  while (f_read(fs_file, buf, 512, &size) == FR_OK && size > 0)
    shell_write(buf, size);

  f_close(fs_file);
  return;
}

VNA_SHELL_FUNCTION(cmd_sd_delete)
{
  FRESULT res;
  if (argc != 1) {
     shell_printf("usage: sd_delete {filename}" VNA_SHELL_NEWLINE_STR);
     return;
  }
  if (cmd_sd_card_mount() != FR_OK)
    return;
  const char *filename = argv[0];
  res = f_unlink(filename);
  shell_printf("delete: %s %s" VNA_SHELL_NEWLINE_STR, filename, res == FR_OK ? "OK" : "err");
  return;
}
#endif

#ifdef __SD_CARD_LOAD__
VNA_SHELL_FUNCTION(cmd_msg)
{
  if (argc == 0) {
    shell_printf("usage: msg delay [text] [header]" VNA_SHELL_NEWLINE_STR);
    return;
  }
  uint32_t delay = my_atoui(argv[0]);
  char *header = 0, *text = 0;
  if (argc > 1) text = argv[1];
  if (argc > 2) header = argv[2];
  drawMessageBox(header, text, delay);
}
#endif

//=============================================================================
VNA_SHELL_FUNCTION(cmd_help);

#pragma pack(push, 2)
typedef struct {
  const char           *sc_name;
  vna_shellcmd_t    sc_function;
  uint16_t flags;
} VNAShellCommand;
#pragma pack(pop)

// Some commands can executed only in sweep thread, not in main cycle
#define CMD_WAIT_MUTEX  1
// Command execution need in sweep thread, and need break sweep for run
#define CMD_BREAK_SWEEP 2
// Command can run in shell thread (if sweep thread process UI, not sweep)
#define CMD_RUN_IN_UI   4
// Command can run in load script
#define CMD_RUN_IN_LOAD 8

static const VNAShellCommand commands[] =
{
//    {"scan"        , cmd_scan        , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP},
#ifdef ENABLE_SCANBIN_COMMAND
    {"scan_bin"    , cmd_scan_bin    , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP},
#endif
    {"data"        , cmd_data        , 0},
    {"frequencies" , cmd_frequencies , 0},
    {"freq"        , cmd_freq        , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
    {"sweep"       , cmd_sweep       , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
//    {"power"       , cmd_power       , CMD_RUN_IN_LOAD},
//    {"pull"        , cmd_pull        , CMD_RUN_IN_LOAD},
#ifdef USE_VARIABLE_OFFSET
    {"offset"      , cmd_offset      , CMD_WAIT_MUTEX|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
#endif
    {"bandwidth"   , cmd_bandwidth   , CMD_RUN_IN_LOAD},
#ifdef __USE_RTC__
    {"time"        , cmd_time        , CMD_RUN_IN_UI},
#endif
#ifdef ENABLE_SD_CARD_COMMAND
    {"sd_list"     , cmd_sd_list     , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI},
    {"sd_read"     , cmd_sd_read     , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI},
    {"sd_delete"   , cmd_sd_delete   , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI},
#endif
#ifdef __VNA_ENABLE_DAC__
    {"dac"         , cmd_dac         , CMD_RUN_IN_LOAD},
#endif
    {"saveconfig"  , cmd_saveconfig  , CMD_RUN_IN_LOAD},
    {"clearconfig" , cmd_clearconfig , CMD_RUN_IN_LOAD},
#ifdef ENABLED_DUMP_COMMAND
    {"dump"        , cmd_dump        , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP},
#endif
#ifdef ENABLE_PORT_COMMAND
    {"port"        , cmd_port        , CMD_RUN_IN_LOAD},
#endif
#ifdef ENABLE_STAT_COMMAND
    {"stat"        , cmd_stat        , CMD_WAIT_MUTEX},
#endif
#ifdef ENABLE_GAIN_COMMAND
    {"gain"        , cmd_gain        , CMD_WAIT_MUTEX},
#endif
#ifdef ENABLE_SAMPLE_COMMAND
    {"sample"      , cmd_sample      , 0},
#endif
#ifdef ENABLE_TEST_COMMAND
    {"test"        , cmd_test        , 0},
#endif
    {"touchcal"    , cmd_touchcal    , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP},
    {"touchtest"   , cmd_touchtest   , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP},
    {"pause"       , cmd_pause       , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
    {"resume"      , cmd_resume      , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
 //   {"cal"         , cmd_cal         , CMD_WAIT_MUTEX},
#ifdef __SD_CARD_LOAD__
    {"msg"         , cmd_msg         , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_LOAD},
#endif
    {"save"        , cmd_save        , CMD_RUN_IN_LOAD},
    {"recall"      , cmd_recall      , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
//    {"trace"       , cmd_trace       , CMD_RUN_IN_LOAD},
    {"marker"      , cmd_marker      , CMD_RUN_IN_LOAD},
//    {"edelay"      , cmd_edelay      , CMD_RUN_IN_LOAD},
//    {"s21offset"   , cmd_s21offset   , CMD_RUN_IN_LOAD},
    {"capture"     , cmd_capture     , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI},
#ifdef __VNA_MEASURE_MODULE__
    {"measure"     , cmd_measure     , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
#endif
#ifdef __REMOTE_DESKTOP__
    {"refresh"     , cmd_refresh     , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI},
    {"touch"       , cmd_touch       , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI},
    {"release"     , cmd_release     , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI},
#endif
    {"vbat"        , cmd_vbat        , CMD_RUN_IN_LOAD},
    {"tcxo"        , cmd_tcxo        , CMD_RUN_IN_LOAD},
    {"reset"       , cmd_reset       , CMD_RUN_IN_LOAD},
#ifdef __USE_SMOOTH__
    {"smooth"      , cmd_smooth      , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
#endif
#ifdef ENABLE_CONFIG_COMMAND
    {"config"      , cmd_config      , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
#endif
#ifdef __USE_SERIAL_CONSOLE__
#ifdef ENABLE_USART_COMMAND
    {"usart_cfg"   , cmd_usart_cfg   , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
    {"usart"       , cmd_usart       , CMD_WAIT_MUTEX|CMD_BREAK_SWEEP|CMD_RUN_IN_UI|CMD_RUN_IN_LOAD},
#endif
#endif
#ifdef ENABLE_VBAT_OFFSET_COMMAND
    {"vbat_offset" , cmd_vbat_offset , CMD_RUN_IN_LOAD},
#endif
#ifdef ENABLE_TRANSFORM_COMMAND
    {"transform"   , cmd_transform   , CMD_RUN_IN_LOAD},
#endif
//    {"threshold"   , cmd_threshold   , CMD_RUN_IN_LOAD},
    {"help"        , cmd_help        , 0},
#ifdef ENABLE_INFO_COMMAND
    {"info"        , cmd_info        , 0},
#endif
    {"version"     , cmd_version     , 0},
#ifdef ENABLE_COLOR_COMMAND
    {"color"       , cmd_color       , CMD_RUN_IN_LOAD},
#endif
#ifdef ENABLE_I2C_COMMAND
    {"i2c"         , cmd_i2c         , CMD_WAIT_MUTEX},
#endif
#ifdef ENABLE_SI5351_REG_WRITE
    {"si"          , cmd_si5351reg   , CMD_WAIT_MUTEX},
#endif
#ifdef ENABLE_LCD_COMMAND
    {"lcd"         , cmd_lcd         , CMD_WAIT_MUTEX},
#endif
#ifdef ENABLE_THREADS_COMMAND
    {"threads"     , cmd_threads     , 0},
#endif
#ifdef ENABLE_SI5351_TIMINGS
    {"t"           , cmd_si5351time  , CMD_WAIT_MUTEX},
#endif
#ifdef ENABLE_I2C_TIMINGS
    {"i"           , cmd_i2ctime     , CMD_WAIT_MUTEX},
#endif
#ifdef ENABLE_BAND_COMMAND
    {"b"           , cmd_band        , CMD_WAIT_MUTEX},
#endif
    {NULL          , NULL            , 0}
};

VNA_SHELL_FUNCTION(cmd_help)
{
  (void)argc;
  (void)argv;
  const VNAShellCommand *scp = commands;
  shell_printf("Commands:");
  while (scp->sc_name != NULL) {
    shell_printf(" %s", scp->sc_name);
    scp++;
  }
  shell_printf(VNA_SHELL_NEWLINE_STR);
  return;
}

/*
 * VNA shell functions
 */

// Check Serial connection requirements
#ifdef __USE_SERIAL_CONSOLE__
#if HAL_USE_SERIAL == FALSE
#error "For serial console need HAL_USE_SERIAL as TRUE in halconf.h"
#endif

// Before start process command from shell, need select input stream
#define PREPARE_STREAM shell_stream = VNA_MODE(VNA_MODE_CONNECTION) ? (BaseSequentialStream *)&SD1 : (BaseSequentialStream *)&SDU1;

// Update Serial connection speed and settings
void shell_update_speed(uint32_t speed){
  config._serial_speed = speed;
  // Update Serial speed settings
  sdSetBaudrate(&SD1, speed);
}

// Check USB connection status
static bool usb_IsActive(void){
  return usbGetDriverStateI(&USBD1) == USB_ACTIVE;
}

// Reset shell I/O queue
void shell_reset_console(void){
  // Reset I/O queue over USB (for USB need also connect/disconnect)
  if (usb_IsActive()){
    if (VNA_MODE(VNA_MODE_CONNECTION))
      sduDisconnectI(&SDU1);
    else
      sduConfigureHookI(&SDU1);
  }
  // Reset I/O queue over Serial
  qResetI(&SD1.oqueue);
  qResetI(&SD1.iqueue);
  // Prepare I/O for shell_stream
  PREPARE_STREAM;
}

// Check active connection for Shell
static bool shell_check_connect(void){
  // Serial connection always active
  if (VNA_MODE(VNA_MODE_CONNECTION))
    return true;
  // USB connection can be USB_SUSPENDED
  return usb_IsActive();
}

static void shell_init_connection(void){
  osalThreadQueueObjectInit(&shell_thread);
/*
 * Initializes and start serial-over-USB CDC driver SDU1, connected to USBD1
 */
  sduObjectInit(&SDU1);
  sduStart(&SDU1, &serusbcfg);
  SerialConfig s_config = {config._serial_speed, 0, USART_CR2_STOP1_BITS, 0 };
  sdStart(&SD1, &s_config);
/*
 * Set Serial speed settings for SD1
 */
  shell_update_speed(config._serial_speed);

/*
 * Activates the USB driver and then the USB bus pull-up on D+.
 * Note, a delay is inserted in order to not have to disconnect the cable
 * after a reset.
 */
  usbDisconnectBus(&USBD1);
  chThdSleepMilliseconds(100);
  usbStart(&USBD1, &usbcfg);
  usbConnectBus(&USBD1);

  shell_reset_console();
}

#else
// Only USB console, shell_stream always on USB
#define PREPARE_STREAM shell_stream = (BaseSequentialStream *)&SDU1;

// Check connection as Active, if no suspend input
static bool shell_check_connect(void){
  return SDU1.config->usbp->state == USB_ACTIVE;
}

// Init shell I/O connection over USB
static void shell_init_connection(void){
/*
 * Initializes and start serial-over-USB CDC driver SDU1, connected to USBD1
 */
  sduObjectInit(&SDU1);
  sduStart(&SDU1, &serusbcfg);

/*
 * Activates the USB driver and then the USB bus pull-up on D+.
 * Note, a delay is inserted in order to not have to disconnect the cable
 * after a reset.
 */
  usbDisconnectBus(&USBD1);
  chThdSleepMilliseconds(100);
  usbStart(&USBD1, &usbcfg);
  usbConnectBus(&USBD1);

/*
 *  Set I/O stream SDU1 for shell
 */
  PREPARE_STREAM;
}
#endif

static inline char* vna_strpbrk(char *s1, const char *s2) {
  do {
    const char *s = s2;
    do {
      if (*s == *s1) return s1;
      s++;
    } while (*s);
    s1++;
  } while(*s1);
  return s1;
}

/*
 * Split line by arguments, return arguments count
 */
int parse_line(char *line, char* args[], int max_cnt) {
  char *lp = line, c;
  const char *brk;
  uint16_t nargs = 0;
  while ((c = *lp) != 0) {                   // While not end
    if (c != ' ' && c != '\t') {             // Skipping white space and tabs.
      if (c == '"') {lp++; brk = "\""; }     // string end is next quote or end
      else          {      brk = " \t";}     // string end is tab or space or end
      if (nargs < max_cnt) args[nargs] = lp; // Put pointer in args buffer (if possible)
      nargs++;                               // Substring count
      lp = vna_strpbrk(lp, brk);             // search end
      if (*lp == 0) break;                   // Stop, end of input string
      *lp = 0;                               // Set zero at the end of substring
    }
    lp++;
  }
  return nargs;
}

static const VNAShellCommand *VNAShell_parceLine(char *line){
  // Parse and execute line
  shell_nargs = parse_line(line, shell_args, ARRAY_COUNT(shell_args));
  if (shell_nargs > ARRAY_COUNT(shell_args)) {
    shell_printf("too many arguments, max " define_to_STR(VNA_SHELL_MAX_ARGUMENTS) "" VNA_SHELL_NEWLINE_STR);
    return NULL;
  }
  if (shell_nargs > 0) {
    const VNAShellCommand *scp;
    for (scp = commands; scp->sc_name != NULL; scp++)
      if (get_str_index(scp->sc_name, shell_args[0]) == 0)
        return scp;
  }
  return NULL;
}

//
// Read command line from shell_stream
//
static const char backspace[] = {0x08, 0x20, 0x08, 0x00};
static int VNAShell_readLine(char *line, int max_size)
{
  // send backspace, space for erase, backspace again
  uint8_t c;
  uint16_t j = 0;
  // Return 0 only if stream not active
  while (shell_read(&c, 1)) {
    // Backspace or Delete
    if (c == 0x08 || c == 0x7f) {
      if (j > 0) {shell_write(backspace, sizeof(backspace)); j--;}
      continue;
    }
    // New line (Enter)
    if (c == '\r') {
      shell_printf(VNA_SHELL_NEWLINE_STR);
      line[j] = 0;
      return 1;
    }
    // Others (skip) or too long - skip
    if (c < ' ' || j >= max_size - 1) continue;
    shell_write(&c, 1); // Echo
    line[j++] = (char)c;
  }
  return 0;
}

//
// Parse and run command line
//
static void VNAShell_executeLine(char *line)
{
  DEBUG_LOG(0, line); // debug console log
  // Execute line
  const VNAShellCommand *scp = VNAShell_parceLine(line);
  if (scp) {
    uint16_t cmd_flag = scp->flags;
    // Skip wait mutex if process UI
    if ((cmd_flag & CMD_RUN_IN_UI) && (sweep_mode&SWEEP_UI_MODE)) cmd_flag&=~CMD_WAIT_MUTEX;
    // Wait
    if (cmd_flag & CMD_WAIT_MUTEX) {
      shell_function = scp->sc_function;
      if (scp->flags & CMD_BREAK_SWEEP) operation_requested|=OP_CONSOLE;
      // Wait execute command in sweep thread
      osalThreadEnqueueTimeoutS(&shell_thread, TIME_INFINITE);
//      do {
//        chThdSleepMilliseconds(10);
//      } while (shell_function);
    } else
      scp->sc_function(shell_nargs - 1, &shell_args[1]);
//  DEBUG_LOG(10, "ok");
  } else if (**shell_args) // unknown command (not empty), ignore <CR>
    shell_printf("%s?" VNA_SHELL_NEWLINE_STR, shell_args[0]);
}

void VNAShell_executeCMDLine(char *line) {
  // Disable shell output (not allow shell_printf write, but not block other output!!)
  shell_stream = NULL;
  const VNAShellCommand *scp = VNAShell_parceLine(line);
  if (scp && (scp->flags & CMD_RUN_IN_LOAD))
    scp->sc_function(shell_nargs - 1, &shell_args[1]);
  PREPARE_STREAM;
}

#ifdef __SD_CARD_LOAD__
#ifndef __USE_SD_CARD__
#error "Need enable SD card support __USE_SD_CARD__ in nanovna.h, for use __SD_CARD_LOAD__"
#endif
bool sd_card_load_config(void){
  // Mount card
  if (f_mount(fs_volume, "", 1) != FR_OK)
    return FALSE;

  if (f_open(fs_file, "config.ini", FA_OPEN_EXISTING | FA_READ) != FR_OK)
    return FALSE;

  // Disable shell output (not allow shell_printf write, but not block other output!!)
  shell_stream = NULL;
  char *buf = (char *)spi_buffer;
  UINT size = 0;

  uint16_t j = 0, i;
  while (f_read(fs_file, buf, 512, &size) == FR_OK && size > 0){
    i = 0;
    while (i < size) {
      uint8_t c = buf[i++];
      // New line (Enter)
      if (c == '\r') {
//        shell_line[j  ] = '\r';
//        shell_line[j+1] = '\n';
//        shell_line[j+2] = 0;
//        shell_printf(shell_line);
        shell_line[j] = 0; j = 0;
        const VNAShellCommand *scp = VNAShell_parceLine(shell_line);
        if (scp && (scp->flags&CMD_RUN_IN_LOAD))
          scp->sc_function(shell_nargs - 1, &shell_args[1]);
        continue;
      }
      // Others (skip)
      if (c < 0x20) continue;
      // Store
      if (j < VNA_SHELL_MAX_LENGTH - 1)
        shell_line[j++] = (char)c;
    }
  }
  f_close(fs_file);
  PREPARE_STREAM;
  return TRUE;
}
#endif

#ifdef VNA_SHELL_THREAD
static THD_WORKING_AREA(waThread2, /* cmd_* max stack size + alpha */442);
THD_FUNCTION(myshellThread, p)
{
  (void)p;
  chRegSetThreadName("shell");
  while (true) {
    shell_printf(VNA_SHELL_PROMPT_STR);
    if (VNAShell_readLine(shell_line, VNA_SHELL_MAX_LENGTH))
      VNAShell_executeLine(shell_line);
    else // Putting a delay in order to avoid an endless loop trying to read an unavailable stream.
      chThdSleepMilliseconds(100);
  }
}
#endif

// Main thread stack size defined in makefile USE_PROCESS_STACKSIZE = 0x200
// Profile stack usage (enable threads command by def ENABLE_THREADS_COMMAND) show:
// Stack maximum usage = 472 bytes (need test more and run all commands), free stack = 40 bytes
//
int main(void){
/*
 * Initialize ChibiOS systems
 */
  *(int32_t *)0xE0042004 = 0xffffffff; // Stop all counters in debug mode
  halInit();
  chSysInit();

/*
 * Init used hardware
 */
/*
 *  Init DMA channels (used for direct send data, used for i2s and spi)
 */
  rccEnableDMA1(false);
//rccEnableDMA2(false);

/*
 * Init GPIO (pin control)
 */
#if HAL_USE_PAL == FALSE
  initPal();
#endif

/*
 * Initialize RTC library (not used ChibiOS RTC module)
 */
#ifdef __USE_RTC__
  rtc_init();
#endif

/*
 * Starting DAC1 driver, setting up the output pin as analog as suggested by the Reference Manual.
 */
#if defined(__VNA_ENABLE_DAC__) ||  defined(__LCD_BRIGHTNESS__)
  dac_init();
#endif

/*
 * restore config and calibration 0 slot from flash memory, also if need use backup data
 */
  load_settings();

/*
 * I2C bus
 */
  i2c_start();

/*
 * Start si5351
 */
  si5351_init();
  dirty_count = 10;

/*
 * Set frequency offset
 */
#ifdef USE_VARIABLE_OFFSET
  si5351_set_frequency_offset(IF_OFFSET);
#endif
  update_frequencies();
/*
 * Init Shell console connection data
 */
  shell_init_connection();

/*
 * SPI bus and LCD Initialize
 */
  lcd_init();

/*
 * tlv320aic Initialize (audio codec)
 */
  tlv320aic3204_init();
  chThdSleepMilliseconds(200); // Wait for aic codec start
/*
 * I2S Initialize
 */
  initI2S(rx_buffer, ARRAY_COUNT(rx_buffer) * sizeof(audio_sample_t) / sizeof(int16_t));

/*
 * SD Card init (if inserted) allow fix issues
 * Some card after insert work in SDIO mode and can corrupt SPI exchange (need switch it to SPI)
 */
#ifdef __USE_SD_CARD__
  disk_initialize(0);
#endif

/*
 * I2C bus run on work speed
 */
  i2c_set_timings(STM32_I2C_TIMINGR);

/*
 * Startup sweep thread
 */
  chThdCreateStatic(waThread1, sizeof(waThread1), NORMALPRIO-1, Thread1, NULL);
//  chThdCreateStatic(waThread1, sizeof(waThread1), HIGHPRIO, Thread1, NULL);

  while (1) {
    if (shell_check_connect()) {
      shell_printf(VNA_SHELL_NEWLINE_STR "NanoVNA Shell" VNA_SHELL_NEWLINE_STR);
#ifdef VNA_SHELL_THREAD
#if CH_CFG_USE_WAITEXIT == FALSE
#error "VNA_SHELL_THREAD use chThdWait, need enable CH_CFG_USE_WAITEXIT in chconf.h"
#endif
      thread_t *shelltp = chThdCreateStatic(waThread2, sizeof(waThread2),
                                            NORMALPRIO + 1,
                                            myshellThread, NULL);
      chThdWait(shelltp);
#else
      do {
        shell_printf(VNA_SHELL_PROMPT_STR);
        if (VNAShell_readLine(shell_line, VNA_SHELL_MAX_LENGTH))
          VNAShell_executeLine(shell_line);
        else
          chThdSleepMilliseconds(20);
      } while (shell_check_connect());
#endif
    }
    chThdSleepMilliseconds(10);
  }
}

/* The prototype shows it is a naked function - in effect this is just an
assembly function. */
void HardFault_Handler(void);

void hard_fault_handler_c(uint32_t *sp) __attribute__((naked));

void HardFault_Handler(void)
{
  uint32_t *sp;
  //__asm volatile ("mrs %0, msp \n\t": "=r" (sp) );
  __asm volatile("mrs %0, psp \n\t" : "=r"(sp));
  hard_fault_handler_c(sp);
}

void hard_fault_handler_c(uint32_t *sp) 
{
#ifdef ENABLE_HARD_FAULT_HANDLER_DEBUG
  uint32_t r0  = sp[0];
  uint32_t r1  = sp[1];
  uint32_t r2  = sp[2];
  uint32_t r3  = sp[3];
  register uint32_t  r4 __asm("r4");
  register uint32_t  r5 __asm("r5");
  register uint32_t  r6 __asm("r6");
  register uint32_t  r7 __asm("r7");
  register uint32_t  r8 __asm("r8");
  register uint32_t  r9 __asm("r9");
  register uint32_t r10 __asm("r10");
  register uint32_t r11 __asm("r11");
  uint32_t r12 = sp[4];
  uint32_t lr  = sp[5];
  uint32_t pc  = sp[6];
  uint32_t psr = sp[7];
  int y = 0;
  int x = 20;
  lcd_set_background(LCD_BG_COLOR);
  lcd_set_foreground(LCD_FG_COLOR);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "SP  0x%08x",  (uint32_t)sp);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R0  0x%08x",  r0);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R1  0x%08x",  r1);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R2  0x%08x",  r2);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R3  0x%08x",  r3);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R4  0x%08x",  r4);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R5  0x%08x",  r5);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R6  0x%08x",  r6);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R7  0x%08x",  r7);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R8  0x%08x",  r8);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R9  0x%08x",  r9);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R10 0x%08x", r10);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R11 0x%08x", r11);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "R12 0x%08x", r12);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "LR  0x%08x",  lr);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "PC  0x%08x",  pc);
  lcd_printf(x, y+=FONT_STR_HEIGHT, "PSR 0x%08x", psr);

  shell_printf("===================================" VNA_SHELL_NEWLINE_STR);
#else
  (void)sp;
#endif
  while (true) {
  }
}
// For new compilers
//void _exit(int x){(void)x;}
//void _kill(void){}
//int  _write (int file, char *data, int len) {(void)file; (void)data; return len;}
//void _getpid(void){}
