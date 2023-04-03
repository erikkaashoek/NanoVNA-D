/*
 * Copyright (c) 2019-2020, Dmitry (DiSlord) dislordlive@gmail.com
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

#include "nanovna.h"

typedef int16_t sincos_t;
//#define SPLIT_FREQUENCY
#ifdef USE_VARIABLE_OFFSET
static sincos_t sincos_tbl[AUDIO_SAMPLES_COUNT][2];
#ifdef SIDE_CHANNEL
static sincos_t sincos_tbl2[AUDIO_SAMPLES_COUNT][2];
#endif

void generate_DSP_Table(int offset){
  float audio_freq  = AUDIO_ADC_FREQ;
  // N = offset * AUDIO_SAMPLES_COUNT / audio_freq; should be integer
  // AUDIO_SAMPLES_COUNT = N * audio_freq / offset; N - minimum integer value for get integer AUDIO_SAMPLES_COUNT
  // Bandwidth on one step = audio_freq / AUDIO_SAMPLES_COUNT
  float step = offset / audio_freq;
  float w = 0; //= step/2;
#ifdef SIDE_CHANNEL
  float step2 = (offset *2 / 3) / audio_freq;
  float w2 = 0;
#endif
  for (int i=0; i<AUDIO_SAMPLES_COUNT; i++){
    float s, c;
    vna_sincosf(w, &s, &c);
    if ((current_props._fft_mode == FFT_AMP || current_props._fft_mode == FFT_B) && (VNA_MODE(VNA_MODE_WIDE)) ) {
#ifdef AUDIO_32_BIT
      sincos_tbl[i][0] = s*(float)(0x7fffff);
      sincos_tbl[i][1] = c*(float)(0x7fffff);
#else
      sincos_tbl[i][0] = s*(float)(0x7ff0);
      sincos_tbl[i][1] = c*(float)(0x7ff0);
#endif
    } else {
      sincos_tbl[i][0] = s*32610.0f * 0.5f * (1-cosf(2*VNA_PI*i/AUDIO_SAMPLES_COUNT)) ;
      sincos_tbl[i][1] = c*32610.0f * 0.5f * (1-cosf(2*VNA_PI*i/AUDIO_SAMPLES_COUNT)) ;
    }
    w+=step;
#ifdef SIDE_CHANNEL
    vna_sincosf(w2, &s, &c);
#if 0
#ifdef AUDIO_32_BIT
    sincos_tbl2[i][0] = s*(float)(0x7fffff);
    sincos_tbl2[i][1] = c*(float)(0x7fffff);
#else
    sincos_tbl2[i][0] = s*(float)(0x7ff0);
    sincos_tbl2[i][1] = c*(float)(0x7ff0);
#endif
#else
    sincos_tbl2[i][0] = s*32610.0f * 0.5f * (1-cosf(2*VNA_PI*i/AUDIO_SAMPLES_COUNT)) ;
    sincos_tbl2[i][1] = c*32610.0f * 0.5f * (1-cosf(2*VNA_PI*i/AUDIO_SAMPLES_COUNT)) ;
#endif
    w2+=step2;
#endif
  }
}

static volatile uint16_t sample_count;

#elif FREQUENCY_OFFSET==7000*(AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT/1000)
// static Table for 28kHz IF and 192kHz ADC (or 7kHz IF and 48kHz ADC) audio ADC
static const int16_t sincos_tbl[48][2] = {
  { 14493, 29389}, { 32138,  6393}, { 24636,-21605}, { -2143,-32698},
  {-27246,-18205}, {-31029, 10533}, {-10533, 31029}, { 18205, 27246},
  { 32698,  2143}, { 21605,-24636}, { -6393,-32138}, {-29389,-14493},
  {-29389, 14493}, { -6393, 32138}, { 21605, 24636}, { 32698, -2143},
  { 18205,-27246}, {-10533,-31029}, {-31029,-10533}, {-27246, 18205},
  { -2143, 32698}, { 24636, 21605}, { 32138, -6393}, { 14493,-29389},
  {-14493,-29389}, {-32138, -6393}, {-24636, 21605}, {  2143, 32698},
  { 27246, 18205}, { 31029,-10533}, { 10533,-31029}, {-18205,-27246},
  {-32698, -2143}, {-21605, 24636}, {  6393, 32138}, { 29389, 14493},
  { 29389,-14493}, {  6393,-32138}, {-21605,-24636}, {-32698,  2143},
  {-18205, 27246}, { 10533, 31029}, { 31029, 10533}, { 27246,-18205},
  {  2143,-32698}, {-24636,-21605}, {-32138,  6393}, {-14493, 29389}
};
#elif FREQUENCY_OFFSET==6000*(AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT/1000)
// static Table for 12kHz IF and 96kHz ADC (or 6kHz IF and 48kHz ADC) audio ADC
static const int16_t sincos_tbl[48][2] = {
  { 6393, 32138}, { 27246, 18205}, { 32138,-6393}, { 18205,-27246},
  {-6393,-32138}, {-27246,-18205}, {-32138, 6393}, {-18205, 27246},
  { 6393, 32138}, { 27246, 18205}, { 32138,-6393}, { 18205,-27246},
  {-6393,-32138}, {-27246,-18205}, {-32138, 6393}, {-18205, 27246},
  { 6393, 32138}, { 27246, 18205}, { 32138,-6393}, { 18205,-27246},
  {-6393,-32138}, {-27246,-18205}, {-32138, 6393}, {-18205, 27246},
  { 6393, 32138}, { 27246, 18205}, { 32138,-6393}, { 18205,-27246},
  {-6393,-32138}, {-27246,-18205}, {-32138, 6393}, {-18205, 27246},
  { 6393, 32138}, { 27246, 18205}, { 32138,-6393}, { 18205,-27246},
  {-6393,-32138}, {-27246,-18205}, {-32138, 6393}, {-18205, 27246},
  { 6393, 32138}, { 27246, 18205}, { 32138,-6393}, { 18205,-27246},
  {-6393,-32138}, {-27246,-18205}, {-32138, 6393}, {-18205, 27246}
};
#elif FREQUENCY_OFFSET==5000*(AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT/1000)
// static Table for 10kHz IF and 96kHz ADC (or 5kHz IF and 48kHz ADC) audio ADC
static const int16_t sincos_tbl[48][2] = {
  { 10533,  31029 }, { 27246,  18205 }, { 32698,  -2143 }, { 24636, -21605 },
  {  6393, -32138 }, {-14493, -29389 }, {-29389, -14493 }, {-32138,   6393 },
  {-21605,  24636 }, { -2143,  32698 }, { 18205,  27246 }, { 31029,  10533 },
  { 31029, -10533 }, { 18205, -27246 }, { -2143, -32698 }, {-21605, -24636 },
  {-32138,  -6393 }, {-29389,  14493 }, {-14493,  29389 }, {  6393,  32138 },
  { 24636,  21605 }, { 32698,   2143 }, { 27246, -18205 }, { 10533, -31029 },
  {-10533, -31029 }, {-27246, -18205 }, {-32698,   2143 }, {-24636,  21605 },
  { -6393,  32138 }, { 14493,  29389 }, { 29389,  14493 }, { 32138,  -6393 },
  { 21605, -24636 }, { 2143,  -32698 }, {-18205, -27246 }, {-31029, -10533 },
  {-31029,  10533 }, {-18205,  27246 }, {  2143,  32698 }, { 21605,  24636 },
  { 32138,   6393 }, { 29389, -14493 }, { 14493, -29389 }, { -6393, -32138 },
  {-24636, -21605 }, {-32698,  -2143 }, {-27246,  18205 }, {-10533,  31029 }
};
#elif FREQUENCY_OFFSET==4000*(AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT/1000)
// static Table for 8kHz IF and 96kHz audio ADC (or 4kHz IF and 48kHz ADC) audio ADC
static const int16_t sincos_tbl[48][2] = {
  {  4277, 32488}, { 19948, 25997}, { 30274, 12540}, { 32488, -4277},
  { 25997,-19948}, { 12540,-30274}, { -4277,-32488}, {-19948,-25997},
  {-30274,-12540}, {-32488,  4277}, {-25997, 19948}, {-12540, 30274},
  {  4277, 32488}, { 19948, 25997}, { 30274, 12540}, { 32488, -4277},
  { 25997,-19948}, { 12540,-30274}, { -4277,-32488}, {-19948,-25997},
  {-30274,-12540}, {-32488,  4277}, {-25997, 19948}, {-12540, 30274},
  {  4277, 32488}, { 19948, 25997}, { 30274, 12540}, { 32488, -4277},
  { 25997,-19948}, { 12540,-30274}, { -4277,-32488}, {-19948,-25997},
  {-30274,-12540}, {-32488,  4277}, {-25997, 19948}, {-12540, 30274},
  {  4277, 32488}, { 19948, 25997}, { 30274, 12540}, { 32488, -4277},
  { 25997,-19948}, { 12540,-30274}, { -4277,-32488}, {-19948,-25997},
  {-30274,-12540}, {-32488,  4277}, {-25997, 19948}, {-12540, 30274}
};
#elif FREQUENCY_OFFSET==3000*(AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT/1000)
// static Table for 6kHz IF and 96kHz audio ADC (or 3kHz IF and 48kHz ADC) audio ADC
static const int16_t sincos_tbl[48][2] = {
  {  3212, 32610}, { 15447, 28899}, { 25330, 20788}, { 31357,  9512},
  { 32610, -3212}, { 28899,-15447}, { 20788,-25330}, {  9512,-31357},
  { -3212,-32610}, {-15447,-28899}, {-25330,-20788}, {-31357, -9512},
  {-32610,  3212}, {-28899, 15447}, {-20788, 25330}, { -9512, 31357},
  {  3212, 32610}, { 15447, 28899}, { 25330, 20788}, { 31357,  9512},
  { 32610, -3212}, { 28899,-15447}, { 20788,-25330}, {  9512,-31357},
  { -3212,-32610}, {-15447,-28899}, {-25330,-20788}, {-31357, -9512},
  {-32610,  3212}, {-28899, 15447}, {-20788, 25330}, { -9512, 31357},
  {  3212, 32610}, { 15447, 28899}, { 25330, 20788}, { 31357,  9512},
  { 32610, -3212}, { 28899,-15447}, { 20788,-25330}, {  9512,-31357},
  { -3212,-32610}, {-15447,-28899}, {-25330,-20788}, {-31357, -9512},
  {-32610,  3212}, {-28899, 15447}, {-20788, 25330}, { -9512, 31357}
};
#elif FREQUENCY_OFFSET==2000*(AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT/1000)
// static Table
static const int16_t sincos_tbl[48][2] = {
#error "Need check/rebuild sin cos table for DAC"
};
#elif FREQUENCY_OFFSET==1000*(AUDIO_ADC_FREQ/AUDIO_SAMPLES_COUNT/1000)
// static Table
static const int16_t sincos_tbl[48][2] = {
#error "Need check/rebuild sin cos table for DAC"
};
#else
#error "Need check/rebuild sin cos table for DAC"
#endif

#ifndef __USE_DSP__
// Define DSP accumulator value type
//typedef float acc_t;
typedef int64_t acc_t;
//typedef float measure_t;
typedef int32_t sum_t;

acc_t acc_samp_s;
acc_t acc_samp_c;
acc_t acc_ref_s;
acc_t acc_ref_c;
static volatile uint16_t sample_count;
void
dsp_process(audio_sample_t *capture, size_t length)
{
#if 0
  int64_t samp_s = 0;
  int64_t samp_c = 0;
  int64_t ref_s = 0;
  int64_t ref_c = 0;
  uint32_t i = 0;
  do{
    int16_t ref = capture[i+0];
    int16_t smp = capture[i+1];
    int64_t sin = ((int32_t *)sincos_tbl)[i+0];
    int64_t cos = ((int32_t *)sincos_tbl)[i+1];
    samp_s+= (smp * sin);
    samp_c+= (smp * cos);
    ref_s += (ref * sin);
    ref_c += (ref * cos);
    i+=2;
  }while (i < length);
#else
  sum_t samp_s = 0;
  sum_t samp_c = 0;
  sum_t ref_s = 0;
  sum_t ref_c = 0;
  uint32_t i = 0;
  do{
#ifdef AUDIO_32_BIT
    int32_t ref = capture[i+0];
    int32_t smp = capture[i+1];
    ref = ref << 16 | ref >> 16;
    smp = smp << 16 | smp >> 16;
#else
    audio_sample_t ref = capture[i+0];
    audio_sample_t smp = capture[i+1];
#endif
    sum_t sin = ((sincos_t *)sincos_tbl)[i+0];
    sum_t cos = ((sincos_t *)sincos_tbl)[i+1];

    samp_s+= (smp * sin)>>AUDIO_SHIFT;
    samp_c+= (smp * cos)>>AUDIO_SHIFT;
    ref_s += (ref * sin)>>AUDIO_SHIFT;
    ref_c += (ref * cos)>>AUDIO_SHIFT;
    i+=2;
  }while (i < length);
#endif
  acc_samp_s += samp_s;
  acc_samp_c += samp_c;
  acc_ref_s += ref_s;
  acc_ref_c += ref_c;
  sample_count++;
}

#else
// Define DSP accumulator value type
typedef int64_t acc_t;
//typedef float measure_t;
static acc_t acc_samp_s;
static acc_t acc_samp_c;
static acc_t acc_ref_s;
static acc_t acc_ref_c;
static acc_t acc_delta_s;
static acc_t acc_delta_c;
//static acc_t acc_prev_s;
//static acc_t acc_prev_c;
#ifdef SIDE_CHANNEL
static acc_t acc_samp_s2;
static acc_t acc_samp_c2;
static acc_t acc_ref_s2;
static acc_t acc_ref_c2;
//static acc_t acc_delta_s2;
//static acc_t acc_delta_c2;
#endif
static phase_t null_phase = 0.5;
// Cortex M4 DSP instruction use
#include "dsp.h"
void
dsp_process(audio_sample_t *capture, size_t length)         // Accumulated (down converted) samples in acc_x_s and acc_x_c registers
{
#if 0
  if (props_mode & TD_PNA) {
    length /= 2;
    while (length-- > 0) {
      acc_samp_s += *capture++;
      acc_ref_s  += *capture++;
    }
    sample_count++;
    return;
  }
#endif

  uint32_t i = 0;
//  int64_t samp_s = 0;
//  int64_t samp_c = 0;
//  int64_t ref_s = 0;
//  int64_t ref_c = 0;

  if( current_props._fft_mode == FFT_B && (VNA_MODE(VNA_MODE_WIDE)))
    capture++;  // Switch to other channel

  do{

    if ((current_props._fft_mode == FFT_AMP || current_props._fft_mode == FFT_B) && (VNA_MODE(VNA_MODE_WIDE)) ) {


//#define AVER_SAMPLE 1
#ifdef   AVER_SAMPLE
      int16_t *sc = (int16_t *)&sincos_tbl[p_sweep % AUDIO_SAMPLES_COUNT];
      int32_t sr = 0;
      int j = 0;
      while (j < AVER_SAMPLE) {
        sr += capture[(i++)*2];
        j++;
      }
#else
      int16_t *sc = (int16_t *)&sincos_tbl[i];
      int32_t sr = capture[i*2]; // & 0xffff;
#endif

      if (p_sweep < requested_points){
        float* tmp  = (float*)spi_buffer;
        tmp[p_sweep * 2 + 1] = (float)((int32_t)(*sc++) * sr); // Only ref
        tmp[p_sweep * 2 + 0] = (float)((int32_t)(*sc++) * sr);
        p_sweep++;
      }
#ifndef   AVER_SAMPLE
      i++;
#endif

    } else {

//#define FAST_CALC
#ifdef FAST_CALC
      acc_samp_c += *(capture++);
      acc_ref_c  += *(capture++);
      i++;
      acc_samp_s += *(capture++);
      acc_ref_s  += *(capture++);
      i++;
      acc_samp_c -= *(capture++);
      acc_ref_c  -= *(capture++);
      i++;
      acc_samp_s -= *(capture++);
      acc_ref_s  -= *(capture++);
      i++;

#else
        int32_t sc = ((int32_t *)sincos_tbl)[i];
        int32_t sr = ((int32_t *)capture)[i];

// int32_t acc DSP functions, but int32 can overflow
//    samp_s = __smlatb(sr, sc, samp_s); // samp_s+= smp * sin
//    samp_c = __smlatt(sr, sc, samp_c); // samp_c+= smp * cos
//    ref_s  = __smlabb(sr, sc, ref_s);  //  ref_s+= ref * sin
//    ref_c  = __smlabt(sr, sc, ref_c);  //  ref_s+= ref * cos
// int64_t acc DSP functions
    acc_samp_s= __smlaltb(acc_samp_s, sr, sc ); // samp_s+= smp * sin
    acc_samp_c= __smlaltt(acc_samp_c, sr, sc ); // samp_c+= smp * cos
    acc_ref_s = __smlalbb( acc_ref_s, sr, sc ); //  ref_s+= ref * sin
    acc_ref_c = __smlalbt( acc_ref_c, sr, sc ); //  ref_s+= ref * cos
#ifdef SIDE_CHANNEL
    int32_t sc2 = ((int32_t *)sincos_tbl2)[i];
// int32_t acc DSP functions, but int32 can overflow
//    samp_s = __smlatb(sr, sc, samp_s); // samp_s+= smp * sin
//    samp_c = __smlatt(sr, sc, samp_c); // samp_c+= smp * cos
//    ref_s  = __smlabb(sr, sc, ref_s);  //  ref_s+= ref * sin
//    ref_c  = __smlabt(sr, sc, ref_c);  //  ref_s+= ref * cos
// int64_t acc DSP functions
    acc_samp_s2= __smlaltb(acc_samp_s2, sr, sc2 ); // samp_s+= smp * sin
    acc_samp_c2= __smlaltt(acc_samp_c2, sr, sc2 ); // samp_c+= smp * cos
    acc_ref_s2 = __smlalbb( acc_ref_s2, sr, sc2 ); //  ref_s+= ref * sin
    acc_ref_c2 = __smlalbt( acc_ref_c2, sr, sc2 ); //  ref_s+= ref * cos
    i++;
#endif
#endif
    }
  } while (i < length/2);
// Accumulate result, for faster calc and prevent overflow reduce size to int32_t
//  acc_samp_s+= (int32_t)(samp_s>>4);
//  acc_samp_c+= (int32_t)(samp_c>>4);
//  acc_ref_s += (int32_t)( ref_s>>4);
//  acc_ref_c += (int32_t)( ref_c>>4);
  sample_count++;
}
#endif

volatile phase_t summed_samp_angle;
volatile phase_t summed_ref_angle;
volatile phase_t summed_delta_angle;
volatile phase_t summed_samp_angle_increment;
volatile phase_t summed_ref_angle_increment;
volatile phase_t summed_delta_angle_increment;
#ifdef SIDE_CHANNEL
volatile phase_t summed_side_delta_angle;
phase_t unwrapped_side_delta_angle;
#endif
volatile int gamma_count = 0;
volatile int decimated_tau;
//volatile phase_t pll_delta_phase;
extern float amp_a;
extern float amp_b;
phase_t unwrapped_samp_angle, unwrapped_ref_angle, unwrapped_delta_angle; // For unwrapping in one summation
phase_t last_samp_angle,last_ref_angle, last_delta_angle; // For frequency calculation, never restarted

//#define LOG_SIZE    100
//volatile float phase_log[LOG_SIZE];
//int log_index = 0;

//#define HALF_PHASE  1.0
//#define FULL_PHASE  2.0

#define CALC_DELTA_ANGLE


void
calculate_vectors(void)
{

  // samp input frequency and phase summation

  phase_t samp_angle;
  samp_angle = vna_atan2f(acc_samp_s,acc_samp_c) / VNA_PI;

  phase_t samp_angle_increment = samp_angle - last_samp_angle;
  last_samp_angle = samp_angle;

  WRAP_FULL_PHASE(samp_angle_increment)
  summed_samp_angle_increment += samp_angle_increment;         // no decimation on frequency


  UNWRAP_PHASE(samp_angle,unwrapped_samp_angle);
  if (gamma_count  < decimated_tau)
    summed_samp_angle += samp_angle;

  phase_t ref_angle;
  ref_angle = vna_atan2f(acc_ref_s,acc_ref_c) / VNA_PI;

  // -- ref input frequency and phase summation

  phase_t ref_angle_increment = ref_angle - last_ref_angle;
  last_ref_angle = ref_angle;

  WRAP_FULL_PHASE(ref_angle_increment)
  summed_ref_angle_increment += ref_angle_increment;         // no decimation on frequency

  UNWRAP_PHASE(ref_angle,unwrapped_ref_angle);
  if (gamma_count  < decimated_tau)
    summed_ref_angle += ref_angle;


#ifdef CALC_DELTA_ANGLE
  // -- delta frequency and phase summation

  phase_t delta_angle;
  acc_delta_s = acc_samp_c * (float)acc_ref_c + acc_samp_s * (float)acc_ref_s;
  acc_delta_c = (acc_samp_s * (double)acc_ref_c - acc_samp_c * (double)acc_ref_s);

  delta_angle =  - vna_atan2f((acc_samp_c * (float)acc_ref_c + acc_samp_s * (float)acc_ref_s),
                         (acc_samp_s * (double)acc_ref_c - acc_samp_c * (double)acc_ref_s)) / VNA_PI;

  phase_t delta_angle_increment = delta_angle - last_delta_angle;
  last_delta_angle = delta_angle;

  WRAP_FULL_PHASE(delta_angle_increment)
  summed_delta_angle_increment += delta_angle_increment;         // no decimation on frequency

  UNWRAP_PHASE(delta_angle,unwrapped_delta_angle);
  if (gamma_count  < decimated_tau)
    summed_delta_angle += delta_angle;
#endif

#ifdef SIDE_CHANNEL
  phase_t side_delta_angle;
  side_delta_angle =  - vna_atan2f((acc_samp_c2 * (float)acc_ref_c2 + acc_samp_s2 * (float)acc_ref_s2),
                         (acc_samp_s2 * (double)acc_ref_c2 - acc_samp_c2 * (double)acc_ref_s2)) / VNA_PI;

  UNWRAP_PHASE(side_delta_angle,unwrapped_side_delta_angle);
  summed_side_delta_angle += side_delta_angle;
#endif

  amp_a = vna_sqrtf((float)acc_ref_c * (float)acc_ref_c + (float)acc_ref_s*(float)acc_ref_s);
  amp_b = vna_sqrtf((float)acc_samp_c * (float)acc_samp_c + (float)acc_samp_s*(float)acc_samp_s);
#ifdef FAST_CALC
  amp_a *= 32000;
  amp_b *= 32000;
#endif
#ifdef SIDE_CHANNEL
  amp_sa = vna_sqrtf((float)acc_ref_c2 * (float)acc_ref_c2 + (float)acc_ref_s2*(float)acc_ref_s2);
  amp_sb = vna_sqrtf((float)acc_samp_c2 * (float)acc_samp_c2 + (float)acc_samp_s2*(float)acc_samp_s2);
#endif

  gamma_count++;

}

#if 0
float get_freq_a(void)
{
  float df = (summed_ref_angle_increment / gamma_count) // pll_delta_phase
      * (AUDIO_ADC_FREQ>>1);
  df /= (config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT;
  return (df);
}

float get_freq_b(void)
{
  float df = (summed_samp_angle_increment / gamma_count) // pll_delta_phase
      * (AUDIO_ADC_FREQ>>1);
  df /= (config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT;
  return (df);
}

float get_freq_delta(void)
{
  float df = (summed_delta_angle_increment / gamma_count) // pll_delta_phase
      * (AUDIO_ADC_FREQ>>1);
  df /= (config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT;
  return (df);
}
#endif

inline float my_fabs(float x)
{
  if (x<0)
    return -x;
  return x;
}

#ifdef SIDE_CHANNEL
static phase_t side_aver;
#endif
int
calculate_gamma(phase_t gamma[MAX_MEASURED], uint16_t tau)
{
  decimated_tau = tau / current_props.decimation;
#ifndef CALC_DELTA_ANGLE
  gamma[B_PHASE] = summed_samp_angle/tau;
#ifndef CALC_DELTA_ANGLE
  gamma[B_PHASE] += null_phase;
#endif
  WRAP_FULL_PHASE(gamma[B_PHASE]);
#endif

  gamma[A_PHASE] = summed_ref_angle/decimated_tau;
  WRAP_FULL_PHASE(gamma[A_PHASE]);

//  df *= AUDIO_ADC_FREQ>>1;
//  df /= current_props.tau*(config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT;


#ifdef CALC_DELTA_ANGLE
  gamma[D_PHASE] = summed_delta_angle/decimated_tau + null_phase;
  if (VNA_MODE(VNA_MODE_SIDE_CHANNEL) && level_sa > -30)
    gamma[D_PHASE] -= side_aver;
#else
  gamma[D_PHASE] = gamma[A_PHASE] - gamma[B_PHASE] ;
#endif
  WRAP_FULL_PHASE(gamma[D_PHASE]);

#ifdef SIDE_CHANNEL
  if (VNA_MODE(VNA_MODE_SIDE_CHANNEL)) {
    phase_t temp = summed_side_delta_angle/tau;
    WRAP_FULL_PHASE(temp);

#define S_AVER 3
    if (my_fabs(temp - side_aver) > 0.001)
      side_aver = temp;
    else
      side_aver = (side_aver * S_AVER + temp) / (S_AVER + 1);
    gamma[S_PHASE] = side_aver;
  }
  gamma[A_FREQ] = ((summed_ref_angle_increment / gamma_count) * (AUDIO_ADC_FREQ>>1) ) / ((config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT);
  gamma[B_FREQ] = ((summed_samp_angle_increment / gamma_count) * (AUDIO_ADC_FREQ>>1) ) / ((config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT);
  gamma[D_FREQ] = ((summed_delta_angle_increment / gamma_count) * (AUDIO_ADC_FREQ>>1) ) / ((config._bandwidth+SAMPLE_OVERHEAD) * AUDIO_SAMPLES_COUNT);

#endif
  if (!(VNA_MODE(VNA_MODE_WIDE))) {
    if (current_props._fft_mode == FFT_AMP && p_sweep < requested_points){
      float* tmp  = (float*)spi_buffer;
      tmp[p_sweep * 2 + 0] = (float)acc_ref_c/3;
      tmp[p_sweep * 2 + 1] = (float)acc_ref_s/3;
      p_sweep++;
    }
    if (current_props._fft_mode == FFT_B && p_sweep < requested_points){
      float* tmp  = (float*)spi_buffer;
      tmp[p_sweep * 2 + 0] = (float)acc_samp_c/3;
      tmp[p_sweep * 2 + 1] = (float)acc_samp_s/3;
      p_sweep++;
    }
    if (current_props._fft_mode == FFT_PHASE && p_sweep < requested_points){
      float* tmp  = (float*)spi_buffer;
      tmp[p_sweep * 2 + 0] = sinf(gamma[D_PHASE] * VNA_PI);
      tmp[p_sweep * 2 + 1] = cosf(gamma[D_PHASE] * VNA_PI);
      p_sweep++;
    }
  }
  return(tau);
}

void calculate_subsamples(phase_t gamma[4], uint16_t tau)
{
  int used_samples = (AUDIO_BUFFER_LEN/2) * tau / current_props.decimation;
  gamma[2] = (float)acc_samp_s/(float)used_samples;
  gamma[3] = (float)acc_ref_s/(float)used_samples;
}

#if 0
void
fetch_amplitude(phase_t gamma[2])
{
  gamma[0] =  acc_samp_s * 1e-9;
  gamma[1] =  acc_samp_c * 1e-9;
}

void
fetch_amplitude_ref(phase_t gamma[2])
{
  gamma[0] =  acc_ref_s * 1e-9;
  gamma[1] =  acc_ref_c * 1e-9;
}
#endif

#if 0
void
fetch_data(phase_t gamma[4])
{
  gamma[0] =  acc_ref_s;
  gamma[1] =  acc_ref_c;
  gamma[2] =  acc_samp_s;
  gamma[3] =  acc_samp_c;
}

#endif

void
reset_dsp_accumerator(void)
{
  acc_ref_s = 0;
  acc_ref_c = 0;
  acc_samp_s = 0;
  acc_samp_c = 0;
//  acc_prev_s = 0;           // For PLL
//  acc_prev_c = 0;
#ifdef SIDE_CHANNEL
  acc_ref_s2 = 0;
  acc_ref_c2 = 0;
  acc_samp_s2 = 0;
  acc_samp_c2 = 0;
#endif
  sample_count = 0;
}

void
reset_averaging(void)
{
  summed_samp_angle = 0.0;  // Summed angle for averaging phase
  summed_ref_angle = 0.0;
  summed_delta_angle = 0.0;
  summed_samp_angle_increment = 0.0;  // summed angle increment for calculating frequency
  summed_ref_angle_increment = 0.0;  // summed angle increment for calculating frequency
  summed_delta_angle_increment = 0.0;  // summed angle increment for calculating frequency
  gamma_count = 0;
  unwrapped_samp_angle = 0;      // All for unwrapping the angle before summing
  unwrapped_ref_angle = 0;
  unwrapped_delta_angle = 0;
#ifdef SIDE_CHANNEL
  summed_side_delta_angle = 0.0;
  unwrapped_side_delta_angle = 0.0;
#endif
}

void set_null_phase(phase_t v)
{
  null_phase += v/180.0;
  reset_sweep();
}
