// ==========================================================================
// btt_amalgamation.c -- Beat-and-Tempo-Tracking library
// Copyright (c) 2021 Michael Krzyzaniak -- MIT License
// https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking
//
// Single-file build.  Add ONLY this file to your JUCE/Projucer project.
// Same pattern as sqlite3.c -- no external .c dependencies at build time.
// ==========================================================================

// --- Platform compatibility ---
#ifdef _MSC_VER
  #ifndef _USE_MATH_DEFINES
    #define _USE_MATH_DEFINES
  #endif
  #pragma warning(push)
  #pragma warning(disable: 4244)  // conversion double->float
  #pragma warning(disable: 4305)  // truncation double->float
  #pragma warning(disable: 4267)  // conversion size_t->int
  #pragma warning(disable: 4018)  // signed/unsigned mismatch
  #pragma warning(disable: 4100)  // unreferenced parameter
  #pragma warning(disable: 4146)  // unary minus on unsigned
  #pragma warning(disable: 4706)  // assignment within conditional
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

#ifdef _MSC_VER
  #define random() rand()
#endif

// VLA replacement (MSVC does not support C99 VLAs)
#define BTT_MAX_TEMPO_CANDIDATES 64

// Neutralize macros from Windows/JUCE headers that clash with BTT identifiers.
// <complex.h> (included transitively by JUCE) can #define 'real' and 'imag'.
#ifdef real
  #undef real
#endif
#ifdef imag
  #undef imag
#endif

// --- C linkage for entire file (MSVC compiles .c as C++) ---
#ifdef __cplusplus
extern "C" {
#endif

// ======================================================================
//  HEADERS
// ======================================================================

// --- fastsin.h ---
#ifndef  __MK_SINE_TABLE__     
#define  __MK_SINE_TABLE__ 1 

                      

#include <stdint.h>


#define  SIN_NUM_SAMPLES        4096
#define  SIN_TWO_PI             0x100000000
#define  SIN_PI                 0x80000000
#define  SIN_HALF_PI            0x40000000
#define  SIN_QUARTER_PI         0x20000000
#define  fastsin_t              uint32_t

float fastsin  (fastsin_t angle);
float fastcos  (fastsin_t angle);

#define fastsin(angle) (*(sinTable + ((angle) >> 20)))
#define fastcos(angle) (*(sinTable + (((angle) + SIN_HALF_PI) >> 20)))

extern const float sinTable[SIN_NUM_SAMPLES];


//slowsin
/*
#define  SIN_TWO_PI             (2 * M_PI)
#define  SIN_PI                 (M_PI)
#define  SIN_HALF_PI            (0.5 * M_PI)
#define  SIN_QUARTER_PI         (0.25 * M_PI)
#define  fastsin_t              double

float fastsin  (fastsin_t angle);
float fastcos  (fastsin_t angle);

#define fastsin(angle) (sin(angle))
#define fastcos(angle) (cos(angle))
*/

                      

#endif//__MK_SINE_TABLE__     


// --- DFT.h ---
/*__--------------_-----------_---------------------__---------------------
 / _|___ _  _ _ _(_)___ _ _  | |_ _ _ __ _ _ _  ___/ _|___ _ _ _ __  
|  _/ _ \ || | '_| / -_) '_| |  _| '_/ _` | ' \(_-<  _/ _ \ '_| '  \ 
|_| \___/\_,_|_| |_\___|_|    \__|_| \__,_|_||_/__/_| \___/_| |_|_|_|
-------------------------------------------------------------------------*/

#ifndef __DFT__
#define __DFT__ 1



typedef float dft_sample_t;

void   dft_init_blackman_window (dft_sample_t* window, int N);
void   dft_init_hann_window     (dft_sample_t* window, int N);
void   dft_init_hamming_window  (dft_sample_t* window, int N);
void   dft_init_half_sine_window(dft_sample_t* window, int N);
void   dft_apply_window         (dft_sample_t* real,   dft_sample_t* window, int N);

void   dft_raw_forward_dft      (dft_sample_t* real,   dft_sample_t* imag,   int N);
void   dft_raw_inverse_dft      (dft_sample_t* real,   dft_sample_t* imag,   int N);
void   dft_bit_reverse_indices  (dft_sample_t* real,   dft_sample_t* imag,   int N);
void   dft_complex_forward_dft  (dft_sample_t* real,   dft_sample_t* imag,   int N);
void   dft_complex_inverse_dft  (dft_sample_t* real,   dft_sample_t* imag,   int N);

void   dft_real_forward_dft     (dft_sample_t* real,   dft_sample_t* imag,   int N);
void   dft_real_inverse_dft     (dft_sample_t* real,   dft_sample_t* imag,   int N);

void   dft_2_real_forward_dfts  (dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N);
void   dft_2_real_inverse_dfts  (dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N);


/*
  Note: dft_real_forward_dft takes real input and ignores imag input,
  and uses a N/2-point complex DFT to produce 2-sided real and imaginary output
  where the negative frequencies are filled in with the complex conjugates
  of the positive ones.
  
  rdft_forward_dft takes real input and uses a native real DFT to produce
  single-sided real and imag output in-place, in the order
  [real[0], real[1] ... real[N/2], imag[N/2-1], imag[1]]
  imag[0] and imag[N/2] are understood to be 0. This is faster and uses
  half the space of dft_real_forward_dft.
*/

void   rdft_bit_reverse_indices (dft_sample_t* real, int N);
void   rdft_real_forward_dft    (dft_sample_t* real,  int N);
void   rdft_real_inverse_dft    (dft_sample_t* real,  int N);
void   rdft_2_real_forward_dfts (dft_sample_t* real_1, dft_sample_t* real_2, int N);
void   rdft_2_real_inverse_dfts (dft_sample_t* real_1, dft_sample_t* real_2, int N);

void   rdft_real_generalized_autocorrelation  (dft_sample_t* real, int N, double exponent);

void   rdft_rect_to_polar       (dft_sample_t* real,  int N);
void   rdft_polar_to_rect       (dft_sample_t* real,  int N);

/* user should zero-pad data to twice its original length for correlation and convolution */
void   dft_real_convolve        (dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N);
void   dft_real_correlate       (dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N);
void   dft_real_autocorrelate   (dft_sample_t* real  , dft_sample_t* imag  , int N);
/* generalized cross-correlation with phase transform */
void   dft_real_generalized_autocorrelation  (dft_sample_t* real  , dft_sample_t* imag, int N, double exponent);

void   dft_rect_to_polar        (dft_sample_t* real,   dft_sample_t* imag,   int N);
void   dft_polar_to_rect        (dft_sample_t* real,   dft_sample_t* imag,   int N);
void   dft_magnitude_to_db      (dft_sample_t* real,   int N);
void   dft_normalize_magnitude  (dft_sample_t* real,   int N);

double dft_bin_of_frequency     (double hz, double sample_rate, int N);
double dft_frequency_of_bin     (double bin, double sample_rate, int N);
int    dft_smallest_power_of_2_at_least_as_great_as(int n);



#endif   // __DFT__


// --- Filter.h ---
#ifndef __FILTER__
#define __FILTER__



typedef struct opaque_filter_struct Filter;

typedef enum filter_type_enum
{
  FILTER_LOW_PASS,
  FILTER_HIGH_PASS,
  FILTER_BAND_PASS,
  FILTER_BAND_STOP,
  FILTER_NUMBER_OF_TYPES,
}filter_type_t;

typedef enum filter_window_enum
{
  FILTER_WINDOW_RESERVED = 0,
  FILTER_WINDOW_RECT,
  FILTER_WINDOW_BARTLETT,
  FILTER_WINDOW_HANN,
  FILTER_WINDOW_HAMMING,
  FILTER_WINDOW_BLACKMANN,
}filter_window_t;

Filter*         filter_new            (filter_type_t type, float cutoff, int order);
Filter*         filter_destroy        (Filter* self);

void            filter_clear          (Filter* self);
void            filter_set_filter_type(Filter* self, filter_type_t type);
filter_type_t   filter_get_filter_type(Filter* self);
void            filter_set_sample_rate(Filter* self, float sample_rate);
float           filter_get_sample_rate(Filter* self);
void            filter_set_cutoff     (Filter* self, float cutoff);
float           filter_get_cutoff     (Filter* self);
void            filter_set_order      (Filter* self, int order); //cannot be greater than the initial order...
int             filter_get_order      (Filter* self);
void            filter_set_window_type(Filter* self, filter_window_t window);
filter_window_t filter_get_window_type(Filter* self);

void            filter_process_data   (Filter* self, float* data, int num_samples);


#endif //__FILTER__


// --- STFT.h ---
/*__--------------_-----------_---------------------__---------------------

-------------------------------------------------------------------------*/

#ifndef __STFT__
#define __STFT__ 1





/*--------------------------------------------------------------------*/
typedef struct Opaque_STFT_Struct STFT;

typedef void (*stft_onprocess_t)(void* onprocess_self, dft_sample_t* real, int N);

STFT*   stft_new     (int window_size /*power of 2 please*/, int overlap /* 1, 2, 4, 8 */, int should_resynthesize);
STFT*   stft_destroy (STFT* self);
void    stft_process (STFT* self, dft_sample_t* real_input, int len, stft_onprocess_t onprocess, void* onprocess_self);
int     stft_get_N (STFT* self);
int     stft_get_overlap (STFT* self);
int     stft_get_hop     (STFT* self);

/*--------------------------------------------------------------------*/
typedef struct Opaque_TWO_STFTS_Struct TWO_STFTS;

typedef void (*two_stfts_onprocess_t)(void* onprocess_self, dft_sample_t* real, dft_sample_t* imag, dft_sample_t* real_2, dft_sample_t* imag_2, int N);

TWO_STFTS*   two_stfts_new(int window_size /*power of 2 please*/, int overlap /* 1, 2, 4, 8 */, int should_resynthesize);
TWO_STFTS*   two_stfts_destroy(TWO_STFTS* self);
void         two_stfts_process(TWO_STFTS* self, dft_sample_t* real_input, dft_sample_t* real_input_2, int len, int two_inverses, two_stfts_onprocess_t onprocess, void* onprocess_self);



#endif   // __DFT__


// --- Statistics.h ---
/*-____--_--------_---_-----_---_------------_--------------------------
  / ___|| |_ __ _| |_(_)___| |_(_) ___ ___  | |__  
  \___ \| __/ _` | __| / __| __| |/ __/ __| | '_ \ 
   ___) | || (_| | |_| \__ \ |_| | (__\__ \_| | | |
  |____/ \__\__,_|\__|_|___/\__|_|\___|___(_)_| |_|
------------------------------------------------------------------------

----------------------------------------------------------------------*/
#ifndef __IC_STATISTICS__
#define __IC_STATISTICS__
  


/*--------------------------------------------------------------------*/
/*--___-------_-_--------------_----------------------------------------
   / _ \ _ _ | (_)_ _  ___    /_\__ _____ _ _ __ _ __ _ ___ 
  | (_) | ' \| | | ' \/ -_)  / _ \ V / -_) '_/ _` / _` / -_);
   \___/|_||_|_|_|_||_\___| /_/ \_\_/\___|_| \__,_\__, \___|
--------------------------------------------------|___/-----------------
  Calculate the mean and variance of a signal one sample at a time.
----------------------------------------------------------------------*/
typedef struct     opaque_online_average_struct     OnlineAverage;
OnlineAverage*     online_average_new               ();
void               online_average_init              (OnlineAverage* self);
OnlineAverage*     online_average_destroy           (OnlineAverage* self);
int                online_average_n                 (OnlineAverage* self);
double             online_average_mean              (OnlineAverage* self);
double             online_average_variance          (OnlineAverage* self);
double             online_average_std_dev           (OnlineAverage* self);
void               online_average_update            (OnlineAverage* self, double     x);

/*--------------------------------------------------------------------*/
/*-__--__---------_---------------_-------------------------------------
  |  \/  |_____ _(_)_ _  __ _    /_\__ _____ _ _ __ _ __ _ ___ 
  | |\/| / _ \ V / | ' \/ _` |  / _ \ V / -_) '_/ _` / _` / -_);
  |_|  |_\___/\_/|_|_||_\__, | /_/ \_\_/\___|_| \__,_\__, \___|
------------------------|___/------------------------|___/--------------
  Calculate the moving or rolling mean and variance over the past N
  samples of a signal. This algorithm is online and updates one sample
  at a time.
----------------------------------------------------------------------*/
typedef struct     opaque_moving_average_struct     MovingAverage;
MovingAverage*     moving_average_new               (unsigned N);
void               moving_average_init              (MovingAverage* self);
MovingAverage*     moving_average_destroy           (MovingAverage* self);
int                moving_average_N                 (MovingAverage* self);
int                moving_average_n                 (MovingAverage* self);
double             moving_average_mean              (MovingAverage* self);
double             moving_average_variance          (MovingAverage* self);
double             moving_average_std_dev           (MovingAverage* self);
void               moving_average_update            (MovingAverage* self, double     x);

/*--------------------------------------------------------------------*/
/*--___-------_-_------------___------------------------_---------------
   / _ \ _ _ | (_)_ _  ___  | _ \___ __ _ _ _ ___ _____(_)___ _ _  
  | (_) | ' \| | | ' \/ -_) |   / -_) _` | '_/ -_|_-<_-< / _ \ ' \ 
   \___/|_||_|_|_|_||_\___| |_|_\___\__, |_| \___/__/__/_\___/_||_|
------------------------------------|___/-------------------------------
  Calculate the means, variances, covariance, slope, y-intercept and 
  correlation coeficient of two signals. This is an online algorithm
  that operates one sample at a time.
----------------------------------------------------------------------*/
typedef struct     opaque_online_regression_struct  OnlineRegression;
OnlineRegression*  online_regression_new            ();
void               online_regression_init           (OnlineRegression* self);
OnlineRegression*  online_regression_destroy        (OnlineRegression* self);
int                online_regression_n              (OnlineRegression* self);
double             online_regression_covariance     (OnlineRegression* self);
double             online_regression_slope          (OnlineRegression* self);
double             online_regression_y_intercept    (OnlineRegression* self);
double             online_regression_r_squared      (OnlineRegression* self);
void               online_regression_update         (OnlineRegression* self, double     a_data, double     b_data);

/*--------------------------------------------------------------------*/
/*----------_-----------_---_-----------_____-_---------------_----_----    _    _ 
    /_\  __| |__ _ _ __| |_(_)_ _____  |_   _| |_  _ _ ___ __| |_ | |_  ___| |__| |
   / _ \/ _` / _` | '_ \  _| \ V / -_)   | | | ' \| '_/ -_|_-< ' \| ' \/ _ \ / _` |
  /_/ \_\__,_\__,_| .__/\__|_|\_/\___|   |_| |_||_|_| \___/__/_||_|_||_\___/_\__,_|
------------------|_|---------------------------------------------------
  Adaptive threshhold tells you when your signal goes a specified number
  of standard deviations above or below a moving average of a filtered
  version of your signal. update() returns +1 or -1 whenever your signal
  transitions to being above or below the threshold.
----------------------------------------------------------------------*/
typedef struct     opaque_adaptive_threshold_struct AdaptiveThreshold;
AdaptiveThreshold* adaptive_threshold_new              (unsigned N);
void               adaptive_threshold_init             (AdaptiveThreshold* self);
void               adaptive_threshold_clear            (AdaptiveThreshold* self);
AdaptiveThreshold* adaptive_threshold_destroy          (AdaptiveThreshold* self);
double             adaptive_threshold_smoothing        (AdaptiveThreshold* self);
void               adaptive_threshold_set_smoothing    (AdaptiveThreshold* self, double     coefficient);
double             adaptive_threshold_threshold_value  (AdaptiveThreshold* self); //mean + num std devs * std dev
double             adaptive_threshold_threshold        (AdaptiveThreshold* self); //num std devs
void               adaptive_threshold_set_threshold    (AdaptiveThreshold* self, double     std_devs);
double             adaptive_threshold_threshold_min    (AdaptiveThreshold* self);
void               adaptive_threshold_set_threshold_min(AdaptiveThreshold* self, double min); //raw value
double             adaptive_threshold_onset_signal     (AdaptiveThreshold* self);
double             adaptive_threshold_mean             (AdaptiveThreshold* self);
double             adaptive_threshold_update           (AdaptiveThreshold* self, double     x);

double             statistics_random_flat(); //(0, 1]
double             statistics_random_normal(double     mean, double     std_dev);
double             statistics_random_cauchy(double peak_location, double half_width_at_half_maximum);



#endif   //__IC_STATISTICS__



// --- BTT.h ---
/*----------------------------------------------------------------------
 ____             _                     _
| __ )  ___  __ _| |_    __ _ _ __   __| |
|  _ \ / _ \/ _` | __|  / _` | '_ \ / _` |
| |_) |  __/ (_| | |_  | (_| | | | | (_| |
|____/ \___|\__,_|\__|  \__,_|_| |_|\__,_|
 
 _____
|_   _|__ _ __ ___  _ __   ___
  | |/ _ \ '_ ` _ \| '_ \ / _ \
  | |  __/ | | | | | |_) | (_) |
  |_|\___|_| |_| |_| .__/ \___/
                   |_|
 _____               _    _
|_   _| __ __ _  ___| | _(_)_ __   __ _
  | || '__/ _` |/ __| |/ / | '_ \ / _` |
  | || | | (_| | (__|   <| | | | | (_| |
  |_||_|  \__,_|\___|_|\_\_|_| |_|\__, |
                                  |___/
------------------------------------------------------------------------

  Made by Michael Krzyzaniak
 
  Version:
    1.0

----------------------------------------------------------------------*/
#ifndef __BTT__
#define __BTT__ 1





/*--------------------------------------------------------------------*/
typedef enum
{
  BTT_COUNT_IN_TRACKING,
  BTT_ONSET_TRACKING,
  BTT_ONSET_AND_TEMPO_TRACKING,
  BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING,
  BTT_TEMPO_LOCKED_BEAT_TRACKING,
  BTT_METRONOME_MODE,
  BTT_NUM_TRACKING_MODES,
}btt_tracking_mode_t;

/*--------------------------------------------------------------------*/
#define BTT_SUGGESTED_SPECTRAL_FLUX_STFT_LEN         1024  // fft size
#define BTT_SUGGESTED_SPECTRAL_FLUX_STFT_OVERLAP     8     // hop size will be len / overlap
#define BTT_SUGGESTED_OSS_FILTER_ORDER               15    // this will delay the signal by about 7 samples
#define BTT_SUGGESTED_OSS_LENGTH                     1024  // enough to hold 3 seconds at 44.1kHz and hop of 128
#define BTT_SUGGESTED_ONSET_THRESHOLD_N              1024  // size of moving average of oss to see if onset occured
#define BTT_SUGGESTED_SAMPLE_RATE                    44100 // audio sample rate
#define BTT_SUGGESTED_CBSS_LENGTH                    1024  // should be at least a little bigger than the slowest tempo lag in oss samples

#define BTT_DEFAULT_ANALYSIS_LATENCY_ONSET_ADJUSTMENT 857
#define BTT_DEFAULT_ANALYSIS_LATENCY_BEAT_ADJUSTMENT  1270

/*--------------------------------------------------------------------*/
#define BTT_DEFAULT_MIN_TEMPO                        50    // BPM
#define BTT_DEFAULT_MAX_TEMPO                        200   // BPM
#define BTT_DEFAULT_SPECTRAL_COMPRESSION_GAMMA       0     // COMPRESSED(spectrum) = log(1+gamma|spectrum|) / log(1+gamma)
#define BTT_DEFAULT_AUTOCORRELATION_EXPONENT         0.5   // for generalized autocorrelation
#define BTT_DEFAULT_NUM_TEMPO_CANDIDATES             10    //
#define BTT_DEFAULT_TRACKING_MODE                    BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING
#define BTT_DEFAULT_OSS_FILTER_CUTOFF                10    // Hz
#define BTT_DEFAULT_USE_AMP_NORMALIZATION            0     // false
#define BTT_DEFAULT_ONSET_TREHSHOLD                  0.1   // std devs above mean OSS signal
#define BTT_DEFAULT_ONSET_TREHSHOLD_MIN              5.0   // raw flux value
#define BTT_DEFAULT_NOISE_CANCELLATION_THRESHOLD     -74   // dB per freq bin
#define BTT_DEFAULT_LOG_GAUSSIAN_TEMPO_WEIGHT_MEAN   120   // supress harmonics by favoring tempos closer to 120
#define BTT_DEFAULT_LOG_GAUSSIAN_TEMPO_WEIGHT_WIDTH  75    // oss samples starndard deviation
#define BTT_DEFAULT_GAUSSIAN_TEMPO_HISTOGRAM_DECAY   0.999 //
#define BTT_DEFAULT_GAUSSIAN_TEMPO_HISTOGRAM_WIDTH   5     // oss samples starndard deviation
#define BTT_DEFAULT_CBSS_ALPHA                       0.9   // 90% old, 10 percent new
#define BTT_DEFAULT_CBSS_ETA                         300   // width of gaussian window. Larger number is narrower window
#define BTT_DEFAULT_BEAT_PREDICTION_ADJUSTMENT       10    // oss samples earlier than detected
#define BTT_DEFAULT_PREDICTED_BEAT_TRIGGER_INDEX     20    //
#define BTT_DEFAULT_PREDICTED_BEAT_GAUSSIAN_WIDTH    10    // oss samples
#define BTT_DEFAULT_IGNORE_SPURIOUS_BEATS_DURATION   40    // percent of beat at current tempo
#define BTT_DEFAULT_COUNT_IN_N                        2

#define BTT_DEFAULT_XCORR_NUM_PULSES                 8     //
#define BTT_DEFAULT_XCORR_PULSE_LOCATIONS            {0, 1, 1.5, 2, 3, 4, 4.5, 6}
#define BTT_DEFAULT_XCORR_PULSE_VALUES               {2.0, 1.0, 0.5, 1.5, 1.5, 0.5, 0.5, 0.5}

/*--------------------------------------------------------------------*/
typedef struct Opaque_BTT_Struct BTT;

typedef void (*btt_onset_callback_t)             (void* SELF, unsigned long long sample_time);
typedef void (*btt_beat_callback_t)              (void* SELF, unsigned long long sample_time);

/*--------------------------------------------------------------------*/
/* if you use btt_new you are going to have to empirically determine the analysis_latency_adjustments using the utility in demos/analysis_latency/
  the analysis latency is caused by complex interactions between filters, buffering, adaptive thresholds, and other things, and I couldn't
  find a closed-form expression that caputures it. If you use btt_new_default you will be fine, if not, you have to manually calculate it.
 */
BTT*      btt_new                                (int spectral_flux_stft_len, int spectral_flux_stft_overlap,
                                                  int oss_filter_order      , int oss_length,
                                                  int cbss_length           , int onset_threshold_len, double sample_rate,
                                                  int analysis_latency_onset_adjustment, int analysis_latency_beat_adjustment);
BTT*      btt_new_default                        ();
BTT*      btt_destroy                            (BTT* self);
void      btt_process                            (BTT* self, dft_sample_t* input, int num_samples);
double    btt_get_sample_rate                    (BTT* self);
void      btt_init                               (BTT* self);
void      btt_clear                              (BTT* self);
void      btt_init_tempo                         (BTT* self, double bpm /*0 to clear tempo*/);

int       btt_get_beat_period_audio_samples      (BTT* self);
double    btt_get_tempo_bpm                      (BTT* self);
double    btt_get_tempo_certainty                (BTT* self);
void      btt_set_count_in_n                     (BTT* self, int n);
int       btt_get_count_in_n                     (BTT* self);

/* only valid in metronome mode */
void      btt_set_metronome_bpm                  (BTT* self, double bpm);

/* onset detection adjustments */
void      btt_set_use_amplitude_normalization    (BTT* self, int use);
int       btt_get_use_amplitude_normalization    (BTT* self);
void      btt_set_spectral_compression_gamma     (BTT* self, double gamma);
double    btt_get_spectral_compression_gamma     (BTT* self);
void      btt_set_oss_filter_cutoff              (BTT* self, double Hz);
double    btt_get_oss_filter_cutoff              (BTT* self);
void      btt_set_onset_threshold                (BTT* self, double num_std_devs);
double    btt_get_onset_threshold                (BTT* self);
void      btt_set_onset_threshold_min            (BTT* self, double value);
double    btt_get_onset_threshold_min            (BTT* self);
void      btt_set_noise_cancellation_threshold   (BTT* self, double dB /*negative*/);
double    btt_get_noise_cancellation_threshold   (BTT* self);

/* tempo tracking adjustments */
void      btt_set_autocorrelation_exponent       (BTT* self, double exponent);
double    btt_get_autocorrelation_exponent       (BTT* self);
void      btt_set_min_tempo                      (BTT* self, double min_tempo);
double    btt_get_min_tempo                      (BTT* self);
void      btt_set_max_tempo                      (BTT* self, double max_tempo);
double    btt_get_max_tempo                      (BTT* self);
void      btt_set_num_tempo_candidates           (BTT* self, int num_candidates);
int       btt_get_num_tempo_candidates           (BTT* self);
void      btt_set_gaussian_tempo_histogram_decay (BTT* self, double coefficient);
double    btt_get_gaussian_tempo_histogram_decay (BTT* self);
void      btt_set_gaussian_tempo_histogram_width (BTT* self, double width);
double    btt_get_gaussian_tempo_histogram_width (BTT* self);
void      btt_set_log_gaussian_tempo_weight_mean (BTT* self, double bpm);
double    btt_get_log_gaussian_tempo_weight_mean (BTT* self);
void      btt_set_log_gaussian_tempo_weight_width(BTT* self, double bpm);
double    btt_get_log_gaussian_tempo_weight_width(BTT* self);

/* beat tracking adjustments */
void      btt_set_cbss_alpha                     (BTT* self, double alpha);
double    btt_get_cbss_alpha                     (BTT* self);
void      btt_set_cbss_eta                       (BTT* self, double eta);
double    btt_get_cbss_eta                       (BTT* self);
void      btt_set_beat_prediction_adjustment     (BTT* self, int oss_samples_earlier);
int       btt_get_beat_prediction_adjustment     (BTT* self);
int       btt_get_beat_prediction_adjustment_audio_samples (BTT* self);
void      btt_set_predicted_beat_trigger_index   (BTT* self, int index);
int       btt_get_predicted_beat_trigger_index   (BTT* self);
void      btt_set_predicted_beat_gaussian_width  (BTT* self, double width);
double    btt_get_predicted_beat_gaussian_width  (BTT* self);
void      btt_set_ignore_spurious_beats_duration (BTT* self, double percent_of_tempo);
double    btt_get_ignore_spurious_beats_duration (BTT* self);
void      btt_set_analysis_latency_onset_adjustment(BTT* self, int adjustment);
int       btt_get_analysis_latency_onset_adjustment(BTT* self);
void      btt_set_analysis_latency_beat_adjustment(BTT* self, int adjustment);
int       btt_get_analysis_latency_beat_adjustment(BTT* self);

void                 btt_set_tracking_mode            (BTT* self, btt_tracking_mode_t mode);
btt_tracking_mode_t  btt_get_tracking_mode            (BTT* self);
const char*          btt_get_tracking_mode_string     (BTT* self);
void                 btt_set_onset_tracking_callback  (BTT* self, btt_onset_callback_t callback, void* callback_self);
btt_onset_callback_t btt_get_onset_tracking_callback  (BTT* self, void** returned_callback_self);
void                 btt_set_beat_tracking_callback   (BTT* self, btt_beat_callback_t callback, void* callback_self);
btt_beat_callback_t  btt_get_beat_tracking_callback   (BTT* self, void** returned_callback_self);



#endif   // __BTT__


// ======================================================================
//  IMPLEMENTATION
// ======================================================================

// --- fastsin.c ---
// Suppress MSVC C4305 (double→float truncation) for the sine lookup table
#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable: 4305)
#endif




extern const float sinTable[] = 
{
  0.000000,  0.001534,  0.003068,  0.004602,  0.006136,  0.007670,  0.009204,  0.010738,  0.012272,  0.013805,  
  0.015339,  0.016873,  0.018407,  0.019940,  0.021474,  0.023008,  0.024541,  0.026075,  0.027608,  0.029142,  
  0.030675,  0.032208,  0.033741,  0.035274,  0.036807,  0.038340,  0.039873,  0.041406,  0.042938,  0.044471,  
  0.046003,  0.047535,  0.049068,  0.050600,  0.052132,  0.053664,  0.055195,  0.056727,  0.058258,  0.059790,  
  0.061321,  0.062852,  0.064383,  0.065913,  0.067444,  0.068974,  0.070505,  0.072035,  0.073565,  0.075094,  
  0.076624,  0.078153,  0.079682,  0.081211,  0.082740,  0.084269,  0.085797,  0.087326,  0.088854,  0.090381,  
  0.091909,  0.093436,  0.094963,  0.096490,  0.098017,  0.099544,  0.101070,  0.102596,  0.104122,  0.105647,  
  0.107172,  0.108697,  0.110222,  0.111747,  0.113271,  0.114795,  0.116319,  0.117842,  0.119365,  0.120888,  
  0.122411,  0.123933,  0.125455,  0.126977,  0.128498,  0.130019,  0.131540,  0.133061,  0.134581,  0.136101,  
  0.137620,  0.139139,  0.140658,  0.142177,  0.143695,  0.145213,  0.146730,  0.148248,  0.149765,  0.151281,  
  0.152797,  0.154313,  0.155828,  0.157343,  0.158858,  0.160372,  0.161886,  0.163400,  0.164913,  0.166426,  
  0.167938,  0.169450,  0.170962,  0.172473,  0.173984,  0.175494,  0.177004,  0.178514,  0.180023,  0.181532,  
  0.183040,  0.184548,  0.186055,  0.187562,  0.189069,  0.190575,  0.192080,  0.193586,  0.195090,  0.196595,  
  0.198098,  0.199602,  0.201105,  0.202607,  0.204109,  0.205610,  0.207111,  0.208612,  0.210112,  0.211611,  
  0.213110,  0.214609,  0.216107,  0.217604,  0.219101,  0.220598,  0.222094,  0.223589,  0.225084,  0.226578,  
  0.228072,  0.229565,  0.231058,  0.232550,  0.234042,  0.235533,  0.237024,  0.238514,  0.240003,  0.241492,  
  0.242980,  0.244468,  0.245955,  0.247442,  0.248928,  0.250413,  0.251898,  0.253382,  0.254866,  0.256349,  
  0.257831,  0.259313,  0.260794,  0.262275,  0.263755,  0.265234,  0.266713,  0.268191,  0.269668,  0.271145,  
  0.272621,  0.274097,  0.275572,  0.277046,  0.278520,  0.279993,  0.281465,  0.282937,  0.284408,  0.285878,  
  0.287347,  0.288816,  0.290285,  0.291752,  0.293219,  0.294685,  0.296151,  0.297616,  0.299080,  0.300543,  
  0.302006,  0.303468,  0.304929,  0.306390,  0.307850,  0.309309,  0.310767,  0.312225,  0.313682,  0.315138,  
  0.316593,  0.318048,  0.319502,  0.320955,  0.322408,  0.323859,  0.325310,  0.326760,  0.328210,  0.329658,  
  0.331106,  0.332553,  0.334000,  0.335445,  0.336890,  0.338334,  0.339777,  0.341219,  0.342661,  0.344101,  
  0.345541,  0.346980,  0.348419,  0.349856,  0.351293,  0.352729,  0.354164,  0.355598,  0.357031,  0.358463,  
  0.359895,  0.361326,  0.362756,  0.364185,  0.365613,  0.367040,  0.368467,  0.369892,  0.371317,  0.372741,  
  0.374164,  0.375586,  0.377007,  0.378428,  0.379847,  0.381266,  0.382683,  0.384100,  0.385516,  0.386931,  
  0.388345,  0.389758,  0.391170,  0.392582,  0.393992,  0.395401,  0.396810,  0.398218,  0.399624,  0.401030,  
  0.402435,  0.403838,  0.405241,  0.406643,  0.408044,  0.409444,  0.410843,  0.412241,  0.413638,  0.415034,  
  0.416430,  0.417824,  0.419217,  0.420609,  0.422000,  0.423390,  0.424780,  0.426168,  0.427555,  0.428941,  
  0.430326,  0.431711,  0.433094,  0.434476,  0.435857,  0.437237,  0.438616,  0.439994,  0.441371,  0.442747,  
  0.444122,  0.445496,  0.446869,  0.448241,  0.449611,  0.450981,  0.452350,  0.453717,  0.455084,  0.456449,  
  0.457813,  0.459177,  0.460539,  0.461900,  0.463260,  0.464619,  0.465977,  0.467333,  0.468689,  0.470043,  
  0.471397,  0.472749,  0.474100,  0.475450,  0.476799,  0.478147,  0.479494,  0.480839,  0.482184,  0.483527,  
  0.484869,  0.486210,  0.487550,  0.488889,  0.490226,  0.491563,  0.492898,  0.494232,  0.495565,  0.496897,  
  0.498228,  0.499557,  0.500885,  0.502212,  0.503538,  0.504863,  0.506187,  0.507509,  0.508830,  0.510150,  
  0.511469,  0.512786,  0.514103,  0.515418,  0.516732,  0.518045,  0.519356,  0.520666,  0.521975,  0.523283,  
  0.524590,  0.525895,  0.527199,  0.528502,  0.529804,  0.531104,  0.532403,  0.533701,  0.534998,  0.536293,  
  0.537587,  0.538880,  0.540171,  0.541462,  0.542751,  0.544039,  0.545325,  0.546610,  0.547894,  0.549177,  
  0.550458,  0.551738,  0.553017,  0.554294,  0.555570,  0.556845,  0.558119,  0.559391,  0.560662,  0.561931,  
  0.563199,  0.564466,  0.565732,  0.566996,  0.568259,  0.569521,  0.570781,  0.572040,  0.573297,  0.574553,  
  0.575808,  0.577062,  0.578314,  0.579565,  0.580814,  0.582062,  0.583309,  0.584554,  0.585798,  0.587040,  
  0.588282,  0.589521,  0.590760,  0.591997,  0.593232,  0.594467,  0.595699,  0.596931,  0.598161,  0.599389,  
  0.600616,  0.601842,  0.603067,  0.604290,  0.605511,  0.606731,  0.607950,  0.609167,  0.610383,  0.611597,  
  0.612810,  0.614022,  0.615232,  0.616440,  0.617647,  0.618853,  0.620057,  0.621260,  0.622461,  0.623661,  
  0.624860,  0.626056,  0.627252,  0.628446,  0.629638,  0.630829,  0.632019,  0.633207,  0.634393,  0.635578,  
  0.636762,  0.637944,  0.639124,  0.640303,  0.641481,  0.642657,  0.643832,  0.645005,  0.646176,  0.647346,  
  0.648514,  0.649681,  0.650847,  0.652011,  0.653173,  0.654334,  0.655493,  0.656651,  0.657807,  0.658961,  
  0.660114,  0.661266,  0.662416,  0.663564,  0.664711,  0.665856,  0.667000,  0.668142,  0.669283,  0.670422,  
  0.671559,  0.672695,  0.673829,  0.674962,  0.676093,  0.677222,  0.678350,  0.679476,  0.680601,  0.681724,  
  0.682846,  0.683965,  0.685084,  0.686200,  0.687315,  0.688429,  0.689541,  0.690651,  0.691759,  0.692866,  
  0.693971,  0.695075,  0.696177,  0.697277,  0.698376,  0.699473,  0.700569,  0.701663,  0.702755,  0.703845,  
  0.704934,  0.706021,  0.707107,  0.708191,  0.709273,  0.710353,  0.711432,  0.712509,  0.713585,  0.714659,  
  0.715731,  0.716801,  0.717870,  0.718937,  0.720003,  0.721066,  0.722128,  0.723188,  0.724247,  0.725304,  
  0.726359,  0.727413,  0.728464,  0.729514,  0.730563,  0.731609,  0.732654,  0.733697,  0.734739,  0.735779,  
  0.736817,  0.737853,  0.738887,  0.739920,  0.740951,  0.741980,  0.743008,  0.744034,  0.745058,  0.746080,  
  0.747101,  0.748119,  0.749136,  0.750152,  0.751165,  0.752177,  0.753187,  0.754195,  0.755201,  0.756206,  
  0.757209,  0.758210,  0.759209,  0.760207,  0.761202,  0.762196,  0.763188,  0.764179,  0.765167,  0.766154,  
  0.767139,  0.768122,  0.769103,  0.770083,  0.771061,  0.772036,  0.773010,  0.773983,  0.774953,  0.775922,  
  0.776888,  0.777853,  0.778817,  0.779778,  0.780737,  0.781695,  0.782651,  0.783605,  0.784557,  0.785507,  
  0.786455,  0.787402,  0.788346,  0.789289,  0.790230,  0.791169,  0.792107,  0.793042,  0.793975,  0.794907,  
  0.795837,  0.796765,  0.797691,  0.798615,  0.799537,  0.800458,  0.801376,  0.802293,  0.803208,  0.804120,  
  0.805031,  0.805940,  0.806848,  0.807753,  0.808656,  0.809558,  0.810457,  0.811355,  0.812251,  0.813144,  
  0.814036,  0.814926,  0.815814,  0.816701,  0.817585,  0.818467,  0.819348,  0.820226,  0.821102,  0.821977,  
  0.822850,  0.823721,  0.824589,  0.825456,  0.826321,  0.827184,  0.828045,  0.828904,  0.829761,  0.830616,  
  0.831470,  0.832321,  0.833170,  0.834018,  0.834863,  0.835706,  0.836548,  0.837387,  0.838225,  0.839060,  
  0.839894,  0.840725,  0.841555,  0.842383,  0.843208,  0.844032,  0.844854,  0.845673,  0.846491,  0.847307,  
  0.848120,  0.848932,  0.849742,  0.850549,  0.851355,  0.852159,  0.852961,  0.853760,  0.854558,  0.855354,  
  0.856147,  0.856939,  0.857729,  0.858516,  0.859302,  0.860085,  0.860867,  0.861646,  0.862424,  0.863199,  
  0.863973,  0.864744,  0.865514,  0.866281,  0.867046,  0.867809,  0.868571,  0.869330,  0.870087,  0.870842,  
  0.871595,  0.872346,  0.873095,  0.873842,  0.874587,  0.875329,  0.876070,  0.876809,  0.877545,  0.878280,  
  0.879012,  0.879743,  0.880471,  0.881197,  0.881921,  0.882643,  0.883363,  0.884081,  0.884797,  0.885511,  
  0.886223,  0.886932,  0.887640,  0.888345,  0.889048,  0.889750,  0.890449,  0.891146,  0.891841,  0.892534,  
  0.893224,  0.893913,  0.894599,  0.895284,  0.895966,  0.896646,  0.897325,  0.898001,  0.898674,  0.899346,  
  0.900016,  0.900683,  0.901349,  0.902012,  0.902673,  0.903332,  0.903989,  0.904644,  0.905297,  0.905947,  
  0.906596,  0.907242,  0.907886,  0.908528,  0.909168,  0.909806,  0.910441,  0.911075,  0.911706,  0.912335,  
  0.912962,  0.913587,  0.914210,  0.914830,  0.915449,  0.916065,  0.916679,  0.917291,  0.917901,  0.918508,  
  0.919114,  0.919717,  0.920318,  0.920917,  0.921514,  0.922109,  0.922701,  0.923291,  0.923880,  0.924465,  
  0.925049,  0.925631,  0.926210,  0.926787,  0.927363,  0.927935,  0.928506,  0.929075,  0.929641,  0.930205,  
  0.930767,  0.931327,  0.931884,  0.932440,  0.932993,  0.933544,  0.934093,  0.934639,  0.935184,  0.935726,  
  0.936266,  0.936803,  0.937339,  0.937872,  0.938404,  0.938932,  0.939459,  0.939984,  0.940506,  0.941026,  
  0.941544,  0.942060,  0.942573,  0.943084,  0.943593,  0.944100,  0.944605,  0.945107,  0.945607,  0.946105,  
  0.946601,  0.947094,  0.947586,  0.948075,  0.948561,  0.949046,  0.949528,  0.950008,  0.950486,  0.950962,  
  0.951435,  0.951906,  0.952375,  0.952842,  0.953306,  0.953768,  0.954228,  0.954686,  0.955141,  0.955594,  
  0.956045,  0.956494,  0.956940,  0.957385,  0.957826,  0.958266,  0.958703,  0.959139,  0.959572,  0.960002,  
  0.960431,  0.960857,  0.961280,  0.961702,  0.962121,  0.962538,  0.962953,  0.963366,  0.963776,  0.964184,  
  0.964590,  0.964993,  0.965394,  0.965793,  0.966190,  0.966584,  0.966976,  0.967366,  0.967754,  0.968139,  
  0.968522,  0.968903,  0.969281,  0.969657,  0.970031,  0.970403,  0.970772,  0.971139,  0.971504,  0.971866,  
  0.972227,  0.972584,  0.972940,  0.973293,  0.973644,  0.973993,  0.974339,  0.974684,  0.975025,  0.975365,  
  0.975702,  0.976037,  0.976370,  0.976700,  0.977028,  0.977354,  0.977677,  0.977998,  0.978317,  0.978634,  
  0.978948,  0.979260,  0.979570,  0.979877,  0.980182,  0.980485,  0.980785,  0.981083,  0.981379,  0.981673,  
  0.981964,  0.982253,  0.982539,  0.982824,  0.983105,  0.983385,  0.983662,  0.983937,  0.984210,  0.984480,  
  0.984748,  0.985014,  0.985278,  0.985539,  0.985798,  0.986054,  0.986308,  0.986560,  0.986809,  0.987057,  
  0.987301,  0.987544,  0.987784,  0.988022,  0.988258,  0.988491,  0.988722,  0.988950,  0.989177,  0.989400,  
  0.989622,  0.989841,  0.990058,  0.990273,  0.990485,  0.990695,  0.990903,  0.991108,  0.991311,  0.991511,  
  0.991710,  0.991906,  0.992099,  0.992291,  0.992480,  0.992666,  0.992850,  0.993032,  0.993212,  0.993389,  
  0.993564,  0.993737,  0.993907,  0.994075,  0.994240,  0.994404,  0.994565,  0.994723,  0.994879,  0.995033,  
  0.995185,  0.995334,  0.995481,  0.995625,  0.995767,  0.995907,  0.996045,  0.996180,  0.996313,  0.996443,  
  0.996571,  0.996697,  0.996820,  0.996941,  0.997060,  0.997176,  0.997290,  0.997402,  0.997511,  0.997618,  
  0.997723,  0.997825,  0.997925,  0.998023,  0.998118,  0.998211,  0.998302,  0.998390,  0.998476,  0.998559,  
  0.998640,  0.998719,  0.998795,  0.998870,  0.998941,  0.999011,  0.999078,  0.999142,  0.999205,  0.999265,  
  0.999322,  0.999378,  0.999431,  0.999481,  0.999529,  0.999575,  0.999619,  0.999660,  0.999699,  0.999735,  
  0.999769,  0.999801,  0.999831,  0.999858,  0.999882,  0.999905,  0.999925,  0.999942,  0.999958,  0.999971,  
  0.999981,  0.999989,  0.999995,  0.999999,  1.000000,  0.999999,  0.999995,  0.999989,  0.999981,  0.999971,  
  0.999958,  0.999942,  0.999925,  0.999905,  0.999882,  0.999858,  0.999831,  0.999801,  0.999769,  0.999735,  
  0.999699,  0.999660,  0.999619,  0.999575,  0.999529,  0.999481,  0.999431,  0.999378,  0.999322,  0.999265,  
  0.999205,  0.999142,  0.999078,  0.999011,  0.998941,  0.998870,  0.998795,  0.998719,  0.998640,  0.998559,  
  0.998476,  0.998390,  0.998302,  0.998211,  0.998118,  0.998023,  0.997925,  0.997825,  0.997723,  0.997618,  
  0.997511,  0.997402,  0.997290,  0.997176,  0.997060,  0.996941,  0.996820,  0.996697,  0.996571,  0.996443,  
  0.996313,  0.996180,  0.996045,  0.995907,  0.995767,  0.995625,  0.995481,  0.995334,  0.995185,  0.995033,  
  0.994879,  0.994723,  0.994565,  0.994404,  0.994240,  0.994075,  0.993907,  0.993737,  0.993564,  0.993389,  
  0.993212,  0.993032,  0.992850,  0.992666,  0.992480,  0.992291,  0.992099,  0.991906,  0.991710,  0.991511,  
  0.991311,  0.991108,  0.990903,  0.990695,  0.990485,  0.990273,  0.990058,  0.989841,  0.989622,  0.989400,  
  0.989177,  0.988950,  0.988722,  0.988491,  0.988258,  0.988022,  0.987784,  0.987544,  0.987301,  0.987057,  
  0.986809,  0.986560,  0.986308,  0.986054,  0.985798,  0.985539,  0.985278,  0.985014,  0.984748,  0.984480,  
  0.984210,  0.983937,  0.983662,  0.983385,  0.983105,  0.982824,  0.982539,  0.982253,  0.981964,  0.981673,  
  0.981379,  0.981083,  0.980785,  0.980485,  0.980182,  0.979877,  0.979570,  0.979260,  0.978948,  0.978634,  
  0.978317,  0.977998,  0.977677,  0.977354,  0.977028,  0.976700,  0.976370,  0.976037,  0.975702,  0.975365,  
  0.975025,  0.974684,  0.974339,  0.973993,  0.973644,  0.973293,  0.972940,  0.972584,  0.972227,  0.971866,  
  0.971504,  0.971139,  0.970772,  0.970403,  0.970031,  0.969657,  0.969281,  0.968903,  0.968522,  0.968139,  
  0.967754,  0.967366,  0.966976,  0.966584,  0.966190,  0.965793,  0.965394,  0.964993,  0.964590,  0.964184,  
  0.963776,  0.963366,  0.962953,  0.962538,  0.962121,  0.961702,  0.961280,  0.960857,  0.960431,  0.960002,  
  0.959572,  0.959139,  0.958703,  0.958266,  0.957826,  0.957385,  0.956940,  0.956494,  0.956045,  0.955594,  
  0.955141,  0.954686,  0.954228,  0.953768,  0.953306,  0.952842,  0.952375,  0.951906,  0.951435,  0.950962,  
  0.950486,  0.950008,  0.949528,  0.949046,  0.948561,  0.948075,  0.947586,  0.947094,  0.946601,  0.946105,  
  0.945607,  0.945107,  0.944605,  0.944100,  0.943593,  0.943084,  0.942573,  0.942060,  0.941544,  0.941026,  
  0.940506,  0.939984,  0.939459,  0.938932,  0.938404,  0.937872,  0.937339,  0.936803,  0.936266,  0.935726,  
  0.935184,  0.934639,  0.934093,  0.933544,  0.932993,  0.932440,  0.931884,  0.931327,  0.930767,  0.930205,  
  0.929641,  0.929075,  0.928506,  0.927935,  0.927363,  0.926787,  0.926210,  0.925631,  0.925049,  0.924465,  
  0.923880,  0.923291,  0.922701,  0.922109,  0.921514,  0.920917,  0.920318,  0.919717,  0.919114,  0.918508,  
  0.917901,  0.917291,  0.916679,  0.916065,  0.915449,  0.914830,  0.914210,  0.913587,  0.912962,  0.912335,  
  0.911706,  0.911075,  0.910441,  0.909806,  0.909168,  0.908528,  0.907886,  0.907242,  0.906596,  0.905947,  
  0.905297,  0.904644,  0.903989,  0.903332,  0.902673,  0.902012,  0.901349,  0.900683,  0.900016,  0.899346,  
  0.898674,  0.898001,  0.897325,  0.896646,  0.895966,  0.895284,  0.894599,  0.893913,  0.893224,  0.892534,  
  0.891841,  0.891146,  0.890449,  0.889750,  0.889048,  0.888345,  0.887640,  0.886932,  0.886223,  0.885511,  
  0.884797,  0.884081,  0.883363,  0.882643,  0.881921,  0.881197,  0.880471,  0.879743,  0.879012,  0.878280,  
  0.877545,  0.876809,  0.876070,  0.875329,  0.874587,  0.873842,  0.873095,  0.872346,  0.871595,  0.870842,  
  0.870087,  0.869330,  0.868571,  0.867809,  0.867046,  0.866281,  0.865514,  0.864744,  0.863973,  0.863199,  
  0.862424,  0.861646,  0.860867,  0.860085,  0.859302,  0.858516,  0.857729,  0.856939,  0.856147,  0.855354,  
  0.854558,  0.853760,  0.852961,  0.852159,  0.851355,  0.850549,  0.849742,  0.848932,  0.848120,  0.847307,  
  0.846491,  0.845673,  0.844854,  0.844032,  0.843208,  0.842383,  0.841555,  0.840725,  0.839894,  0.839060,  
  0.838225,  0.837387,  0.836548,  0.835706,  0.834863,  0.834018,  0.833170,  0.832321,  0.831470,  0.830616,  
  0.829761,  0.828904,  0.828045,  0.827184,  0.826321,  0.825456,  0.824589,  0.823721,  0.822850,  0.821977,  
  0.821102,  0.820226,  0.819348,  0.818467,  0.817585,  0.816701,  0.815814,  0.814926,  0.814036,  0.813144,  
  0.812251,  0.811355,  0.810457,  0.809558,  0.808656,  0.807753,  0.806848,  0.805940,  0.805031,  0.804120,  
  0.803208,  0.802293,  0.801376,  0.800458,  0.799537,  0.798615,  0.797691,  0.796765,  0.795837,  0.794907,  
  0.793975,  0.793042,  0.792107,  0.791169,  0.790230,  0.789289,  0.788346,  0.787402,  0.786455,  0.785507,  
  0.784557,  0.783605,  0.782651,  0.781695,  0.780737,  0.779778,  0.778817,  0.777853,  0.776888,  0.775922,  
  0.774953,  0.773983,  0.773010,  0.772036,  0.771061,  0.770083,  0.769103,  0.768122,  0.767139,  0.766154,  
  0.765167,  0.764179,  0.763188,  0.762196,  0.761202,  0.760207,  0.759209,  0.758210,  0.757209,  0.756206,  
  0.755201,  0.754195,  0.753187,  0.752177,  0.751165,  0.750152,  0.749136,  0.748119,  0.747101,  0.746080,  
  0.745058,  0.744034,  0.743008,  0.741980,  0.740951,  0.739920,  0.738887,  0.737853,  0.736817,  0.735779,  
  0.734739,  0.733697,  0.732654,  0.731609,  0.730563,  0.729514,  0.728464,  0.727413,  0.726359,  0.725304,  
  0.724247,  0.723188,  0.722128,  0.721066,  0.720003,  0.718937,  0.717870,  0.716801,  0.715731,  0.714659,  
  0.713585,  0.712509,  0.711432,  0.710353,  0.709273,  0.708191,  0.707107,  0.706021,  0.704934,  0.703845,  
  0.702755,  0.701663,  0.700569,  0.699473,  0.698376,  0.697277,  0.696177,  0.695075,  0.693971,  0.692866,  
  0.691759,  0.690651,  0.689541,  0.688429,  0.687315,  0.686200,  0.685084,  0.683965,  0.682846,  0.681724,  
  0.680601,  0.679476,  0.678350,  0.677222,  0.676093,  0.674962,  0.673829,  0.672695,  0.671559,  0.670422,  
  0.669283,  0.668142,  0.667000,  0.665856,  0.664711,  0.663564,  0.662416,  0.661266,  0.660114,  0.658961,  
  0.657807,  0.656651,  0.655493,  0.654334,  0.653173,  0.652011,  0.650847,  0.649681,  0.648514,  0.647346,  
  0.646176,  0.645005,  0.643832,  0.642657,  0.641481,  0.640303,  0.639124,  0.637944,  0.636762,  0.635578,  
  0.634393,  0.633207,  0.632019,  0.630829,  0.629638,  0.628446,  0.627252,  0.626056,  0.624860,  0.623661,  
  0.622461,  0.621260,  0.620057,  0.618853,  0.617647,  0.616440,  0.615232,  0.614022,  0.612810,  0.611597,  
  0.610383,  0.609167,  0.607950,  0.606731,  0.605511,  0.604290,  0.603067,  0.601842,  0.600616,  0.599389,  
  0.598161,  0.596931,  0.595699,  0.594467,  0.593232,  0.591997,  0.590760,  0.589521,  0.588282,  0.587040,  
  0.585798,  0.584554,  0.583309,  0.582062,  0.580814,  0.579565,  0.578314,  0.577062,  0.575808,  0.574553,  
  0.573297,  0.572040,  0.570781,  0.569521,  0.568259,  0.566996,  0.565732,  0.564466,  0.563199,  0.561931,  
  0.560662,  0.559391,  0.558119,  0.556845,  0.555570,  0.554294,  0.553017,  0.551738,  0.550458,  0.549177,  
  0.547894,  0.546610,  0.545325,  0.544039,  0.542751,  0.541462,  0.540171,  0.538880,  0.537587,  0.536293,  
  0.534998,  0.533701,  0.532403,  0.531104,  0.529804,  0.528502,  0.527199,  0.525895,  0.524590,  0.523283,  
  0.521975,  0.520666,  0.519356,  0.518045,  0.516732,  0.515418,  0.514103,  0.512786,  0.511469,  0.510150,  
  0.508830,  0.507509,  0.506187,  0.504863,  0.503538,  0.502212,  0.500885,  0.499557,  0.498228,  0.496897,  
  0.495565,  0.494232,  0.492898,  0.491563,  0.490226,  0.488889,  0.487550,  0.486210,  0.484869,  0.483527,  
  0.482184,  0.480839,  0.479494,  0.478147,  0.476799,  0.475450,  0.474100,  0.472749,  0.471397,  0.470043,  
  0.468689,  0.467333,  0.465977,  0.464619,  0.463260,  0.461900,  0.460539,  0.459177,  0.457813,  0.456449,  
  0.455084,  0.453717,  0.452350,  0.450981,  0.449611,  0.448241,  0.446869,  0.445496,  0.444122,  0.442747,  
  0.441371,  0.439994,  0.438616,  0.437237,  0.435857,  0.434476,  0.433094,  0.431711,  0.430326,  0.428941,  
  0.427555,  0.426168,  0.424780,  0.423390,  0.422000,  0.420609,  0.419217,  0.417824,  0.416430,  0.415034,  
  0.413638,  0.412241,  0.410843,  0.409444,  0.408044,  0.406643,  0.405241,  0.403838,  0.402435,  0.401030,  
  0.399624,  0.398218,  0.396810,  0.395401,  0.393992,  0.392582,  0.391170,  0.389758,  0.388345,  0.386931,  
  0.385516,  0.384100,  0.382683,  0.381266,  0.379847,  0.378428,  0.377007,  0.375586,  0.374164,  0.372741,  
  0.371317,  0.369892,  0.368467,  0.367040,  0.365613,  0.364185,  0.362756,  0.361326,  0.359895,  0.358463,  
  0.357031,  0.355598,  0.354164,  0.352729,  0.351293,  0.349856,  0.348419,  0.346980,  0.345541,  0.344101,  
  0.342661,  0.341219,  0.339777,  0.338334,  0.336890,  0.335445,  0.334000,  0.332553,  0.331106,  0.329658,  
  0.328210,  0.326760,  0.325310,  0.323859,  0.322408,  0.320955,  0.319502,  0.318048,  0.316593,  0.315138,  
  0.313682,  0.312225,  0.310767,  0.309309,  0.307850,  0.306390,  0.304929,  0.303468,  0.302006,  0.300543,  
  0.299080,  0.297616,  0.296151,  0.294685,  0.293219,  0.291752,  0.290285,  0.288816,  0.287347,  0.285878,  
  0.284408,  0.282937,  0.281465,  0.279993,  0.278520,  0.277046,  0.275572,  0.274097,  0.272621,  0.271145,  
  0.269668,  0.268191,  0.266713,  0.265234,  0.263755,  0.262275,  0.260794,  0.259313,  0.257831,  0.256349,  
  0.254866,  0.253382,  0.251898,  0.250413,  0.248928,  0.247442,  0.245955,  0.244468,  0.242980,  0.241492,  
  0.240003,  0.238514,  0.237024,  0.235533,  0.234042,  0.232550,  0.231058,  0.229565,  0.228072,  0.226578,  
  0.225084,  0.223589,  0.222094,  0.220598,  0.219101,  0.217604,  0.216107,  0.214609,  0.213110,  0.211611,  
  0.210112,  0.208612,  0.207111,  0.205610,  0.204109,  0.202607,  0.201105,  0.199602,  0.198098,  0.196595,  
  0.195090,  0.193586,  0.192080,  0.190575,  0.189069,  0.187562,  0.186055,  0.184548,  0.183040,  0.181532,  
  0.180023,  0.178514,  0.177004,  0.175494,  0.173984,  0.172473,  0.170962,  0.169450,  0.167938,  0.166426,  
  0.164913,  0.163400,  0.161886,  0.160372,  0.158858,  0.157343,  0.155828,  0.154313,  0.152797,  0.151281,  
  0.149765,  0.148248,  0.146730,  0.145213,  0.143695,  0.142177,  0.140658,  0.139139,  0.137620,  0.136101,  
  0.134581,  0.133061,  0.131540,  0.130019,  0.128498,  0.126977,  0.125455,  0.123933,  0.122411,  0.120888,  
  0.119365,  0.117842,  0.116319,  0.114795,  0.113271,  0.111747,  0.110222,  0.108697,  0.107172,  0.105647,  
  0.104122,  0.102596,  0.101070,  0.099544,  0.098017,  0.096490,  0.094963,  0.093436,  0.091909,  0.090381,  
  0.088854,  0.087326,  0.085797,  0.084269,  0.082740,  0.081211,  0.079682,  0.078153,  0.076624,  0.075094,  
  0.073565,  0.072035,  0.070505,  0.068974,  0.067444,  0.065913,  0.064383,  0.062852,  0.061321,  0.059790,  
  0.058258,  0.056727,  0.055195,  0.053664,  0.052132,  0.050600,  0.049068,  0.047535,  0.046003,  0.044471,  
  0.042938,  0.041406,  0.039873,  0.038340,  0.036807,  0.035274,  0.033741,  0.032208,  0.030675,  0.029142,  
  0.027608,  0.026075,  0.024541,  0.023008,  0.021474,  0.019940,  0.018407,  0.016873,  0.015339,  0.013805,  
  0.012272,  0.010738,  0.009204,  0.007670,  0.006136,  0.004602,  0.003068,  0.001534,  -0.000000,  -0.001534,  
  -0.003068,  -0.004602,  -0.006136,  -0.007670,  -0.009204,  -0.010738,  -0.012272,  -0.013805,  -0.015339,  -0.016873,  
  -0.018407,  -0.019940,  -0.021474,  -0.023008,  -0.024541,  -0.026075,  -0.027608,  -0.029142,  -0.030675,  -0.032208,  
  -0.033741,  -0.035274,  -0.036807,  -0.038340,  -0.039873,  -0.041406,  -0.042938,  -0.044471,  -0.046003,  -0.047535,  
  -0.049068,  -0.050600,  -0.052132,  -0.053664,  -0.055195,  -0.056727,  -0.058258,  -0.059790,  -0.061321,  -0.062852,  
  -0.064383,  -0.065913,  -0.067444,  -0.068974,  -0.070505,  -0.072035,  -0.073565,  -0.075094,  -0.076624,  -0.078153,  
  -0.079682,  -0.081211,  -0.082740,  -0.084269,  -0.085797,  -0.087326,  -0.088854,  -0.090381,  -0.091909,  -0.093436,  
  -0.094963,  -0.096490,  -0.098017,  -0.099544,  -0.101070,  -0.102596,  -0.104122,  -0.105647,  -0.107172,  -0.108697,  
  -0.110222,  -0.111747,  -0.113271,  -0.114795,  -0.116319,  -0.117842,  -0.119365,  -0.120888,  -0.122411,  -0.123933,  
  -0.125455,  -0.126977,  -0.128498,  -0.130019,  -0.131540,  -0.133061,  -0.134581,  -0.136101,  -0.137620,  -0.139139,  
  -0.140658,  -0.142177,  -0.143695,  -0.145213,  -0.146730,  -0.148248,  -0.149765,  -0.151281,  -0.152797,  -0.154313,  
  -0.155828,  -0.157343,  -0.158858,  -0.160372,  -0.161886,  -0.163400,  -0.164913,  -0.166426,  -0.167938,  -0.169450,  
  -0.170962,  -0.172473,  -0.173984,  -0.175494,  -0.177004,  -0.178514,  -0.180023,  -0.181532,  -0.183040,  -0.184548,  
  -0.186055,  -0.187562,  -0.189069,  -0.190575,  -0.192080,  -0.193586,  -0.195090,  -0.196595,  -0.198098,  -0.199602,  
  -0.201105,  -0.202607,  -0.204109,  -0.205610,  -0.207111,  -0.208612,  -0.210112,  -0.211611,  -0.213110,  -0.214609,  
  -0.216107,  -0.217604,  -0.219101,  -0.220598,  -0.222094,  -0.223589,  -0.225084,  -0.226578,  -0.228072,  -0.229565,  
  -0.231058,  -0.232550,  -0.234042,  -0.235533,  -0.237024,  -0.238514,  -0.240003,  -0.241492,  -0.242980,  -0.244468,  
  -0.245955,  -0.247442,  -0.248928,  -0.250413,  -0.251898,  -0.253382,  -0.254866,  -0.256349,  -0.257831,  -0.259313,  
  -0.260794,  -0.262275,  -0.263755,  -0.265234,  -0.266713,  -0.268191,  -0.269668,  -0.271145,  -0.272621,  -0.274097,  
  -0.275572,  -0.277046,  -0.278520,  -0.279993,  -0.281465,  -0.282937,  -0.284408,  -0.285878,  -0.287347,  -0.288816,  
  -0.290285,  -0.291752,  -0.293219,  -0.294685,  -0.296151,  -0.297616,  -0.299080,  -0.300543,  -0.302006,  -0.303468,  
  -0.304929,  -0.306390,  -0.307850,  -0.309309,  -0.310767,  -0.312225,  -0.313682,  -0.315138,  -0.316593,  -0.318048,  
  -0.319502,  -0.320955,  -0.322408,  -0.323859,  -0.325310,  -0.326760,  -0.328210,  -0.329658,  -0.331106,  -0.332553,  
  -0.334000,  -0.335445,  -0.336890,  -0.338334,  -0.339777,  -0.341219,  -0.342661,  -0.344101,  -0.345541,  -0.346980,  
  -0.348419,  -0.349856,  -0.351293,  -0.352729,  -0.354164,  -0.355598,  -0.357031,  -0.358463,  -0.359895,  -0.361326,  
  -0.362756,  -0.364185,  -0.365613,  -0.367040,  -0.368467,  -0.369892,  -0.371317,  -0.372741,  -0.374164,  -0.375586,  
  -0.377007,  -0.378428,  -0.379847,  -0.381266,  -0.382683,  -0.384100,  -0.385516,  -0.386931,  -0.388345,  -0.389758,  
  -0.391170,  -0.392582,  -0.393992,  -0.395401,  -0.396810,  -0.398218,  -0.399624,  -0.401030,  -0.402435,  -0.403838,  
  -0.405241,  -0.406643,  -0.408044,  -0.409444,  -0.410843,  -0.412241,  -0.413638,  -0.415034,  -0.416430,  -0.417824,  
  -0.419217,  -0.420609,  -0.422000,  -0.423390,  -0.424780,  -0.426168,  -0.427555,  -0.428941,  -0.430326,  -0.431711,  
  -0.433094,  -0.434476,  -0.435857,  -0.437237,  -0.438616,  -0.439994,  -0.441371,  -0.442747,  -0.444122,  -0.445496,  
  -0.446869,  -0.448241,  -0.449611,  -0.450981,  -0.452350,  -0.453717,  -0.455084,  -0.456449,  -0.457813,  -0.459177,  
  -0.460539,  -0.461900,  -0.463260,  -0.464619,  -0.465977,  -0.467333,  -0.468689,  -0.470043,  -0.471397,  -0.472749,  
  -0.474100,  -0.475450,  -0.476799,  -0.478147,  -0.479494,  -0.480839,  -0.482184,  -0.483527,  -0.484869,  -0.486210,  
  -0.487550,  -0.488889,  -0.490226,  -0.491563,  -0.492898,  -0.494232,  -0.495565,  -0.496897,  -0.498228,  -0.499557,  
  -0.500885,  -0.502212,  -0.503538,  -0.504863,  -0.506187,  -0.507509,  -0.508830,  -0.510150,  -0.511469,  -0.512786,  
  -0.514103,  -0.515418,  -0.516732,  -0.518045,  -0.519356,  -0.520666,  -0.521975,  -0.523283,  -0.524590,  -0.525895,  
  -0.527199,  -0.528502,  -0.529804,  -0.531104,  -0.532403,  -0.533701,  -0.534998,  -0.536293,  -0.537587,  -0.538880,  
  -0.540171,  -0.541462,  -0.542751,  -0.544039,  -0.545325,  -0.546610,  -0.547894,  -0.549177,  -0.550458,  -0.551738,  
  -0.553017,  -0.554294,  -0.555570,  -0.556845,  -0.558119,  -0.559391,  -0.560662,  -0.561931,  -0.563199,  -0.564466,  
  -0.565732,  -0.566996,  -0.568259,  -0.569521,  -0.570781,  -0.572040,  -0.573297,  -0.574553,  -0.575808,  -0.577062,  
  -0.578314,  -0.579565,  -0.580814,  -0.582062,  -0.583309,  -0.584554,  -0.585798,  -0.587040,  -0.588282,  -0.589521,  
  -0.590760,  -0.591997,  -0.593232,  -0.594467,  -0.595699,  -0.596931,  -0.598161,  -0.599389,  -0.600616,  -0.601842,  
  -0.603067,  -0.604290,  -0.605511,  -0.606731,  -0.607950,  -0.609167,  -0.610383,  -0.611597,  -0.612810,  -0.614022,  
  -0.615232,  -0.616440,  -0.617647,  -0.618853,  -0.620057,  -0.621260,  -0.622461,  -0.623661,  -0.624860,  -0.626056,  
  -0.627252,  -0.628446,  -0.629638,  -0.630829,  -0.632019,  -0.633207,  -0.634393,  -0.635578,  -0.636762,  -0.637944,  
  -0.639124,  -0.640303,  -0.641481,  -0.642657,  -0.643832,  -0.645005,  -0.646176,  -0.647346,  -0.648514,  -0.649681,  
  -0.650847,  -0.652011,  -0.653173,  -0.654334,  -0.655493,  -0.656651,  -0.657807,  -0.658961,  -0.660114,  -0.661266,  
  -0.662416,  -0.663564,  -0.664711,  -0.665856,  -0.667000,  -0.668142,  -0.669283,  -0.670422,  -0.671559,  -0.672695,  
  -0.673829,  -0.674962,  -0.676093,  -0.677222,  -0.678350,  -0.679476,  -0.680601,  -0.681724,  -0.682846,  -0.683965,  
  -0.685084,  -0.686200,  -0.687315,  -0.688429,  -0.689541,  -0.690651,  -0.691759,  -0.692866,  -0.693971,  -0.695075,  
  -0.696177,  -0.697277,  -0.698376,  -0.699473,  -0.700569,  -0.701663,  -0.702755,  -0.703845,  -0.704934,  -0.706021,  
  -0.707107,  -0.708191,  -0.709273,  -0.710353,  -0.711432,  -0.712509,  -0.713585,  -0.714659,  -0.715731,  -0.716801,  
  -0.717870,  -0.718937,  -0.720003,  -0.721066,  -0.722128,  -0.723188,  -0.724247,  -0.725304,  -0.726359,  -0.727413,  
  -0.728464,  -0.729514,  -0.730563,  -0.731609,  -0.732654,  -0.733697,  -0.734739,  -0.735779,  -0.736817,  -0.737853,  
  -0.738887,  -0.739920,  -0.740951,  -0.741980,  -0.743008,  -0.744034,  -0.745058,  -0.746080,  -0.747101,  -0.748119,  
  -0.749136,  -0.750152,  -0.751165,  -0.752177,  -0.753187,  -0.754195,  -0.755201,  -0.756206,  -0.757209,  -0.758210,  
  -0.759209,  -0.760207,  -0.761202,  -0.762196,  -0.763188,  -0.764179,  -0.765167,  -0.766154,  -0.767139,  -0.768122,  
  -0.769103,  -0.770083,  -0.771061,  -0.772036,  -0.773010,  -0.773983,  -0.774953,  -0.775922,  -0.776888,  -0.777853,  
  -0.778817,  -0.779778,  -0.780737,  -0.781695,  -0.782651,  -0.783605,  -0.784557,  -0.785507,  -0.786455,  -0.787402,  
  -0.788346,  -0.789289,  -0.790230,  -0.791169,  -0.792107,  -0.793042,  -0.793975,  -0.794907,  -0.795837,  -0.796765,  
  -0.797691,  -0.798615,  -0.799537,  -0.800458,  -0.801376,  -0.802293,  -0.803208,  -0.804120,  -0.805031,  -0.805940,  
  -0.806848,  -0.807753,  -0.808656,  -0.809558,  -0.810457,  -0.811355,  -0.812251,  -0.813144,  -0.814036,  -0.814926,  
  -0.815814,  -0.816701,  -0.817585,  -0.818467,  -0.819348,  -0.820226,  -0.821102,  -0.821977,  -0.822850,  -0.823721,  
  -0.824589,  -0.825456,  -0.826321,  -0.827184,  -0.828045,  -0.828904,  -0.829761,  -0.830616,  -0.831470,  -0.832321,  
  -0.833170,  -0.834018,  -0.834863,  -0.835706,  -0.836548,  -0.837387,  -0.838225,  -0.839060,  -0.839894,  -0.840725,  
  -0.841555,  -0.842383,  -0.843208,  -0.844032,  -0.844854,  -0.845673,  -0.846491,  -0.847307,  -0.848120,  -0.848932,  
  -0.849742,  -0.850549,  -0.851355,  -0.852159,  -0.852961,  -0.853760,  -0.854558,  -0.855354,  -0.856147,  -0.856939,  
  -0.857729,  -0.858516,  -0.859302,  -0.860085,  -0.860867,  -0.861646,  -0.862424,  -0.863199,  -0.863973,  -0.864744,  
  -0.865514,  -0.866281,  -0.867046,  -0.867809,  -0.868571,  -0.869330,  -0.870087,  -0.870842,  -0.871595,  -0.872346,  
  -0.873095,  -0.873842,  -0.874587,  -0.875329,  -0.876070,  -0.876809,  -0.877545,  -0.878280,  -0.879012,  -0.879743,  
  -0.880471,  -0.881197,  -0.881921,  -0.882643,  -0.883363,  -0.884081,  -0.884797,  -0.885511,  -0.886223,  -0.886932,  
  -0.887640,  -0.888345,  -0.889048,  -0.889750,  -0.890449,  -0.891146,  -0.891841,  -0.892534,  -0.893224,  -0.893913,  
  -0.894599,  -0.895284,  -0.895966,  -0.896646,  -0.897325,  -0.898001,  -0.898674,  -0.899346,  -0.900016,  -0.900683,  
  -0.901349,  -0.902012,  -0.902673,  -0.903332,  -0.903989,  -0.904644,  -0.905297,  -0.905947,  -0.906596,  -0.907242,  
  -0.907886,  -0.908528,  -0.909168,  -0.909806,  -0.910441,  -0.911075,  -0.911706,  -0.912335,  -0.912962,  -0.913587,  
  -0.914210,  -0.914830,  -0.915449,  -0.916065,  -0.916679,  -0.917291,  -0.917901,  -0.918508,  -0.919114,  -0.919717,  
  -0.920318,  -0.920917,  -0.921514,  -0.922109,  -0.922701,  -0.923291,  -0.923880,  -0.924465,  -0.925049,  -0.925631,  
  -0.926210,  -0.926787,  -0.927363,  -0.927935,  -0.928506,  -0.929075,  -0.929641,  -0.930205,  -0.930767,  -0.931327,  
  -0.931884,  -0.932440,  -0.932993,  -0.933544,  -0.934093,  -0.934639,  -0.935184,  -0.935726,  -0.936266,  -0.936803,  
  -0.937339,  -0.937872,  -0.938404,  -0.938932,  -0.939459,  -0.939984,  -0.940506,  -0.941026,  -0.941544,  -0.942060,  
  -0.942573,  -0.943084,  -0.943593,  -0.944100,  -0.944605,  -0.945107,  -0.945607,  -0.946105,  -0.946601,  -0.947094,  
  -0.947586,  -0.948075,  -0.948561,  -0.949046,  -0.949528,  -0.950008,  -0.950486,  -0.950962,  -0.951435,  -0.951906,  
  -0.952375,  -0.952842,  -0.953306,  -0.953768,  -0.954228,  -0.954686,  -0.955141,  -0.955594,  -0.956045,  -0.956494,  
  -0.956940,  -0.957385,  -0.957826,  -0.958266,  -0.958703,  -0.959139,  -0.959572,  -0.960002,  -0.960431,  -0.960857,  
  -0.961280,  -0.961702,  -0.962121,  -0.962538,  -0.962953,  -0.963366,  -0.963776,  -0.964184,  -0.964590,  -0.964993,  
  -0.965394,  -0.965793,  -0.966190,  -0.966584,  -0.966976,  -0.967366,  -0.967754,  -0.968139,  -0.968522,  -0.968903,  
  -0.969281,  -0.969657,  -0.970031,  -0.970403,  -0.970772,  -0.971139,  -0.971504,  -0.971866,  -0.972227,  -0.972584,  
  -0.972940,  -0.973293,  -0.973644,  -0.973993,  -0.974339,  -0.974684,  -0.975025,  -0.975365,  -0.975702,  -0.976037,  
  -0.976370,  -0.976700,  -0.977028,  -0.977354,  -0.977677,  -0.977998,  -0.978317,  -0.978634,  -0.978948,  -0.979260,  
  -0.979570,  -0.979877,  -0.980182,  -0.980485,  -0.980785,  -0.981083,  -0.981379,  -0.981673,  -0.981964,  -0.982253,  
  -0.982539,  -0.982824,  -0.983105,  -0.983385,  -0.983662,  -0.983937,  -0.984210,  -0.984480,  -0.984748,  -0.985014,  
  -0.985278,  -0.985539,  -0.985798,  -0.986054,  -0.986308,  -0.986560,  -0.986809,  -0.987057,  -0.987301,  -0.987544,  
  -0.987784,  -0.988022,  -0.988258,  -0.988491,  -0.988722,  -0.988950,  -0.989177,  -0.989400,  -0.989622,  -0.989841,  
  -0.990058,  -0.990273,  -0.990485,  -0.990695,  -0.990903,  -0.991108,  -0.991311,  -0.991511,  -0.991710,  -0.991906,  
  -0.992099,  -0.992291,  -0.992480,  -0.992666,  -0.992850,  -0.993032,  -0.993212,  -0.993389,  -0.993564,  -0.993737,  
  -0.993907,  -0.994075,  -0.994240,  -0.994404,  -0.994565,  -0.994723,  -0.994879,  -0.995033,  -0.995185,  -0.995334,  
  -0.995481,  -0.995625,  -0.995767,  -0.995907,  -0.996045,  -0.996180,  -0.996313,  -0.996443,  -0.996571,  -0.996697,  
  -0.996820,  -0.996941,  -0.997060,  -0.997176,  -0.997290,  -0.997402,  -0.997511,  -0.997618,  -0.997723,  -0.997825,  
  -0.997925,  -0.998023,  -0.998118,  -0.998211,  -0.998302,  -0.998390,  -0.998476,  -0.998559,  -0.998640,  -0.998719,  
  -0.998795,  -0.998870,  -0.998941,  -0.999011,  -0.999078,  -0.999142,  -0.999205,  -0.999265,  -0.999322,  -0.999378,  
  -0.999431,  -0.999481,  -0.999529,  -0.999575,  -0.999619,  -0.999660,  -0.999699,  -0.999735,  -0.999769,  -0.999801,  
  -0.999831,  -0.999858,  -0.999882,  -0.999905,  -0.999925,  -0.999942,  -0.999958,  -0.999971,  -0.999981,  -0.999989,  
  -0.999995,  -0.999999,  -1.000000,  -0.999999,  -0.999995,  -0.999989,  -0.999981,  -0.999971,  -0.999958,  -0.999942,  
  -0.999925,  -0.999905,  -0.999882,  -0.999858,  -0.999831,  -0.999801,  -0.999769,  -0.999735,  -0.999699,  -0.999660,  
  -0.999619,  -0.999575,  -0.999529,  -0.999481,  -0.999431,  -0.999378,  -0.999322,  -0.999265,  -0.999205,  -0.999142,  
  -0.999078,  -0.999011,  -0.998941,  -0.998870,  -0.998795,  -0.998719,  -0.998640,  -0.998559,  -0.998476,  -0.998390,  
  -0.998302,  -0.998211,  -0.998118,  -0.998023,  -0.997925,  -0.997825,  -0.997723,  -0.997618,  -0.997511,  -0.997402,  
  -0.997290,  -0.997176,  -0.997060,  -0.996941,  -0.996820,  -0.996697,  -0.996571,  -0.996443,  -0.996313,  -0.996180,  
  -0.996045,  -0.995907,  -0.995767,  -0.995625,  -0.995481,  -0.995334,  -0.995185,  -0.995033,  -0.994879,  -0.994723,  
  -0.994565,  -0.994404,  -0.994240,  -0.994075,  -0.993907,  -0.993737,  -0.993564,  -0.993389,  -0.993212,  -0.993032,  
  -0.992850,  -0.992666,  -0.992480,  -0.992291,  -0.992099,  -0.991906,  -0.991710,  -0.991511,  -0.991311,  -0.991108,  
  -0.990903,  -0.990695,  -0.990485,  -0.990273,  -0.990058,  -0.989841,  -0.989622,  -0.989400,  -0.989177,  -0.988950,  
  -0.988722,  -0.988491,  -0.988258,  -0.988022,  -0.987784,  -0.987544,  -0.987301,  -0.987057,  -0.986809,  -0.986560,  
  -0.986308,  -0.986054,  -0.985798,  -0.985539,  -0.985278,  -0.985014,  -0.984748,  -0.984480,  -0.984210,  -0.983937,  
  -0.983662,  -0.983385,  -0.983105,  -0.982824,  -0.982539,  -0.982253,  -0.981964,  -0.981673,  -0.981379,  -0.981083,  
  -0.980785,  -0.980485,  -0.980182,  -0.979877,  -0.979570,  -0.979260,  -0.978948,  -0.978634,  -0.978317,  -0.977998,  
  -0.977677,  -0.977354,  -0.977028,  -0.976700,  -0.976370,  -0.976037,  -0.975702,  -0.975365,  -0.975025,  -0.974684,  
  -0.974339,  -0.973993,  -0.973644,  -0.973293,  -0.972940,  -0.972584,  -0.972227,  -0.971866,  -0.971504,  -0.971139,  
  -0.970772,  -0.970403,  -0.970031,  -0.969657,  -0.969281,  -0.968903,  -0.968522,  -0.968139,  -0.967754,  -0.967366,  
  -0.966976,  -0.966584,  -0.966190,  -0.965793,  -0.965394,  -0.964993,  -0.964590,  -0.964184,  -0.963776,  -0.963366,  
  -0.962953,  -0.962538,  -0.962121,  -0.961702,  -0.961280,  -0.960857,  -0.960431,  -0.960002,  -0.959572,  -0.959139,  
  -0.958703,  -0.958266,  -0.957826,  -0.957385,  -0.956940,  -0.956494,  -0.956045,  -0.955594,  -0.955141,  -0.954686,  
  -0.954228,  -0.953768,  -0.953306,  -0.952842,  -0.952375,  -0.951906,  -0.951435,  -0.950962,  -0.950486,  -0.950008,  
  -0.949528,  -0.949046,  -0.948561,  -0.948075,  -0.947586,  -0.947094,  -0.946601,  -0.946105,  -0.945607,  -0.945107,  
  -0.944605,  -0.944100,  -0.943593,  -0.943084,  -0.942573,  -0.942060,  -0.941544,  -0.941026,  -0.940506,  -0.939984,  
  -0.939459,  -0.938932,  -0.938404,  -0.937872,  -0.937339,  -0.936803,  -0.936266,  -0.935726,  -0.935184,  -0.934639,  
  -0.934093,  -0.933544,  -0.932993,  -0.932440,  -0.931884,  -0.931327,  -0.930767,  -0.930205,  -0.929641,  -0.929075,  
  -0.928506,  -0.927935,  -0.927363,  -0.926787,  -0.926210,  -0.925631,  -0.925049,  -0.924465,  -0.923880,  -0.923291,  
  -0.922701,  -0.922109,  -0.921514,  -0.920917,  -0.920318,  -0.919717,  -0.919114,  -0.918508,  -0.917901,  -0.917291,  
  -0.916679,  -0.916065,  -0.915449,  -0.914830,  -0.914210,  -0.913587,  -0.912962,  -0.912335,  -0.911706,  -0.911075,  
  -0.910441,  -0.909806,  -0.909168,  -0.908528,  -0.907886,  -0.907242,  -0.906596,  -0.905947,  -0.905297,  -0.904644,  
  -0.903989,  -0.903332,  -0.902673,  -0.902012,  -0.901349,  -0.900683,  -0.900016,  -0.899346,  -0.898674,  -0.898001,  
  -0.897325,  -0.896646,  -0.895966,  -0.895284,  -0.894599,  -0.893913,  -0.893224,  -0.892534,  -0.891841,  -0.891146,  
  -0.890449,  -0.889750,  -0.889048,  -0.888345,  -0.887640,  -0.886932,  -0.886223,  -0.885511,  -0.884797,  -0.884081,  
  -0.883363,  -0.882643,  -0.881921,  -0.881197,  -0.880471,  -0.879743,  -0.879012,  -0.878280,  -0.877545,  -0.876809,  
  -0.876070,  -0.875329,  -0.874587,  -0.873842,  -0.873095,  -0.872346,  -0.871595,  -0.870842,  -0.870087,  -0.869330,  
  -0.868571,  -0.867809,  -0.867046,  -0.866281,  -0.865514,  -0.864744,  -0.863973,  -0.863199,  -0.862424,  -0.861646,  
  -0.860867,  -0.860085,  -0.859302,  -0.858516,  -0.857729,  -0.856939,  -0.856147,  -0.855354,  -0.854558,  -0.853760,  
  -0.852961,  -0.852159,  -0.851355,  -0.850549,  -0.849742,  -0.848932,  -0.848120,  -0.847307,  -0.846491,  -0.845673,  
  -0.844854,  -0.844032,  -0.843208,  -0.842383,  -0.841555,  -0.840725,  -0.839894,  -0.839060,  -0.838225,  -0.837387,  
  -0.836548,  -0.835706,  -0.834863,  -0.834018,  -0.833170,  -0.832321,  -0.831470,  -0.830616,  -0.829761,  -0.828904,  
  -0.828045,  -0.827184,  -0.826321,  -0.825456,  -0.824589,  -0.823721,  -0.822850,  -0.821977,  -0.821102,  -0.820226,  
  -0.819348,  -0.818467,  -0.817585,  -0.816701,  -0.815814,  -0.814926,  -0.814036,  -0.813144,  -0.812251,  -0.811355,  
  -0.810457,  -0.809558,  -0.808656,  -0.807753,  -0.806848,  -0.805940,  -0.805031,  -0.804120,  -0.803208,  -0.802293,  
  -0.801376,  -0.800458,  -0.799537,  -0.798615,  -0.797691,  -0.796765,  -0.795837,  -0.794907,  -0.793975,  -0.793042,  
  -0.792107,  -0.791169,  -0.790230,  -0.789289,  -0.788346,  -0.787402,  -0.786455,  -0.785507,  -0.784557,  -0.783605,  
  -0.782651,  -0.781695,  -0.780737,  -0.779778,  -0.778817,  -0.777853,  -0.776888,  -0.775922,  -0.774953,  -0.773983,  
  -0.773010,  -0.772036,  -0.771061,  -0.770083,  -0.769103,  -0.768122,  -0.767139,  -0.766154,  -0.765167,  -0.764179,  
  -0.763188,  -0.762196,  -0.761202,  -0.760207,  -0.759209,  -0.758210,  -0.757209,  -0.756206,  -0.755201,  -0.754195,  
  -0.753187,  -0.752177,  -0.751165,  -0.750152,  -0.749136,  -0.748119,  -0.747101,  -0.746080,  -0.745058,  -0.744034,  
  -0.743008,  -0.741980,  -0.740951,  -0.739920,  -0.738887,  -0.737853,  -0.736817,  -0.735779,  -0.734739,  -0.733697,  
  -0.732654,  -0.731609,  -0.730563,  -0.729514,  -0.728464,  -0.727413,  -0.726359,  -0.725304,  -0.724247,  -0.723188,  
  -0.722128,  -0.721066,  -0.720003,  -0.718937,  -0.717870,  -0.716801,  -0.715731,  -0.714659,  -0.713585,  -0.712509,  
  -0.711432,  -0.710353,  -0.709273,  -0.708191,  -0.707107,  -0.706021,  -0.704934,  -0.703845,  -0.702755,  -0.701663,  
  -0.700569,  -0.699473,  -0.698376,  -0.697277,  -0.696177,  -0.695075,  -0.693971,  -0.692866,  -0.691759,  -0.690651,  
  -0.689541,  -0.688429,  -0.687315,  -0.686200,  -0.685084,  -0.683965,  -0.682846,  -0.681724,  -0.680601,  -0.679476,  
  -0.678350,  -0.677222,  -0.676093,  -0.674962,  -0.673829,  -0.672695,  -0.671559,  -0.670422,  -0.669283,  -0.668142,  
  -0.667000,  -0.665856,  -0.664711,  -0.663564,  -0.662416,  -0.661266,  -0.660114,  -0.658961,  -0.657807,  -0.656651,  
  -0.655493,  -0.654334,  -0.653173,  -0.652011,  -0.650847,  -0.649681,  -0.648514,  -0.647346,  -0.646176,  -0.645005,  
  -0.643832,  -0.642657,  -0.641481,  -0.640303,  -0.639124,  -0.637944,  -0.636762,  -0.635578,  -0.634393,  -0.633207,  
  -0.632019,  -0.630829,  -0.629638,  -0.628446,  -0.627252,  -0.626056,  -0.624860,  -0.623661,  -0.622461,  -0.621260,  
  -0.620057,  -0.618853,  -0.617647,  -0.616440,  -0.615232,  -0.614022,  -0.612810,  -0.611597,  -0.610383,  -0.609167,  
  -0.607950,  -0.606731,  -0.605511,  -0.604290,  -0.603067,  -0.601842,  -0.600616,  -0.599389,  -0.598161,  -0.596931,  
  -0.595699,  -0.594467,  -0.593232,  -0.591997,  -0.590760,  -0.589521,  -0.588282,  -0.587040,  -0.585798,  -0.584554,  
  -0.583309,  -0.582062,  -0.580814,  -0.579565,  -0.578314,  -0.577062,  -0.575808,  -0.574553,  -0.573297,  -0.572040,  
  -0.570781,  -0.569521,  -0.568259,  -0.566996,  -0.565732,  -0.564466,  -0.563199,  -0.561931,  -0.560662,  -0.559391,  
  -0.558119,  -0.556845,  -0.555570,  -0.554294,  -0.553017,  -0.551738,  -0.550458,  -0.549177,  -0.547894,  -0.546610,  
  -0.545325,  -0.544039,  -0.542751,  -0.541462,  -0.540171,  -0.538880,  -0.537587,  -0.536293,  -0.534998,  -0.533701,  
  -0.532403,  -0.531104,  -0.529804,  -0.528502,  -0.527199,  -0.525895,  -0.524590,  -0.523283,  -0.521975,  -0.520666,  
  -0.519356,  -0.518045,  -0.516732,  -0.515418,  -0.514103,  -0.512786,  -0.511469,  -0.510150,  -0.508830,  -0.507509,  
  -0.506187,  -0.504863,  -0.503538,  -0.502212,  -0.500885,  -0.499557,  -0.498228,  -0.496897,  -0.495565,  -0.494232,  
  -0.492898,  -0.491563,  -0.490226,  -0.488889,  -0.487550,  -0.486210,  -0.484869,  -0.483527,  -0.482184,  -0.480839,  
  -0.479494,  -0.478147,  -0.476799,  -0.475450,  -0.474100,  -0.472749,  -0.471397,  -0.470043,  -0.468689,  -0.467333,  
  -0.465977,  -0.464619,  -0.463260,  -0.461900,  -0.460539,  -0.459177,  -0.457813,  -0.456449,  -0.455084,  -0.453717,  
  -0.452350,  -0.450981,  -0.449611,  -0.448241,  -0.446869,  -0.445496,  -0.444122,  -0.442747,  -0.441371,  -0.439994,  
  -0.438616,  -0.437237,  -0.435857,  -0.434476,  -0.433094,  -0.431711,  -0.430326,  -0.428941,  -0.427555,  -0.426168,  
  -0.424780,  -0.423390,  -0.422000,  -0.420609,  -0.419217,  -0.417824,  -0.416430,  -0.415034,  -0.413638,  -0.412241,  
  -0.410843,  -0.409444,  -0.408044,  -0.406643,  -0.405241,  -0.403838,  -0.402435,  -0.401030,  -0.399624,  -0.398218,  
  -0.396810,  -0.395401,  -0.393992,  -0.392582,  -0.391170,  -0.389758,  -0.388345,  -0.386931,  -0.385516,  -0.384100,  
  -0.382683,  -0.381266,  -0.379847,  -0.378428,  -0.377007,  -0.375586,  -0.374164,  -0.372741,  -0.371317,  -0.369892,  
  -0.368467,  -0.367040,  -0.365613,  -0.364185,  -0.362756,  -0.361326,  -0.359895,  -0.358463,  -0.357031,  -0.355598,  
  -0.354164,  -0.352729,  -0.351293,  -0.349856,  -0.348419,  -0.346980,  -0.345541,  -0.344101,  -0.342661,  -0.341219,  
  -0.339777,  -0.338334,  -0.336890,  -0.335445,  -0.334000,  -0.332553,  -0.331106,  -0.329658,  -0.328210,  -0.326760,  
  -0.325310,  -0.323859,  -0.322408,  -0.320955,  -0.319502,  -0.318048,  -0.316593,  -0.315138,  -0.313682,  -0.312225,  
  -0.310767,  -0.309309,  -0.307850,  -0.306390,  -0.304929,  -0.303468,  -0.302006,  -0.300543,  -0.299080,  -0.297616,  
  -0.296151,  -0.294685,  -0.293219,  -0.291752,  -0.290285,  -0.288816,  -0.287347,  -0.285878,  -0.284408,  -0.282937,  
  -0.281465,  -0.279993,  -0.278520,  -0.277046,  -0.275572,  -0.274097,  -0.272621,  -0.271145,  -0.269668,  -0.268191,  
  -0.266713,  -0.265234,  -0.263755,  -0.262275,  -0.260794,  -0.259313,  -0.257831,  -0.256349,  -0.254866,  -0.253382,  
  -0.251898,  -0.250413,  -0.248928,  -0.247442,  -0.245955,  -0.244468,  -0.242980,  -0.241492,  -0.240003,  -0.238514,  
  -0.237024,  -0.235533,  -0.234042,  -0.232550,  -0.231058,  -0.229565,  -0.228072,  -0.226578,  -0.225084,  -0.223589,  
  -0.222094,  -0.220598,  -0.219101,  -0.217604,  -0.216107,  -0.214609,  -0.213110,  -0.211611,  -0.210112,  -0.208612,  
  -0.207111,  -0.205610,  -0.204109,  -0.202607,  -0.201105,  -0.199602,  -0.198098,  -0.196595,  -0.195090,  -0.193586,  
  -0.192080,  -0.190575,  -0.189069,  -0.187562,  -0.186055,  -0.184548,  -0.183040,  -0.181532,  -0.180023,  -0.178514,  
  -0.177004,  -0.175494,  -0.173984,  -0.172473,  -0.170962,  -0.169450,  -0.167938,  -0.166426,  -0.164913,  -0.163400,  
  -0.161886,  -0.160372,  -0.158858,  -0.157343,  -0.155828,  -0.154313,  -0.152797,  -0.151281,  -0.149765,  -0.148248,  
  -0.146730,  -0.145213,  -0.143695,  -0.142177,  -0.140658,  -0.139139,  -0.137620,  -0.136101,  -0.134581,  -0.133061,  
  -0.131540,  -0.130019,  -0.128498,  -0.126977,  -0.125455,  -0.123933,  -0.122411,  -0.120888,  -0.119365,  -0.117842,  
  -0.116319,  -0.114795,  -0.113271,  -0.111747,  -0.110222,  -0.108697,  -0.107172,  -0.105647,  -0.104122,  -0.102596,  
  -0.101070,  -0.099544,  -0.098017,  -0.096490,  -0.094963,  -0.093436,  -0.091909,  -0.090381,  -0.088854,  -0.087326,  
  -0.085797,  -0.084269,  -0.082740,  -0.081211,  -0.079682,  -0.078153,  -0.076624,  -0.075094,  -0.073565,  -0.072035,  
  -0.070505,  -0.068974,  -0.067444,  -0.065913,  -0.064383,  -0.062852,  -0.061321,  -0.059790,  -0.058258,  -0.056727,  
  -0.055195,  -0.053664,  -0.052132,  -0.050600,  -0.049068,  -0.047535,  -0.046003,  -0.044471,  -0.042938,  -0.041406,  
  -0.039873,  -0.038340,  -0.036807,  -0.035274,  -0.033741,  -0.032208,  -0.030675,  -0.029142,  -0.027608,  -0.026075,  
  -0.024541,  -0.023008,  -0.021474,  -0.019940,  -0.018407,  -0.016873,  -0.015339,  -0.013805,  -0.012272,  -0.010738,  
  -0.009204,  -0.007670,  -0.006136,  -0.004602,  -0.003068,  -0.001534,  
};


#ifdef _MSC_VER
  #pragma warning(pop)
#endif


// --- DFT.c ---
/*__--------------_-----------_---------------------__---------------------
 / _|___ _  _ _ _(_)___ _ _  | |_ _ _ __ _ _ _  ___/ _|___ _ _ _ __  
|  _/ _ \ || | '_| / -_) '_| |  _| '_/ _` | ' \(_-<  _/ _ \ '_| '  \ 
|_| \___/\_,_|_| |_\___|_|    \__|_| \__,_|_||_/__/_| \___/_| |_|_|_|
-------------------------------------------------------------------------*/
//MODIFIED AUGUST 29 2018 TO FIX ERRORS IN REAL FORWARD AND INVERSE TRANSFORMS!
//MODIFIED MARCH  25 2023 TO INCLUDE IN PLACE REAL TRANSFORMS







/*-----------------------------------------------------------------------*/
//produces output in bit-reversed order (Decimation in Frequency)
void dft_raw_forward_dft(dft_sample_t* real, dft_sample_t* imag, int N)
{
  fastsin_t two_pi_over_N = (fastsin_t)(SIN_TWO_PI / N), omega_two_pi_over_n;
  int sub_transform, butterfly;
  int num_sub_transforms = 1, num_butterflies = N/2, omega;
  dft_sample_t wr, wi, *r=real, *i=imag, tempr, tempi;
  int top_index = 0, bottom_index;
  
  while(num_sub_transforms < N)
    {
      top_index = 0;
      for(sub_transform=0; sub_transform<num_sub_transforms; sub_transform++)
        {
          omega = 0;
          for(butterfly=0; butterfly<num_butterflies; butterfly++)
            {
              bottom_index        = num_butterflies + top_index;
              omega_two_pi_over_n = omega * two_pi_over_N;
              wr                  =  fastcos(omega_two_pi_over_n);
              wi                  = -fastsin(omega_two_pi_over_n);
              tempr               = r[top_index];
              tempi               = i[top_index];
              r[top_index]       += r[bottom_index];
              i[top_index]       += i[bottom_index];
              r[bottom_index]     = tempr - r[bottom_index];
              i[bottom_index]     = tempi - i[bottom_index];
              tempr = (r[bottom_index] * wr) - (i[bottom_index] * wi);
              tempi = (r[bottom_index] * wi) + (i[bottom_index] * wr);
              r[bottom_index]     = tempr;
              i[bottom_index]     = tempi;
              omega              += num_sub_transforms;
              top_index++;
            }
          top_index += num_butterflies;
        }
      num_sub_transforms <<= 1;
      num_butterflies    >>= 1;
    }
}

/*-----------------------------------------------------------------------*/
//takes input in bit-reversed order (Decimation in Time)
void dft_raw_inverse_dft(dft_sample_t* real, dft_sample_t* imag, int N)
{
  fastsin_t two_pi_over_N = (fastsin_t)(SIN_TWO_PI / N), omega_two_pi_over_n;
  int sub_transform, butterfly;
  int num_sub_transforms = N, num_butterflies = 1, omega;
  dft_sample_t wr, wi, *r=real, *i=imag, tempr, tempi;
  int top_index = 0, bottom_index;
  
  while((num_sub_transforms >>= 1) > 0)
    {
      top_index = 0;
      for(sub_transform=0; sub_transform<num_sub_transforms; sub_transform++)
        {
          omega = 0;
          for(butterfly=0; butterfly<num_butterflies; butterfly++)
            {
              bottom_index        = num_butterflies + top_index;
              omega_two_pi_over_n = omega * two_pi_over_N;
              wr                  = fastcos(omega_two_pi_over_n);
              wi                  = fastsin(omega_two_pi_over_n);
              tempr = (r[bottom_index] * wr) - (i[bottom_index] * wi);
              tempi = (r[bottom_index] * wi) + (i[bottom_index] * wr);
              wr                  = r[top_index]; 
              wi                  = i[top_index];
              r[top_index]       += tempr;
              i[top_index]       += tempi;
              r[bottom_index]     = wr - tempr;
              i[bottom_index]     = wi - tempi;
              omega              += num_sub_transforms;
              top_index++;
            }
          top_index += num_butterflies;
        }
      num_butterflies <<= 1;
    }
 
  tempr = 1.0/N;
  for(omega=0; omega<N; omega++)
    {
      r[omega] *= tempr;
      i[omega] *= tempr;
    }
}

/*-----------------------------------------------------------------------*/
void   dft_complex_forward_dft (dft_sample_t* real,   dft_sample_t* imag,   int N)
{
  //normal order in, normal order out
  dft_raw_forward_dft(real, imag, N);
  dft_bit_reverse_indices(real, imag, N);
}

/*-----------------------------------------------------------------------*/
void   dft_complex_inverse_dft (dft_sample_t* real,   dft_sample_t* imag,   int N)
{
  //normal order in, normal order out
  dft_bit_reverse_indices(real, imag, N);
  dft_raw_inverse_dft(real, imag, N);
}

/*-----------------------------------------------------------------------*/
void dft_real_forward_dft(dft_sample_t* real, dft_sample_t* imag, int N)
{
  //takes and returns normal order. real input, complex output
  //i.e. give me real_1 and I'll give back real and imag
  //computes positive and negative frequencies
  
  int i;
  int N_over_2 = N >> 1;
  int two_i;
  fastsin_t pi_over_N = SIN_PI / N_over_2;
  fastsin_t i_pi_over_N;
  float ar, ai, br, bi;
  dft_sample_t r1, i1;
  
  for(i=0; i<N_over_2; i++)
    {
      two_i = i << 1;
      real[i] = real[two_i];
      imag[i] = real[two_i + 1];
    }
  
  dft_raw_forward_dft(real, imag, N_over_2);
  dft_bit_reverse_indices(real, imag, N_over_2);
  
  for(i=1; i<N_over_2; i++)
    {
      i_pi_over_N = i * pi_over_N;
      ar = 0.5 * (1-fastsin(i_pi_over_N));
      ai = 0.5 * ( -fastcos(i_pi_over_N));
      br = 0.5 * (1+fastsin(i_pi_over_N));
      bi = 0.5 * (  fastcos(i_pi_over_N));
      r1 = real[i] * ar - imag[i] * ai + real[N_over_2-i] * br + imag[N_over_2-i] * bi;
      i1 = imag[i] * ar + real[i] * ai + real[N_over_2-i] * bi - imag[N_over_2-i] * br;
      real[N-i] = r1;
      imag[N-i] = -i1;
    }

  real[N_over_2] = real[0] - imag[0];
  imag[N_over_2] = 0;
  ai = -0.5;
  ar = 0.5;
  bi = 0.5;
  br = 0.5;
  r1 = real[0] * ar - imag[0] * ai + real[0] * br + imag[0] * bi;
  i1 = imag[0] * ar + real[0] * ai + real[0] * bi - imag[0] * br;
  real[0] = r1;
  imag[0] = i1;

  for(i=1; i<N_over_2; i++)
    {
      real[i] = real[N-i];
      imag[i] = -imag[N-i];
    }
}

/*-----------------------------------------------------------------------*/
void dft_real_inverse_dft(dft_sample_t* real, dft_sample_t* imag, int N)
{
  //takes and returns normal order. complex input, real output
  //i.e. give me real and imag and I'll give back real
  //top halves of real and imag will be ignored (assumed to be conjugates of bottom halves)
  
  int i;
  int N_over_2 = N >> 1;
  int two_i;
  int i_plus_N_over_2;
  fastsin_t pi_over_N = SIN_PI / N_over_2;
  fastsin_t i_pi_over_N;
  float ar, ai, br, bi;
  dft_sample_t r1, i1;
  
  
  for(i=0; i<N_over_2; i++)
    {
      i_pi_over_N = i * pi_over_N;
      ar = 0.5 * (1-fastsin(i_pi_over_N));
      ai = 0.5 * (  fastcos(i_pi_over_N));
      br = 0.5 * (1+fastsin(i_pi_over_N));
      bi = 0.5 * (  -fastcos(i_pi_over_N));
      
      i_plus_N_over_2 = i + N_over_2;
      
      //put it in the top half of the buffers to conserve space
      r1 = real[i] * ar - imag[i] * ai + real[N_over_2-i] * br + imag[N_over_2-i] * bi;
      i1 = imag[i] * ar + real[i] * ai + real[N_over_2-i] * bi - imag[N_over_2-i] * br;
      real[i_plus_N_over_2] = r1;
      imag[i_plus_N_over_2] = i1;
    }

  dft_bit_reverse_indices(real+N_over_2, imag+N_over_2, N_over_2);
  dft_raw_inverse_dft    (real+N_over_2, imag+N_over_2, N_over_2);
      
  for(i=0; i<N_over_2; i++)
    {
      i_plus_N_over_2 = i + N_over_2;
      two_i = i << 1;
      real[two_i]   = real[i_plus_N_over_2];
      real[two_i+1] = imag[i_plus_N_over_2];
      imag[i] = 0;
      imag[i_plus_N_over_2] = 0;
    }
}

/*-----------------------------------------------------------------------*/
void dft_2_real_forward_dfts(dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N)
{
  //takes and returns normal order. real input, complex output
  //i.e. give me real_1 and real_2 and I'll give back real_1, real_2, imag_1 and imag_2
  
  int i;
  int N_over_2 = N >> 1;
  
  dft_sample_t *real = real_1, *imag = real_2;
  dft_sample_t r1, i1, r2, i2;
  
  dft_raw_forward_dft(real, imag, N);
  dft_bit_reverse_indices(real, imag, N);

  *imag_1 = 0;
  *imag_2 = 0;
  real_1[N_over_2] = real[N_over_2];
  imag_1[N_over_2] = 0;
  real_2[N_over_2] = imag[N_over_2];
  imag_2[N_over_2] = 0;
  
  for(i=1; i<N_over_2; i++)
    {
      r1 = 0.5 * (real[i] + real[N-i]);
      i1 = 0.5 * (imag[i] - imag[N-i]);
      r2 = 0.5 * (imag[i] + imag[N-i]);
      i2 = -0.5 * (real[i] - real[N-i]);
      real_1[i] = r1;
      imag_1[i] = i1;
      real_2[i] = r2;
      imag_2[i] = i2;
      real_1[N-i] = r1;
      imag_1[N-i] = -i1;
      real_2[N-i] = r2;
      imag_2[N-i] = -i2;
    }
}

/*-----------------------------------------------------------------------*/
void dft_2_real_inverse_dfts(dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N)
{
  //takes and returns normal order. complex input, real output;
  //i.e. give me real_1, real_2, imag_1 and imag_2 real_1 and real_2 and I'll give back real_1 and real_2
  //top halves of real and imag must contain correct values (cojugate of lower halves)
  
  int i;
  dft_sample_t *real = real_1, *imag = real_2;
  dft_sample_t temp;
  
  for(i=0; i<N; i++)
    {
      temp    = *real_1 - *imag_2;
      *real_2 = *imag_1 + *real_2;
      *real_1 = temp;
      ++real_1; ++real_2;
      ++imag_1; ++imag_2;
    }
  dft_bit_reverse_indices(real, imag, N);
  dft_raw_inverse_dft    (real, imag, N);
}

/*-----------------------------------------------------------------------*/
void   rdft_real_forward_dft     (dft_sample_t* real,  int N)
{
  int i, j, k;
  int n1, n2, n4;
  int i1, i2, i3, i4;
  int m=1; while(1<<m < N) m++;
  
  fastsin_t A, two_pi_over_N1;
  dft_sample_t temp, cc, ss, t1, t2;
  
  rdft_bit_reverse_indices(real, N);
  
  //length 2 butterfiles
  for(i=0; i<N; i+=2)
    {
      temp      = real[i];
      real[i]   = temp + real[i+1];
      real[i+1] = temp - real[i+1];
    }
    
  //other butterflies
  n2 = 1;
  for(k=1; k<m; k++)
    {
      n4 = n2;
      n2 <<= 1;
      n1 = n2 << 1;
      two_pi_over_N1 = SIN_PI / n2;
      
      for(i=0; i<N; i+=n1)
        {
          temp = real[i];
          real[i]   = temp + real[i+n2];
          real[i+n2] = temp - real[i+n2];
          real[i + n2 + n4] = -real[i + n2 + n4];
          A = two_pi_over_N1;
          
          for(j=1; j<n4; j++)
            {
              i1 = i+j;
              i2 = i-j+n2;
              i3 = i+j+n2;
              i4 = i-j+n1;
              cc = fastcos(A);
              ss = fastsin(A);
              A += two_pi_over_N1;
              t1 = real[i3] * cc + real[i4] * ss;
              t2 = real[i3] * ss - real[i4] * cc;
              real[i4] =  real[i2] - t2;
              real[i3] = -real[i2] - t2;
              real[i2] =  real[i1] - t1;
              real[i1] =  real[i1] + t1;
            }
        }
    }
}

/*-----------------------------------------------------------------------*/
void   rdft_real_inverse_dft     (dft_sample_t* real,  int N)
{
  int i, j, k;
  int n2, n4, n8;
  int is, id, i1, i2, i3, i4, i5, i6, i7, i8;
  
  fastsin_t two_pi_over_N2, A, A3;
  dft_sample_t t1, t2, t3, t4, t5;
  dft_sample_t cc1, cc3, ss1, ss3;
  dft_sample_t one_over_root_2 = fastsin(SIN_PI >> 2);
  
  int m=1; while(1<<m < N) m++;

  //L shaped butterflies
  n2 = N << 1;
  for(k=1; k<m; k++)
    {
      is = 0;
      id = n2;
      n2 >>= 1;
      n4 = n2 >> 2;
      n8 = n4 >> 1;
      two_pi_over_N2 = (SIN_PI / n2) << 1;

      while(is < N-1)
        {
          for(i=is; i<N; i+=id)
            {
              i1 = i;
              i2 = i1 + n4;
              i3 = i2 + n4;
              i4 = i3 + n4;
              
              t1 = real[i1] - real[i3];
              real[i1] =  real[i1] + real[i3];
              real[i2] *= 2;
              real[i3] = t1 - 2*real[i4];
              real[i4] = t1 + 2*real[i4];
              if(n4 == 1)
                continue;
              i1 += n8;
              i2 += n8;
              i3 += n8;
              i4 += n8;
              t1 = (real[i2] - real[i1]) * one_over_root_2;
              t2 = (real[i4] + real[i3]) * one_over_root_2;
              real[i1] += real[i2];
              real[i2] = real[i4] - real[i3];
              real[i3] = 2 * ((-t2) - t1);
              real[i4] = 2 * ((-t2) + t1);
            }
          is = 2 * id - n2;
          id <<= 2;
        }
      A = two_pi_over_N2;
        
      for(j=2; j<=n8; j++)
        {
          A3 = 3 * A;
          cc1 = fastcos(A);
          ss1 = fastsin(A);
          cc3 = fastcos(A3);
          ss3 = fastsin(A3);
          A = j * two_pi_over_N2;
          is = 0;
          id = n2 << 1;
  
          while(is < N-1)
            {
              for(i=is; i<N; i+=id)
                {
                  i1 = i+j - 1;
                  i2 = i1 + n4;
                  i3 = i2 + n4;
                  i4 = i3 + n4;
                  i5 = i + n4 - j + 1;
                  i6 = i5 + n4;
                  i7 = i6 + n4;
                  i8 = i7 + n4;
                  t1 = real[i1] - real[i6];
                  real[i1] += real[i6];
                  t2 = real[i5] - real[i2];
                  real[i5] += real[i2];
                  t3 = real[i8] + real[i3];
                  real[i6] = real[i8] - real[i3];
                  t4 = real[i4] + real[i7];
                  real[i2] = real[i4] - real[i7];
                  t5 = t1 - t4;
                  t1 += t4;
                  t4 = t2 - t3;
                  t2 += t3;
                  real[i3] =  t5*cc1 + t4*ss1;
                  real[i7] =  -t4*cc1 + t5*ss1;
                  real[i4] = t1*cc3 - t2*ss3;
                  real[i8] = t2*cc3 + t1*ss3;
                }
              is = 2 * id - n2;
              id *= 4;
            }
        }
    }
  
  //length 2 butterflies
  is = 1;
  id = 4;
  while(is < N)
    {
      for(i=is-1; i<N; i+=id)
        {
          i1 = i + 1;
          t1 = real[i];
          real[i] = t1 + real[i1];
          real[i1] = t1 - real[i1];
        }
      is = 2 * id - 1;
      id *= 4;
    }
  
  rdft_bit_reverse_indices(real, N);
  
  for(i=0; i<N; i++)
    real[i] /= (dft_sample_t)N;
}

/*-----------------------------------------------------------------------*/
void rdft_2_real_forward_dfts(dft_sample_t* real_1, dft_sample_t* real_2, int N)
{
  int i;
  int N_over_2 = N >> 1;
  
  dft_sample_t *real = real_1, *imag = real_2;
  dft_sample_t r1, i1, r2, i2;
  
  dft_raw_forward_dft(real, imag, N);
  dft_bit_reverse_indices(real, imag, N);

  real_1[0] = real[0];
  real_2[0] = imag[0];
  real_1[N_over_2] = real[N_over_2];
  real_2[N_over_2] = imag[N_over_2];
  
  for(i=1; i<N_over_2; i++)
    {
      r1 = 0.5 * (real[i] + real[N-i]);
      i1 = 0.5 * (imag[i] - imag[N-i]);
      r2 = 0.5 * (imag[i] + imag[N-i]);
      i2 = -0.5 * (real[i] - real[N-i]);
      real_1[i]   = r1;
      real_1[N-i] = i1;
      real_2[i]   = r2;
      real_2[N-i] = i2;
    }
}

/*-----------------------------------------------------------------------*/
void rdft_2_real_inverse_dfts(dft_sample_t* real_1, dft_sample_t* real_2, int N)
{
  int i;
  int N_over_2 = N >> 1;
  dft_sample_t *real = real_1, *imag = real_2;
  dft_sample_t r1, i1, r2, i2;
  
  //already correct
  /*
  real[0] = real_1[0];
  imag[0] = real_2[0];
  real[N_over_2] = real_1[N_over_2];
  imag[N_over_2] = real_2[N_over_2];
  */
  
  for(i=1; i<N_over_2; i++)
    {
      r1 = real_1[i]    - real_2[N-i];
      i1 = real_1[N-i]  + real_2[i];
      r2 = real_1[i]    + real_2[N-i];
      i2 = -real_1[N-i] + real_2[i];
      
      real[i] = r1;
      imag[i] = i1;
      real[N-i] = r2;
      imag[N-i] = i2;
    }

  dft_bit_reverse_indices(real, imag, N);
  dft_raw_inverse_dft    (real, imag, N);
}

/*-----------------------------------------------------------------------*/
void   rdft_rect_to_polar       (dft_sample_t* real,  int N)
{
  dft_sample_t temp;
  dft_sample_t* imag = real + N - 1;
  ++real;
  N >>= 1;
  while(N-- > 1)
    {
      temp = *real;
      *real = hypot(*real, *imag);
      *imag = atan2(*imag, temp);
      ++real, --imag;
    }
}

/*-----------------------------------------------------------------------*/
void   rdft_polar_to_rect       (dft_sample_t* real,  int N)
{
  dft_sample_t temp;
  dft_sample_t* imag = real + N - 1;
  ++real;
  N >>= 1;

  while(N-- > 1)
    {
      temp = *real;
      *real = *real * cos(*imag);
      *imag = temp  * sin(*imag);
      ++real, --imag;
    }
}

/*-----------------------------------------------------------------------*/
void   dft_real_convolve       (dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N)
{
  int i;
  int n_over_2 = N >> 1;
  dft_sample_t re, im;
  
  dft_2_real_forward_dfts(real_1, real_2, imag_1, imag_2, N);
  
  for(i=0; i<n_over_2; i++)
    {
      re = real_1[i]*real_2[i] - imag_1[i]*imag_2[i];
      im = real_1[i]*imag_2[i] + real_2[i]*imag_1[i];
      real_1[i] = re;
      imag_1[i] = im;
    }
  
  dft_real_inverse_dft(real_1, imag_1, N);
}

/*-----------------------------------------------------------------------*/
void   dft_real_correlate      (dft_sample_t* real_1, dft_sample_t* real_2, dft_sample_t* imag_1, dft_sample_t* imag_2, int N)
{
  int i;
  int n_over_2 = N >> 1;
  dft_sample_t re, im;
  
  dft_2_real_forward_dfts(real_1, real_2, imag_1, imag_2, N);
  
  for(i=0; i<n_over_2; i++)
    {
      re = real_1[i]*real_2[i] + imag_1[i]*imag_2[i];
      im = real_2[i]*imag_1[i] - real_1[i]*imag_2[i];
      real_1[i] = re;
      imag_1[i] = im;
    }
  
  dft_real_inverse_dft(real_1, imag_1, N);
}

/*-----------------------------------------------------------------------*/
void   dft_real_autocorrelate  (dft_sample_t* real, dft_sample_t* imag, int N)
{
  int i;
  int n_over_2 = N >> 1;
  dft_sample_t re, im;
  
  dft_real_forward_dft(real, imag, N);
  
  for(i=0; i<n_over_2; i++)
    {
      re = real[i]*real[i] + imag[i]*imag[i];
      im = real[i]*imag[i] - real[i]*imag[i];
      real[i] = re;
      imag[i] = im;
    }
  
  dft_real_inverse_dft(real, imag, N);
}

/*-----------------------------------------------------------------------*/
void   dft_real_generalized_autocorrelation  (dft_sample_t* real  , dft_sample_t* imag, int N, double exponent)
{
  int i;
  int n_over_2 = N >> 1;
  
  dft_real_forward_dft(real, imag, N);
  dft_rect_to_polar(real, imag, n_over_2);
  
  for(i=0; i<n_over_2; i++)
    {
      real[i] = pow(real[i], exponent);
      imag[i] = 0;
    }
  
  dft_real_inverse_dft(real, imag, N);
}

/*-----------------------------------------------------------------------*/
void   rdft_real_generalized_autocorrelation  (dft_sample_t* real, int N, double exponent)
{
  int i;
  int n_over_2 = N >> 1;
  
  rdft_real_forward_dft(real, N);
  rdft_rect_to_polar(real, N);
  
  real[0] = pow(real[0], exponent);
  for(i=1; i<n_over_2; i++)
    {
      real[i] = pow(real[i], exponent);
      real[N-i-1] = 0;
    }
  real[n_over_2] = pow(real[n_over_2], exponent);
  
  //it seems like polar to rect should be performed here but the Percival Tzanetakis paper says not in Eq 3.
  //further testing should probably be done
  
  rdft_real_inverse_dft(real, N);
}


/*-----------------------------------------------------------------------*/
void rdft_bit_reverse_indices(dft_sample_t* real, int N)
{
  dft_sample_t temp;
  
  int N_over_2 = N >> 1;
  unsigned n, bit, rev;
  unsigned n_reversed = N_over_2;
  
  for(n=1; n<N; n++)
    {
      if(n_reversed > n)
        {
          temp            = real[n]        ;
          real[n]         = real[n_reversed];
          real[n_reversed] = temp           ;
        }
      bit = ~n & (n + 1);
      rev = N_over_2 / bit;
      n_reversed ^= (N - 1) & ~(rev - 1);
    }
}

/*-----------------------------------------------------------------------*/
void dft_bit_reverse_indices(dft_sample_t* real, dft_sample_t* imag, int N)
{
  dft_sample_t temp;
  int N_over_2 = N >> 1;
  unsigned n, bit, rev;
  unsigned n_reversed = N_over_2;
  
  for(n=1; n<N; n++)
    {
      if(n_reversed > n)
        {
          temp            = real[n]         ;
          real[n]         = real[n_reversed];
          real[n_reversed] = temp           ;
          temp            = imag[n]         ;
          imag[n]         = imag[n_reversed];
          imag[n_reversed] = temp           ;
        }
      bit = ~n & (n + 1);
      rev = N_over_2 / bit;
      n_reversed ^= (N - 1) & ~(rev - 1);
    }
}

/*-----------------------------------------------------------------------*/
void dft_rect_to_polar(dft_sample_t* real, dft_sample_t* imag, int N)
{
  dft_sample_t temp;

  while(N-- > 0)
    {
      temp = *real;
      *real = hypot(*real, *imag);
      *imag = atan2(*imag, temp);
      ++real, ++imag;    
    }
}

/*-----------------------------------------------------------------------*/
void dft_polar_to_rect(dft_sample_t* real, dft_sample_t* imag, int N)
{
  dft_sample_t temp;

  while(N-- > 0)
    {
      temp = *real;
      *real = *real * cos(*imag);
      *imag = temp  * sin(*imag);
      ++real, ++imag;
    }
}

/*-----------------------------------------------------------------------*/
void   dft_magnitude_to_db     (dft_sample_t* real,   int N)
{
  int i;
  
  for(i=0; i<N; i++)
    {
      if(*real == 0)
        *real = FLT_MIN;
      else
        *real = 20 * log10(*real);
      ++real;
    }
}

/*-----------------------------------------------------------------------*/
void   dft_normalize_magnitude (dft_sample_t* real,   int N)
{
  dft_sample_t max = 0;
  int i;
  dft_sample_t* r = real;
  
  for(i=0; i<N; i++)
    {
      if(*r > max)
        max = *r;
      ++r;
    }
  
  r = real;
  if(max > 0)
    {
      max = 1 / max;
      for(i=0; i<N; i++)
        *r++ *= max;
    }
}

/*-----------------------------------------------------------------------*/
double dft_frequency_of_bin    (double bin, double sample_rate, int N)
{
  return bin * (sample_rate / N);
}

/*-----------------------------------------------------------------------*/
double dft_bin_of_frequency    (double hz, double sample_rate, int N)
{
  return hz * ((double)N / (double)sample_rate);
}

/*-----------------------------------------------------------------------*/
int dft_smallest_power_of_2_at_least_as_great_as(int n)
{
  int result = 1;
  while(result < n)
    result <<= 1;
  return result;
}

/*-----------------------------------------------------------------------*/
void dft_init_blackman_window(dft_sample_t* window, int N)
{
  //could be faster using fastsin
  //for now this is only initialization
  //so it doesn't really matter
  int i;
  long double phase = 0;
  long double phase_increment = 2*M_PI / (long double)N;
  double a = 0;
  double a0 = (1-a)/2.0;
  double a1 = 1/2.0;
  double a2 = a/2.0;
  
  for(i=0; i<N; i++)
    {
      *window++ = a0 - a1*cos(phase) + a2*cos(2*phase);
      phase += phase_increment;
    }  
}

/*-----------------------------------------------------------------------*/
void dft_init_hann_window(dft_sample_t* window, int N)
{
  int i;
  long double phase = 0;
  long double phase_increment = 2*M_PI / (long double)N;
  for(i=0; i<N; i++)
    {
      *window++ = 0.5 * (1-cos(phase));
      phase += phase_increment;
    }  
}

/*-----------------------------------------------------------------------*/
void dft_init_hamming_window(dft_sample_t* window, int N)
{
  int i;
  long double phase = 0;
  long double phase_increment = 2*M_PI / (long double)N;
  for(i=0; i<N; i++)
    {
      *window++ = 0.54 - 0.46 * cos(phase);
      phase += phase_increment;
    }  
}

/*-----------------------------------------------------------------------*/
void   dft_init_half_sine_window(dft_sample_t* window, int N)
{
  int i;
  long double coeff = M_PI / (long double)N;
  for(i=0; i<N; i++)
    *window++ = sin(coeff * (i+0.5));
}

/*-----------------------------------------------------------------------*/
void dft_apply_window(dft_sample_t* real, dft_sample_t* window, int N)
{
  while(N-- > 0)
    *real++ *= *window++;
}



// --- Filter.c ---




#define TWO_PI (M_PI * 2.0)

/*-------------------------------------------------*/
void filter_init_lowpass_coeffs  (Filter* self);
void filter_init_highpass_coeffs (Filter* self);
void filter_init_bandpass_coeffs (Filter* self);
void filter_init_bandstop_coeffs (Filter* self);
void filter_init_rect_window     (Filter* self);
void filter_init_hann_window     (Filter* self);
void filter_init_hamming_window  (Filter* self);
void filter_init_blackmann_window(Filter* self);

/*-------------------------------------------------*/
struct opaque_filter_struct
{
  filter_type_t   type;
  filter_window_t window_type;
  int             order, max_order;
  float           cutoff;
  //float           cutoff2;
  float           sample_rate;
  float*          prev_samples;
  float*          window;
  float*          coeffs;
};

/*-------------------------------------------------*/
Filter* filter_new(filter_type_t type, float cutoff, int order)
{
  Filter* self = (Filter*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      self->type         = type;
      self->order        = order;
      self->max_order    = order;
      self->cutoff       = cutoff;
      self->sample_rate  = 44100;
      self->prev_samples = (float*)calloc(order+1, sizeof(*(self->prev_samples)));
      if(self->prev_samples == NULL) return filter_destroy(self);
      self->window = (float*)calloc(order+1, sizeof(*(self->window)));
      if(self->window == NULL) return filter_destroy(self);
      self->coeffs = (float*)calloc(order+1, sizeof(*(self->coeffs)));
      if(self->coeffs == NULL) return filter_destroy(self);
      filter_set_window_type(self, FILTER_WINDOW_BLACKMANN); //triggers calculation of window and coeffs
    }
  
  return self;
}

/*-------------------------------------------------*/
Filter* filter_destroy(Filter* self)
{
  if(self != NULL)
    {
      if(self->window != NULL)
        free(self->window);
      if(self->coeffs != NULL)
        free(self->coeffs);
      if(self->prev_samples != NULL)
        free(self->prev_samples);

      free(self);
    }
  return (Filter*) NULL;
}

/*-------------------------------------------------*/
void            filter_clear          (Filter* self)
{
  int i;
  for(i=0; i<self->order+1; i++)
    self->prev_samples[i] = 0;
}

/*-------------------------------------------------*/
void filter_set_filter_type(Filter* self, filter_type_t type)
{
  if((self->type == type) /*|| (type < 0)*/ || (type >= FILTER_NUMBER_OF_TYPES)) 
    return;
  
  self->type = type;
  filter_set_cutoff(self, self->cutoff);
}

/*-------------------------------------------------*/
filter_type_t filter_get_filter_type(Filter* self)
{
  return self->type;
}

/*-------------------------------------------------*/
void filter_set_sample_rate(Filter* self, float sample_rate)
{
  self->sample_rate = sample_rate;
  filter_set_cutoff(self, self->cutoff);
}

/*-------------------------------------------------*/
float filter_get_sample_rate(Filter* self)
{
  return self->sample_rate;
}

/*-------------------------------------------------*/
void filter_set_cutoff(Filter* self, float cutoff)
{
  self->cutoff = cutoff;
  void (*init_coeffs)(Filter*) = NULL;
  
  switch(self->type)
    {
      case FILTER_LOW_PASS : 
        init_coeffs = filter_init_lowpass_coeffs;
        break;
      case FILTER_HIGH_PASS:
        init_coeffs = filter_init_highpass_coeffs;
        break;
      case FILTER_BAND_PASS:
        init_coeffs = filter_init_bandpass_coeffs;
        break;
      case FILTER_BAND_STOP:
        init_coeffs = filter_init_bandstop_coeffs;
        break;

      default:break;
    }
  if(init_coeffs != NULL)
    init_coeffs(self);
}

/*-------------------------------------------------*/
float filter_get_cutoff(Filter* self)
{
  return self->cutoff;
}

/*-------------------------------------------------*/
void filter_set_order(Filter* self, int order)
{
  if(order > self->max_order)
    order = self->max_order;
  if(order < 0)
    order = 0;

  self->order = order;
  filter_set_window_type(self, self->window_type); //recalculate window and coeffs
}

/*-------------------------------------------------*/
int filter_get_order(Filter* self)
{
  return self->order;
}

/*-------------------------------------------------*/
void filter_set_window_type(Filter* self, filter_window_t window)
{
  if(window == self->window_type) return;

  self->window_type = window;
  void (*init_window)(Filter*) = NULL;
  
  switch(self->window_type)
    {
      case FILTER_WINDOW_RECT : 
        init_window = filter_init_rect_window;
        break;
      case FILTER_WINDOW_HANN:
        init_window = filter_init_hann_window;
        break;
      case FILTER_WINDOW_HAMMING:
        init_window = filter_init_hamming_window;
        break;
      case FILTER_WINDOW_BLACKMANN:
        init_window = filter_init_blackmann_window;
        break;

      default:break;
    }
  if(init_window != NULL)
    init_window(self);

  filter_set_cutoff(self, self->cutoff);  
}

/*-------------------------------------------------*/
filter_window_t filter_get_window_type(Filter* self)
{
  return self->window_type;
}

/*-------------------------------------------------*/
void filter_init_lowpass_coeffs(Filter* self)
{
  int i, n=self->order+1;
  float m_over_2 = self->order / 2.0;
  float f_t = self->cutoff / self->sample_rate;
  float two_pi_f_t = TWO_PI * f_t;  
  float i_minus_m_over_2;
  float temp;

  for(i=0; i<n; i++)
    {
      if(i != m_over_2)
        {
          i_minus_m_over_2 = i - m_over_2;
          temp = sin(two_pi_f_t * i_minus_m_over_2);
          temp /= M_PI * i_minus_m_over_2;
        }
      else
        temp = 2.0 * f_t;

      temp *= self->window[i];
      self->coeffs[i] = temp;
    }  
}

/*-------------------------------------------------*/
void filter_init_highpass_coeffs(Filter* self)
{
  /* to do:                                                */
  /* this is almost identical to filter_init_lowpass_coeffs*/
  /* and could be serviced by the same function            */

  int i, n=self->order+1;
  float m_over_2 = self->order / 2.0;
  float f_t = self->cutoff / self->sample_rate;
  float two_pi_f_t = TWO_PI * f_t;  
  float i_minus_m_over_2;
  float temp;

  for(i=0; i<n; i++)
    {
      if(i != m_over_2)
        {
          i_minus_m_over_2 = i - m_over_2;
          temp = sin(two_pi_f_t * i_minus_m_over_2);
          temp /= M_PI * i_minus_m_over_2;
          temp *= -1;
        }
      else
        temp = 1 - (2.0 * f_t);

      temp *= self->window[i];
      self->coeffs[i] = temp;
    }
}

/*-------------------------------------------------*/
void filter_init_bandpass_coeffs(Filter* self)
{
  int i, n=self->order+1;
  float m_over_2 = self->order / 2.0;
  float f_t = self->cutoff / self->sample_rate;
  float two_pi_f_t = TWO_PI * f_t;
  float i_minus_m_over_2;
  float temp;
  
  for(i=0; i<n; i++)
    {
      if(i != m_over_2)
        {
          i_minus_m_over_2 = i - m_over_2;
          temp = sin(two_pi_f_t * i_minus_m_over_2);
          temp /= M_PI * i_minus_m_over_2;
          temp *= -1;
        }
      else
        temp = 1 - (2.0 * f_t);

      temp *= self->window[i];
      self->coeffs[i] = temp;
    }
}

/*-------------------------------------------------*/
void filter_init_bandstop_coeffs(Filter* self)
{

}

/*-------------------------------------------------*/
void filter_init_rect_window(Filter* self)
{
  int i, n = self->order + 1;
  for(i=0; i<n; i++)
    self->window[i] = 1;
}

/*-------------------------------------------------*/
void filter_init_hann_window(Filter* self)
{
  int i, n = self->order + 1;
  double phase = 0;
  double phase_increment = TWO_PI / (double)self->order;

  for(i=0; i<n; i++)
    {
      self->window[i] = 0.5 * (1-cos(phase));
      phase += phase_increment;
    }
}

/*-------------------------------------------------*/
void filter_init_hamming_window(Filter* self)
{
  int i, n = self->order + 1;
  double phase = 0;
  double phase_increment = TWO_PI / (double)self->order;
  for(i=0; i<n; i++)
    {
      self->window[i] = 0.54 - 0.46 * cos(phase);
      phase += phase_increment;
    }  
}

/*-------------------------------------------------*/
void filter_init_blackmann_window(Filter* self)
{
  int i, n = self->order + 1;
  double phase = 0;
  double phase_increment = TWO_PI / (double)self->order;
  double a = 0;
  double a0 = (1-a)/2.0;
  double a1 = 1/2.0;
  double a2 = a/2.0;
  
  for(i=0; i<n; i++)
    {
      self->window[i] = a0 - a1*cos(phase) + a2*cos(2*phase);
      phase += phase_increment;
    } 
}

/*-------------------------------------------------*/
void    filter_process_data(Filter* self, float* data, int num_samples)
{
  int i, n = self->order + 1;
  float output;

  while(num_samples-- > 0)
    {
      //shift previous samples
      for(i=self->order; i>0; i--)
        self->prev_samples[i] = self->prev_samples[i-1];
      self->prev_samples[0] = *data;

      //calculate filter output
      output = 0;      
      for(i=0; i<n; i++)
        output += self->prev_samples[i] * self->coeffs[i];

      *data++ = output;
    }
}



// --- STFT.c ---


 //calloc


/*--------------------------------------------------------------------*/
struct Opaque_STFT_Struct
{
  int           should_resynthesize;
  int           window_size;
  int           fft_N;
  int           overlap;
  int           hop_size;
  int           sample_counter;
  int           input_index;
  int           output_index;;
  dft_sample_t* window;
  dft_sample_t* running_input;
  dft_sample_t* running_output;
  dft_sample_t* real;
};

/*--------------------------------------------------------------------*/
STFT* stft_new(int window_size /*power of 2 please*/, int overlap /* 1, 2, 4, 8 */, int should_resynthesize)
{
  STFT* self = (STFT*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      self->should_resynthesize  = should_resynthesize;
      self->window_size          = window_size;
      self->fft_N                = (should_resynthesize) ? overlap * window_size : window_size;
      self->overlap              = overlap;
      self->hop_size             = window_size / overlap;
      self->sample_counter       = 0;
      self->input_index          = 0;
      self->output_index         = 0;
      self->window               = (dft_sample_t*)calloc(self->window_size, sizeof(*(self->window)));
      self->running_input        = (dft_sample_t*)calloc(self->window_size, sizeof(*(self->running_input)));
      self->running_output       = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->running_output)));
      self->real                 = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->real)));
      if(self->window           == NULL) return stft_destroy(self);
      if(self->running_input    == NULL) return stft_destroy(self);
      if(self->running_output   == NULL) return stft_destroy(self);
      if(self->real             == NULL) return stft_destroy(self);
  
      //no choice for now
      dft_init_blackman_window(self->window, self->window_size);
  }
  return self;
}

/*--------------------------------------------------------------------*/
STFT* stft_destroy(STFT* self)
{
  if(self != NULL)
    {
      if(self->window != NULL)
        free(self->window);
      if(self->running_input != NULL)
        free(self->running_input);
      if(self->running_output != NULL)
        free(self->running_output);
      if(self->real != NULL)
        free(self->real);
    
      free(self);
    }
  return (STFT*) NULL;
}

/*--------------------------------------------------------------------*/
/* resynthesized samples, if any, returned in real_input */
void stft_process(STFT* self, dft_sample_t* real_input, int len, stft_onprocess_t onprocess, void* onprocess_self)
{
  int i, j;

  for(i=0; i<len; i++)
    {
      self->running_input[self->input_index++] = real_input[i];
      self->input_index %= self->window_size;
    
      if(self->should_resynthesize)
        {
          real_input[i] = self->running_output[self->output_index];
          self->running_output[self->output_index++] = 0;
          self->output_index %= self->fft_N;
        }
    
      if(++self->sample_counter == self->hop_size)
        {
          self->sample_counter = 0;
        
          for(j=0; j<self->window_size; j++)
            {
              self->real[j] = self->running_input[(self->input_index+j) % self->window_size];
            }

          dft_apply_window(self->real, self->window, self->window_size);
          for(j=self->window_size; j<self->fft_N; j++)
            {
              self->real[j] = 0;
            }
          rdft_real_forward_dft(self->real, self->fft_N);
          onprocess           (onprocess_self, self->real, self->fft_N);
          if(self->should_resynthesize)
            {
              rdft_real_inverse_dft(self->real, self->fft_N);
              for(j=0; j<self->fft_N; j++)
                self->running_output[(self->output_index+j) % self->fft_N] += self->real[j];
            }
        }
    }
}

/*--------------------------------------------------------------------*/
int   stft_get_N (STFT* self)
{
  return self->fft_N;
}

/*--------------------------------------------------------------------*/
int     stft_get_overlap (STFT* self)
{
  return self->overlap;
}

/*--------------------------------------------------------------------*/
int     stft_get_hop     (STFT* self)
{
  return self->hop_size;
}

/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
struct Opaque_TWO_STFTS_Struct
{
  int           should_resynthesize;
  int           window_size;
  int           fft_N;
  int           overlap;
  int           hop_size;
  int           sample_counter;
  int           input_index;
  int           output_index;;
  dft_sample_t* window;
  dft_sample_t* running_input;
  dft_sample_t* running_output;
  dft_sample_t* real;
  dft_sample_t* imag;
  dft_sample_t* running_input_2;
  dft_sample_t* running_output_2;
  dft_sample_t* real_2;
  dft_sample_t* imag_2;
};



/*--------------------------------------------------------------------*/
TWO_STFTS* two_stfts_new(int window_size /*power of 2 please*/, int overlap /* 1, 2, 4, 8 */, int should_resynthesize)
{
  TWO_STFTS* self = (TWO_STFTS*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      self->should_resynthesize  = should_resynthesize;
      self->window_size          = window_size;
      self->fft_N                = (should_resynthesize) ? overlap * window_size : window_size;
      self->overlap              = overlap;
      self->hop_size             = window_size / overlap;
      self->sample_counter       = 0;

      self->input_index          = 0;
      self->output_index         = 0;

      self->window               = (dft_sample_t*)calloc(self->window_size, sizeof(*(self->window)));
      self->running_input        = (dft_sample_t*)calloc(self->window_size, sizeof(*(self->running_input)));
      self->running_output       = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->running_output)));
      self->real                 = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->real)));
      self->imag                 = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->imag)));
      self->running_input_2      = (dft_sample_t*)calloc(self->window_size, sizeof(*(self->running_input_2)));
      self->running_output_2     = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->running_output_2)));
      self->real_2               = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->real_2)));
      self->imag_2               = (dft_sample_t*)calloc(self->fft_N      , sizeof(*(self->imag_2)));

      if(self->window           == NULL) return two_stfts_destroy(self);
      if(self->running_input    == NULL) return two_stfts_destroy(self);
      if(self->running_output   == NULL) return two_stfts_destroy(self);
      if(self->real             == NULL) return two_stfts_destroy(self);
      if(self->imag             == NULL) return two_stfts_destroy(self);
      if(self->running_input_2  == NULL) return two_stfts_destroy(self);
      if(self->running_output_2 == NULL) return two_stfts_destroy(self);
      if(self->real_2           == NULL) return two_stfts_destroy(self);
      if(self->imag_2           == NULL) return two_stfts_destroy(self);

      //no choice for now
      dft_init_blackman_window(self->window, self->window_size);
  }
  
  return self;
}

/*--------------------------------------------------------------------*/
TWO_STFTS* two_stfts_destroy(TWO_STFTS* self)
{
  if(self != NULL)
    {
      if(self->window != NULL)
        free(self->window);
      if(self->running_input != NULL)
        free(self->running_input);
      if(self->running_output != NULL)
        free(self->running_output);
      if(self->real != NULL)
        free(self->real);
      if(self->imag != NULL)
        free(self->imag);
      if(self->running_input_2 != NULL)
        free(self->running_input_2);
      if(self->running_output_2 != NULL)
        free(self->running_output_2);
      if(self->real_2 != NULL)
        free(self->real_2);
      if(self->imag_2 != NULL)
        free(self->imag_2);
      free(self);
    }
  return (TWO_STFTS*) NULL;
}

/*--------------------------------------------------------------------*/
void two_stfts_process(TWO_STFTS* self, dft_sample_t* real_input, dft_sample_t* real_input_2, int len, int two_inverses, two_stfts_onprocess_t onprocess, void* onprocess_self)
{
  int i, j;

  for(i=0; i<len; i++)
    {
      self->running_input[self->input_index] = real_input[i];
      self->running_input_2[self->input_index] = real_input_2[i];
      ++self->input_index; self->input_index %= self->window_size;
    
      //?
      if(self->should_resynthesize)
        {
          real_input[i] = self->running_output[self->output_index];
          self->running_output[self->output_index] = 0;
          if(two_inverses)
            {
              real_input_2[i] = self->running_output_2[self->output_index];
              self->running_output_2[self->output_index] = 0;
            }
          ++self->output_index; self->output_index %= self->fft_N;
        }

      ++self->sample_counter;

      //new dft if necessary
      if(self->sample_counter == self->hop_size)
        {
          self->sample_counter = 0;
        
          for(j=0; j<self->window_size; j++)
            {
              self->real[j] = self->running_input[(self->input_index+j) % self->window_size];
              self->imag[j] = 0;
              self->real_2[j] = self->running_input_2[(self->input_index+j) % self->window_size];
              self->imag_2[j] = 0;
            }

          dft_apply_window(self->real, self->window, self->window_size);
          dft_apply_window(self->real_2, self->window, self->window_size);
          for(j=self->window_size; j<self->fft_N; j++)
            {
              self->real[j] = 0;
              self->imag[j] = 0;
              self->real_2[j] = 0;
              self->imag_2[j] = 0;
            }

          dft_2_real_forward_dfts  (self->real, self->real_2, self->imag, self->imag_2, self->fft_N);
          onprocess                  (onprocess_self, self->real, self->real_2, self->imag, self->imag_2, self->fft_N);

          if(self->should_resynthesize)
            {
              if(two_inverses)
                dft_2_real_inverse_dfts(self->real, self->real_2, self->imag, self->imag_2, self->fft_N);
              else
                dft_real_inverse_dft(self->real, self->imag, self->fft_N);
        
              for(j=0; j<self->fft_N; j++)
                {
                  self->running_output[(self->output_index+j) % self->fft_N] += self->real[j];
                  if(two_inverses)
                    self->running_output_2[(self->output_index+j) % self->fft_N] += self->real_2[j];
                }
            }
        }
    }
}



// --- Statistics.c ---
/*-____--_--------_---_-----_---_--------------------------------------- 
  / ___|| |_ __ _| |_(_)___| |_(_) ___ ___   ___ 
  \___ \| __/ _` | __| / __| __| |/ __/ __| / __|
   ___) | || (_| | |_| \__ \ |_| | (__\__ \| (__ 
  |____/ \__\__,_|\__|_|___/\__|_|\___|___(_)___|
------------------------------------------------------------------------

----------------------------------------------------------------------*/



 //memset, memcpy


/*--------------------------------------------------------------------*/
/*--___-------_-_-------------------------------------------------------
   / _ \ _ _ | (_)_ _  ___    /_\__ _____ _ _ __ _ __ _ ___ 
  | (_) | ' \| | | ' \/ -_)  / _ \ V / -_) '_/ _` / _` / -_)
   \___/|_||_|_|_|_||_\___| /_/ \_\_/\___|_| \__,_\__, \___|
--------------------------------------------------|___/-----------------
  Calculate the mean and variance of a signal one sample at a time.
----------------------------------------------------------------------*/
struct opaque_online_average_struct
{
  unsigned     n;
  double       mean;
  double       variance;
  double       old_mean;
  double       s;
  double       old_s;
};

/*--------------------------------------------------------------------*/
OnlineAverage* online_average_new()
{
  OnlineAverage* self = (OnlineAverage*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      online_average_init(self);
    }
  return self;
}

/*--------------------------------------------------------------------*/
void online_average_init(OnlineAverage* self)
{
  self->n          = 0;
  self->mean       = 0;
  self->variance   = 0;
  self->old_mean   = 0;
  self->s          = 0;
  self->old_s      = 0;
}

/*--------------------------------------------------------------------*/
OnlineAverage* online_average_destroy(OnlineAverage* self)
{
  if(self != NULL)
    {
      free(self);
    }
  return (OnlineAverage*) NULL;
}

/*--------------------------------------------------------------------*/
int online_average_n(OnlineAverage* self)
{
  return self->n;
}

/*--------------------------------------------------------------------*/
double     online_average_mean(OnlineAverage* self)
{
  return self->mean;
}

/*--------------------------------------------------------------------*/
double     online_average_variance(OnlineAverage* self)
{
  return self->variance;
}

/*--------------------------------------------------------------------*/
double     online_average_std_dev(OnlineAverage* self)
{
  return sqrt(self->variance);
}

/*--------------------------------------------------------------------*/
void online_average_update(OnlineAverage* self, double     x)
{
  ++self->n;
  
  if (self->n == 1)
    {
      self->old_mean = self->mean = x;
      self->old_s    = 0.0;
    }
  else
    {
      self->mean     = self->old_mean + (x - self->old_mean) / self->n;
      self->s        = self->old_s + ((x - self->old_mean) * (x - self->mean));
      self->old_mean = self->mean; 
      self->old_s    = self->s;
      self->variance = self->s / (self->n - 1);
    }
}

/*--------------------------------------------------------------------*/
/*-__--__---------_---------------_-------------------------------------
  |  \/  |_____ _(_)_ _  __ _    /_\__ _____ _ _ __ _ __ _ ___ 
  | |\/| / _ \ V / | ' \/ _` |  / _ \ V / -_) '_/ _` / _` / -_)
  |_|  |_\___/\_/|_|_||_\__, | /_/ \_\_/\___|_| \__,_\__, \___|
------------------------|___/------------------------|___/--------------
  Calculate the moving or rolling mean and variance over the past N
  samples of a signal. This algorithm is online and updates one sample
  at a time.
----------------------------------------------------------------------*/
struct opaque_moving_average_struct
{
  unsigned       N;  // the nominal window size
  double         mean;
  double         variance;
  double*        past_samples;
  unsigned       current_sample_index;
  double         s;
  OnlineAverage* online;  //used until there are at least self->N samples
};

/*--------------------------------------------------------------------*/
MovingAverage* moving_average_new(unsigned N)
{
  MovingAverage* self = (MovingAverage*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      self->N = N;
      self->past_samples = (double*)calloc(self->N, sizeof(*self->past_samples));
      if(self->past_samples == NULL)
        return moving_average_destroy(self);
      
      self->online = online_average_new();
      if(self->online == NULL)
        return moving_average_destroy(self);
      
      moving_average_init(self);
    }
  return self;
}

/*--------------------------------------------------------------------*/
void moving_average_init(MovingAverage* self)
{
  self->mean                 = 0;
  self->variance             = 0;
  self->current_sample_index = 0;
  self->s                    = 0;
  online_average_init(self->online);  
}

/*--------------------------------------------------------------------*/
MovingAverage* moving_average_destroy(MovingAverage* self)
{
  if(self != NULL)
    {
      if(self->past_samples != NULL)
        free(self->past_samples);
        
      self->online = online_average_destroy(self->online);
      
      free(self);
    }
  return (MovingAverage*) NULL;
}

/*--------------------------------------------------------------------*/
int moving_average_N(MovingAverage* self)
{
  return self->N;
}

/*--------------------------------------------------------------------*/
int moving_average_n(MovingAverage* self)
{
  return online_average_n(self->online);
}

/*--------------------------------------------------------------------*/
double     moving_average_mean(MovingAverage* self)
{
  return self->mean;
}

/*--------------------------------------------------------------------*/
double     moving_average_variance(MovingAverage* self)
{
  return self->variance;
}

/*--------------------------------------------------------------------*/
double     moving_average_std_dev(MovingAverage* self)
{
  return sqrt(self->variance);
}
/*--------------------------------------------------------------------*/
void moving_average_update(MovingAverage* self, double     x)
{
  double     new_mean;
  double     oldest_sample = self->past_samples[self->current_sample_index];
  
  self->past_samples[self->current_sample_index] = x;
  ++self->current_sample_index;
  self->current_sample_index %= self->N;

  if(online_average_n(self->online) < self->N)
    {
      online_average_update(self->online, x);
      self->variance = online_average_variance(self->online);
      self->mean     = online_average_mean(self->online);
      self->s        = self->variance * self->N;
    }
  else
    {
      new_mean       = self->mean + (x - oldest_sample) / self->N;
      self->s       += (x + oldest_sample - self->mean - new_mean) * (x - oldest_sample);
      self->variance = self->s / self->N;
      self->mean     = new_mean;
    }
};

/*--------------------------------------------------------------------*/
/*--___-------_-_------------___------------------------_---------------
   / _ \ _ _ | (_)_ _  ___  | _ \___ __ _ _ _ ___ _____(_)___ _ _  
  | (_) | ' \| | | ' \/ -_) |   / -_) _` | '_/ -_|_-<_-< / _ \ ' \ 
   \___/|_||_|_|_|_||_\___| |_|_\___\__, |_| \___/__/__/_\___/_||_|
------------------------------------|___/-------------------------------
  Calculate the means, variances, covariance, slope, y-intercept and 
  correlation coeficient of two signals. This is an online algorithm
  that operates one sample at a time.
----------------------------------------------------------------------*/
struct opaque_online_regression_struct
{
  unsigned     n         ;
  double       a_mean    ;
  double       b_mean    ;
  double       a_variance;
  double       b_variance;
  double       covariance;
  
  //slope, y-intercept, correlation
  double       m         ;
  double       b         ;
  double       r_2       ;
};

/*--------------------------------------------------------------------*/
OnlineRegression* online_regression_new()
{
  OnlineRegression* self = (OnlineRegression*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      online_regression_init(self);
    }
  return self;
}

/*--------------------------------------------------------------------*/
void online_regression_init(OnlineRegression* self)
{
  self->n          = 0;
  self->a_mean     = 0;
  self->b_mean     = 0;
  self->a_variance = 0;
  self->b_variance = 0;
  self->covariance = 0;
  self->m          = 1;
  self->b          = 0;
  self->r_2        = 0;
}

/*--------------------------------------------------------------------*/
OnlineRegression* online_regression_destroy(OnlineRegression* self)
{
  if(self != NULL)
    {
      free(self);
    }
  return (OnlineRegression*) NULL;
}

/*--------------------------------------------------------------------*/
int online_regression_n(OnlineRegression* self)
{
  return self->n;
}

/*--------------------------------------------------------------------*/
double     online_regression_covariance(OnlineRegression* self)
{
  return self->covariance;
}

/*--------------------------------------------------------------------*/
double     online_regression_slope(OnlineRegression* self)
{
  return self->m;
}

/*--------------------------------------------------------------------*/
double     online_regression_y_intercept(OnlineRegression* self)
{
  return self->b;
}

/*--------------------------------------------------------------------*/
double     online_regression_r_squared(OnlineRegression* self)
{
  return self->r_2;
}

/*--------------------------------------------------------------------*/
void online_regression_update(OnlineRegression* self, double     a_data /*x*/, double     b_data /*y*/)
{
  ++self->n;
  double     one_over_n = 1.0 / (double)self->n;
  
  double     n_minus_1_over_n = (self->n-1) * one_over_n;
  double     d_a = a_data - self->a_mean;
  double     d_b = b_data - self->b_mean;
  
  double     new_m, new_b;
  self->a_variance += (n_minus_1_over_n * d_a * d_a - self->a_variance) * one_over_n;
  self->b_variance += (n_minus_1_over_n * d_b * d_b - self->b_variance) * one_over_n;
  self->covariance += (n_minus_1_over_n * d_a * d_b - self->covariance) * one_over_n;
  
  self->a_mean += d_a * one_over_n;
  self->b_mean += d_b * one_over_n;
  
  if((self->a_variance == 0) || (self->covariance == 0))
    return;
  
  new_m = self->covariance / self->a_variance;
  new_b = self->b_mean - (new_m * self->a_mean);
  self->m = new_m;
  self->b = new_b;
  self->r_2 = (self->covariance * self->covariance) / (self->a_variance * self->b_variance);
}

/*--------------------------------------------------------------------*/
/*----------_-----------_---_-----------_____-_---------------_----_----    _    _ 
    /_\  __| |__ _ _ __| |_(_)_ _____  |_   _| |_  _ _ ___ __| |_ | |_  ___| |__| |
   / _ \/ _` / _` | '_ \  _| \ V / -_)   | | | ' \| '_/ -_|_-< ' \| ' \/ _ \ / _` |
  /_/ \_\__,_\__,_| .__/\__|_|\_/\___|   |_| |_||_|_| \___/__/_||_|_||_\___/_\__,_|
------------------|_|---------------------------------------------------
  Adaptive threshhold tells you when your signal goes a specified number
  of standard deviations above or below a moving average of a filtered
  version of your signal. update() returns +1 or -1 whenever your signal
  transitions to being above or below the threshold.
----------------------------------------------------------------------*/
struct opaque_adaptive_threshold_struct
{
  double         smoothing; //coefficient 0 ~ 1
  double         threshold; //standard deviations from filtered signal
  double         min;
  // step function (-1, 0, 1) that will hold its value while input is 
  // above threshold. update() only returns +1 or -1 at the moment
  // onset_signal transitions to +1 or -1;
  double         onset_signal;
  
  //(start with high variance to avoid spurious onsets on first iterations)
  MovingAverage* avg;
  double         filtered_x;
};

/*--------------------------------------------------------------------*/
AdaptiveThreshold* adaptive_threshold_new(unsigned N)
{
  AdaptiveThreshold* self = (AdaptiveThreshold*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      self->avg = moving_average_new(N);
      if(self->avg == NULL)
        return adaptive_threshold_destroy(self);

      adaptive_threshold_init(self);
    }
  return self;
}

/*--------------------------------------------------------------------*/
void adaptive_threshold_init(AdaptiveThreshold* self)
{
  self->smoothing  = 0.5; //coefficient 0 ~ 1
  self->threshold  = 3.5; //standard deviations from filtered signal
  self->min        = 0;
  adaptive_threshold_clear(self);
}

/*--------------------------------------------------------------------*/
void adaptive_threshold_clear(AdaptiveThreshold* self)
{
  self->onset_signal = 0;
  moving_average_init(self->avg);
  self->filtered_x   = 0;
}

/*--------------------------------------------------------------------*/
AdaptiveThreshold* adaptive_threshold_destroy(AdaptiveThreshold* self)
{
  if(self != NULL)
    {
      self->avg = moving_average_destroy(self->avg);
      free(self);
    }
  return (AdaptiveThreshold*) NULL;
}

/*--------------------------------------------------------------------*/
double     adaptive_threshold_smoothing(AdaptiveThreshold* self)
{
  return self->smoothing;
}

/*--------------------------------------------------------------------*/
void adaptive_threshold_set_smoothing(AdaptiveThreshold* self, double     coefficient)
{
  coefficient = (coefficient > 1) ? 1 : ((coefficient < 0) ? 0 : coefficient);
  self->smoothing = coefficient;
}

/*--------------------------------------------------------------------*/
double     adaptive_threshold_threshold(AdaptiveThreshold* self)
{
  return self->threshold;
}

/*--------------------------------------------------------------------*/
double     adaptive_threshold_threshold_value(AdaptiveThreshold* self)
{
  double result = moving_average_std_dev(self->avg) * self->threshold;
  if(result < self->min)
    result = self->min;
  return result + moving_average_mean(self->avg);
}

/*--------------------------------------------------------------------*/
void adaptive_threshold_set_threshold(AdaptiveThreshold* self, double     std_devs)
{
  //if(std_devs < 0) std_devs = 0;
  self->threshold = std_devs;
}

/*--------------------------------------------------------------------*/
double             adaptive_threshold_threshold_min    (AdaptiveThreshold* self)
{
  return self->min;
}
/*--------------------------------------------------------------------*/
void               adaptive_threshold_set_threshold_min(AdaptiveThreshold* self, double min)
{
  //if(min < 0) min = 0;
  self->min = min;
}

/*--------------------------------------------------------------------*/
double     adaptive_threshold_onset_signal(AdaptiveThreshold* self)
{
  return self->onset_signal;
}

/*--------------------------------------------------------------------*/
double             adaptive_threshold_mean          (AdaptiveThreshold* self)
{
  return moving_average_mean(self->avg);
}

/*--------------------------------------------------------------------*/
double     adaptive_threshold_update(AdaptiveThreshold* self, double     x)
{
  double     next   = 0;
  double     result = 0;

  self->filtered_x = (x * (1-self->smoothing)) + (self->filtered_x * self->smoothing);
  moving_average_update(self->avg, self->filtered_x);

  double thresh = moving_average_std_dev(self->avg) * self->threshold;
  if(thresh < self->min) thresh = self->min;

  if((fabs(x) - moving_average_mean(self->avg)) > thresh)
    next = (x > moving_average_mean(self->avg)) ? 1 : -1;
  
  //ignore the first few samples so we get a stable std dev
  if(moving_average_n(self->avg) > 10)
    if((next != 0) && (self->onset_signal != next))
      result = next;

  self->onset_signal = next;
  
  return result;
};


/*--------------------------------------------------------------------*/
double     statistics_random_flat()
{
  return random() / (double)RAND_MAX;
}

/*--------------------------------------------------------------------*/
double     statistics_random_normal(double mean, double std_dev)
{
  double     u, v, r;
  
  do{
    u = 2 * statistics_random_flat() - 1;
    v = 2 * statistics_random_flat() - 1;
    r = u*u + v*v;
  }while(r == 0 || r > 1);

  double     c = sqrt(-2 * log(r) / r);
  return mean + u * c * std_dev;
}

/*--------------------------------------------------------------------*/
double     statistics_random_cauchy(double peak_location, double half_width_at_half_maximum)
{
  double result = statistics_random_flat();
  result -= 0.5;
  result *= M_PI;
  //if(fabs(result) == M_PI)
    //result = 0;
  //else
    result = tan(result);
  
  result *= half_width_at_half_maximum;
  result += peak_location;
  return result;
}



// --- BTT.c ---
/*----------------------------------------------------------------------
 ____             _                     _
| __ )  ___  __ _| |_    __ _ _ __   __| |
|  _ \ / _ \/ _` | __|  / _` | '_ \ / _` |
| |_) |  __/ (_| | |_  | (_| | | | | (_| |
|____/ \___|\__,_|\__|  \__,_|_| |_|\__,_|
 
 _____
|_   _|__ _ __ ___  _ __   ___
  | |/ _ \ '_ ` _ \| '_ \ / _ \
  | |  __/ | | | | | |_) | (_) |
  |_|\___|_| |_| |_| .__/ \___/
                   |_|
 _____               _    _
|_   _| __ __ _  ___| | _(_)_ __   __ _
  | || '__/ _` |/ __| |/ / | '_ \ / _` |
  | || | | (_| | (__|   <| | | | | (_| |
  |_||_|  \__,_|\___|_|\_\_|_| |_|\__, |
                                  |___/
------------------------------------------------------------------------

  Made by Michael Krzyzaniak
 
  Version:
    1.0

----------------------------------------------------------------------*/





 //calloc
 //memset

 //testing only


#define BPM_TO_LAG(bpm) (60 * self->oss_sample_rate / ((float)(bpm)))
#define LAG_TO_BPM(lag) (60 * self->oss_sample_rate / ((float)(lag)))
#define MODULO(x, N)    (((x) % (N) + (N)) % (N))

void btt_spectral_flux_stft_callback (void*   SELF, dft_sample_t* real, int N);
void btt_onset_tracking              (BTT* self, dft_sample_t* real, int N);

void btt_tempo_tracking              (BTT* self);
void btt_beat_tracking               (BTT* self);

/*--------------------------------------------------------------------*/
struct Opaque_BTT_Struct
{
  STFT*              spectral_flux_stft;
  dft_sample_t*      prev_spectrum_magnitude;
  int                should_normalize_amplitude;
  double             spectral_compression_gamma;
  double             noise_cancellation_threshold;
  AdaptiveThreshold* onset_threshold;
  double             sample_rate;
  double             oss_sample_rate;
  Filter*            oss_filter;
  dft_sample_t*      oss;
  int                oss_index;
  int                oss_length;

  int                max_lag;
  int                min_lag;
  int                num_tempo_candidates;
  dft_sample_t*      autocorrelation_real;
  
  double             autocorrelation_exponent;
  OnlineAverage*     tempo_score_variance;
  double             log_gaussian_tempo_weight_width;
  double             log_gaussian_tempo_weight_mean;
  dft_sample_t*      gaussian_tempo_histogram;
  double             gaussian_tempo_histogram_decay;
  double             one_minus_gaussian_tempo_histogram_decay;
  double             gaussian_tempo_histogram_width;

  unsigned long long num_audio_samples_processed;
  unsigned long long num_oss_frames_processed;
  int                beat_period_oss_samples;
  
  float*             cbss;
  int                cbss_length;
  int                cbss_index;
  double             cbss_alpha;
  double             one_minus_cbss_alpha;
  double             cbss_alpha_before_lock;
  double             cbss_eta;
  float*             predicted_beat_signal;
  int                predicted_beat_index;
  int                beat_prediction_adjustment;
  int                predicted_beat_trigger_index;
  int                predicted_beat_gaussian_width;
  unsigned long long ignore_beats_until;
  double             ignore_spurious_beats_duration;
  
  int                count_in_n;
  OnlineAverage*     count_in_average;
  int                count_in_count;
  unsigned long long last_count_in_time;
  
  int                analysis_latency_onset_adjustment; /* in audio samples */
  int                analysis_latency_beat_adjustment_coarse; /* in oss samples */
  int                analysis_latency_beat_adjustment_fine; /* in audio samples */
  
  unsigned long long metronome_clock;
  unsigned long long metronome_lag;
  
  btt_tracking_mode_t   tracking_mode;
  btt_tracking_mode_t   tracking_mode_before_count_in;
  btt_onset_callback_t  onset_callback;
  btt_beat_callback_t   beat_callback;
  void* onset_callback_self;
  void* tempo_callback_self;
  void* beat_callback_self;
};

/*--------------------------------------------------------------------*/
BTT*   btt_new_default                      ()
{
  return btt_new(BTT_SUGGESTED_SPECTRAL_FLUX_STFT_LEN,
                    BTT_SUGGESTED_SPECTRAL_FLUX_STFT_OVERLAP,
                    BTT_SUGGESTED_OSS_FILTER_ORDER,
                    BTT_SUGGESTED_OSS_LENGTH,
                    BTT_SUGGESTED_ONSET_THRESHOLD_N,
                    BTT_SUGGESTED_CBSS_LENGTH,
                    BTT_SUGGESTED_SAMPLE_RATE,
                    BTT_DEFAULT_ANALYSIS_LATENCY_ONSET_ADJUSTMENT,
                    BTT_DEFAULT_ANALYSIS_LATENCY_BEAT_ADJUSTMENT
                    );
}

/*--------------------------------------------------------------------*/
BTT* btt_new(int spectral_flux_stft_len, int spectral_flux_stft_overlap, int oss_filter_order, int oss_length, int onset_threshold_len, int cbss_length, double sample_rate, int analysis_latency_onset_adjustment, int analysis_latency_beat_adjustment)
{
  BTT* self = (BTT*)calloc(1, sizeof(*self));
  if(self != NULL)
    {
      self->spectral_flux_stft = stft_new(spectral_flux_stft_len, spectral_flux_stft_overlap, 0);
      if(self->spectral_flux_stft == NULL) return btt_destroy(self);
    
      self->prev_spectrum_magnitude = (dft_sample_t*)calloc(spectral_flux_stft_len, sizeof(*self->prev_spectrum_magnitude));
      if(self->prev_spectrum_magnitude == NULL) return btt_destroy(self);
    
      self->onset_threshold = adaptive_threshold_new(onset_threshold_len);
      if(self->onset_threshold == NULL) return btt_destroy(self);
      adaptive_threshold_set_smoothing(self->onset_threshold, 0);
    
      self->sample_rate = sample_rate;
      self->oss_sample_rate = sample_rate / stft_get_hop(self->spectral_flux_stft);
      self->oss_filter = filter_new(FILTER_LOW_PASS, BTT_DEFAULT_OSS_FILTER_CUTOFF, oss_filter_order);
      if(self->oss_filter == NULL) return btt_destroy(self);
      filter_set_sample_rate(self->oss_filter, self->oss_sample_rate);
      filter_set_window_type(self->oss_filter, FILTER_WINDOW_HAMMING);
    
      self->oss_length = oss_length;
      self->oss = (float*)calloc(self->oss_length, sizeof(*self->oss));
      if(self->oss == NULL) return btt_destroy(self);
      self->oss_index = 0;

      self->autocorrelation_real = (dft_sample_t*)calloc(2*self->oss_length, sizeof(*self->autocorrelation_real));
      if(self->autocorrelation_real == NULL) return btt_destroy(self);
      
      self->gaussian_tempo_histogram = (float*)calloc(self->oss_length, sizeof(*self->gaussian_tempo_histogram));
      if(self->gaussian_tempo_histogram == NULL) return btt_destroy(self);
    
      self->cbss_length = cbss_length;
      self->cbss = (float*)calloc(self->cbss_length, sizeof(*self->cbss));
      if(self->cbss == NULL) return btt_destroy(self);

      self->predicted_beat_signal = (float*)calloc(self->cbss_length, sizeof(*self->predicted_beat_signal));
      if(self->predicted_beat_signal == NULL) return btt_destroy(self);
      self->tempo_score_variance = online_average_new();
      if(self->tempo_score_variance == NULL) return btt_destroy(self);

      self->count_in_average = online_average_new();
      if(self->count_in_average == NULL) return btt_destroy(self);

      btt_set_analysis_latency_onset_adjustment(self, analysis_latency_onset_adjustment);
      btt_set_analysis_latency_beat_adjustment (self, analysis_latency_beat_adjustment );
    
      btt_init(self);
      
      btt_set_metronome_bpm (self, BTT_DEFAULT_LOG_GAUSSIAN_TEMPO_WEIGHT_MEAN);
    }
  return self;
}

/*--------------------------------------------------------------------*/
BTT* btt_destroy(BTT* self)
{
  if(self != NULL)
    {
      if(self->prev_spectrum_magnitude != NULL)
        free(self->prev_spectrum_magnitude);

      if(self->oss != NULL)
        free(self->oss);
    
      if(self->autocorrelation_real != NULL)
        free(self->autocorrelation_real);
    
      if(self->gaussian_tempo_histogram != NULL)
        free(self->gaussian_tempo_histogram);
    
      if(self->cbss != NULL)
        free(self->cbss);

      if(self->predicted_beat_signal != NULL)
        free(self->predicted_beat_signal);
    
      stft_destroy(self->spectral_flux_stft);
      filter_destroy(self->oss_filter);
      adaptive_threshold_destroy(self->onset_threshold);
      online_average_destroy(self->tempo_score_variance);
      online_average_destroy(self->count_in_average);
    
      free(self);
    }
  return (BTT*) NULL;
}

/*--------------------------------------------------------------------*/
void      btt_init(BTT* self)
{
  btt_set_min_tempo                       (self, BTT_DEFAULT_MIN_TEMPO);
  btt_set_max_tempo                       (self, BTT_DEFAULT_MAX_TEMPO);
  btt_set_spectral_compression_gamma      (self, BTT_DEFAULT_SPECTRAL_COMPRESSION_GAMMA);
  btt_set_autocorrelation_exponent        (self, BTT_DEFAULT_AUTOCORRELATION_EXPONENT);
  btt_set_num_tempo_candidates            (self, BTT_DEFAULT_NUM_TEMPO_CANDIDATES);
  btt_set_tracking_mode                   (self, BTT_DEFAULT_TRACKING_MODE);
  btt_set_oss_filter_cutoff               (self, BTT_DEFAULT_OSS_FILTER_CUTOFF);
  btt_set_use_amplitude_normalization     (self, BTT_DEFAULT_USE_AMP_NORMALIZATION);
  btt_set_onset_threshold                 (self, BTT_DEFAULT_ONSET_TREHSHOLD);
  btt_set_onset_threshold_min             (self, BTT_DEFAULT_ONSET_TREHSHOLD_MIN);
  btt_set_noise_cancellation_threshold    (self, BTT_DEFAULT_NOISE_CANCELLATION_THRESHOLD);
  btt_set_cbss_alpha                      (self, BTT_DEFAULT_CBSS_ALPHA);
  btt_set_cbss_eta                        (self, BTT_DEFAULT_CBSS_ETA);
  btt_set_log_gaussian_tempo_weight_mean  (self, BTT_DEFAULT_LOG_GAUSSIAN_TEMPO_WEIGHT_MEAN);
  btt_set_log_gaussian_tempo_weight_width (self, BTT_DEFAULT_LOG_GAUSSIAN_TEMPO_WEIGHT_WIDTH);
  btt_set_gaussian_tempo_histogram_decay  (self, BTT_DEFAULT_GAUSSIAN_TEMPO_HISTOGRAM_DECAY);
  btt_set_gaussian_tempo_histogram_width  (self, BTT_DEFAULT_GAUSSIAN_TEMPO_HISTOGRAM_WIDTH);
  btt_set_beat_prediction_adjustment      (self, BTT_DEFAULT_BEAT_PREDICTION_ADJUSTMENT);
  btt_set_predicted_beat_trigger_index    (self, BTT_DEFAULT_PREDICTED_BEAT_TRIGGER_INDEX);
  btt_set_predicted_beat_gaussian_width   (self, BTT_DEFAULT_PREDICTED_BEAT_GAUSSIAN_WIDTH);
  btt_set_ignore_spurious_beats_duration  (self, BTT_DEFAULT_IGNORE_SPURIOUS_BEATS_DURATION);
  btt_set_count_in_n                      (self, BTT_DEFAULT_COUNT_IN_N);
  btt_set_onset_tracking_callback         (self, NULL, NULL);
  btt_set_beat_tracking_callback          (self, NULL, NULL);
  
  btt_clear(self);
}

/*--------------------------------------------------------------------*/
void      btt_clear(BTT* self)
{
  self->num_audio_samples_processed = 0;
  self->num_oss_frames_processed    = 0;
  self->oss_index                   = 0;
  self->count_in_count              = 0;
  online_average_init (self->count_in_average);
  memset(self->oss, 0, self->oss_length * sizeof(*self->oss));
  btt_init_tempo(self, 0);
}

/*--------------------------------------------------------------------*/
void      btt_init_tempo(BTT* self, double bpm /*0 to clear tempo*/)
{
  memset(self->gaussian_tempo_histogram, 0, self->oss_length * sizeof(*self->gaussian_tempo_histogram));
  memset(self->cbss, 0, self->cbss_length * sizeof(*self->cbss));
  memset(self->predicted_beat_signal, 0, self->cbss_length * sizeof(*self->predicted_beat_signal));
  
  self->beat_period_oss_samples = 0;
  self->cbss_index              = 0;
  
  if(bpm > 0)
    {
      float max_bpm = btt_get_max_tempo(self);
      float min_bpm = btt_get_min_tempo(self);
      while (bpm > max_bpm) bpm *= 0.5;
      while (bpm < min_bpm) bpm *= 2;
      int lag = round(BPM_TO_LAG(bpm));
    
      if((lag >= 0) && (lag < self->oss_length))
        {
         //normalize height later
          self->gaussian_tempo_histogram[lag] = 1;
          self->beat_period_oss_samples = lag;
        
          int i;
          for(i=self->cbss_length-1; i>=0; i-=lag)
          //for(i=2; i<self->cbss_length; i+=lag)
            self->cbss[(self->cbss_length + i) % self->cbss_length] = 15;
        }
    
      //btt_set_metronome_bpm(self, bpm);
      self->metronome_clock = 0;
    }
}
/*--------------------------------------------------------------------*/
int       btt_get_beat_period_audio_samples(BTT* self)
{
  if(self->tracking_mode == BTT_METRONOME_MODE)
    return self->metronome_lag * stft_get_hop(self->spectral_flux_stft);
  else
    return self->beat_period_oss_samples * stft_get_hop(self->spectral_flux_stft);
}

/*--------------------------------------------------------------------*/
double    btt_get_tempo_bpm(BTT* self)
{
  if(self->beat_period_oss_samples <=0)
    return 0;
  else if(self->tracking_mode == BTT_METRONOME_MODE)
    return LAG_TO_BPM(self->metronome_lag);
  else
    return LAG_TO_BPM(self->beat_period_oss_samples);
}

/*--------------------------------------------------------------------*/
void      btt_set_metronome_bpm                  (BTT* self, double bpm)
{
  float max_bpm = btt_get_max_tempo(self);
  float min_bpm = btt_get_min_tempo(self);
  if(bpm > max_bpm) bpm = max_bpm;
  if(bpm < min_bpm) bpm = min_bpm;
  
  self->metronome_lag = round(BPM_TO_LAG(bpm));
}

/*--------------------------------------------------------------------*/
double    btt_get_tempo_certainty(BTT* self)
{
  return self->gaussian_tempo_histogram[self->beat_period_oss_samples];
}

/*--------------------------------------------------------------------*/
void btt_onset_tracking              (BTT* self, dft_sample_t* real, int N)
{
  int i;
  int n_over_2  = N >> 1;
  float flux = 0;
  
  //get spectrum magnitude, ignore DCC component in future calculations
  rdft_rect_to_polar(real, N);
  
  real[0] = 0;
  
  //perform log compression
  if(self->spectral_compression_gamma > 0)
    {
      double denominator = 1 / log(1 + self->spectral_compression_gamma);
      for(i=1; i<n_over_2; i++)
        real[i] = denominator * log(1+self->spectral_compression_gamma*real[i]);
    }
  
  //bins less than -74 dB are noise-cancelled
  for(i=1; i<n_over_2; i++)
    if(real[i] < self->noise_cancellation_threshold)
      real[i] = 0;
  
  //Normalize again
  if(self->should_normalize_amplitude)
    dft_normalize_magnitude(real, n_over_2);
  
  //Calculate flux and save spectrum
  for(i=1; i<n_over_2; i++)
    {
      flux += (real[i] > self->prev_spectrum_magnitude[i]) ? real[i] - self->prev_spectrum_magnitude[i] : 0;
      self->prev_spectrum_magnitude[i] = real[i];
    }
  
  //10HZ low-pass filter flux to obtaion OSS, delays oss by (filter_order-1) / 2 oss samples
  filter_process_data(self->oss_filter, &flux, 1);
  self->oss[self->oss_index] = flux;
  ++self->oss_index; self->oss_index %= self->oss_length;

  //call onset detected callback; technically the threshold filter is storing the oss signal again, using ~8kb redundant space
  if(flux > 0)
    if(adaptive_threshold_update(self->onset_threshold, flux) == 1)
      {
        if(self->onset_callback != NULL)
          {
            unsigned long long t = self->num_audio_samples_processed;
            t -= self->analysis_latency_onset_adjustment;
            self->onset_callback(self->onset_callback_self, t);
          }
      
        if(self->tracking_mode == BTT_COUNT_IN_TRACKING)
          {
            ++self->count_in_count;
            if(self->count_in_count > 1)
              {
                int delta = self->num_oss_frames_processed - self->last_count_in_time;
                //too much time in between counts, so start over
                if((delta > self->max_lag) || (delta < self->min_lag))
                  {
                    online_average_init (self->count_in_average);
                    self->count_in_count = 1;
                  }
                else
                  online_average_update(self->count_in_average, self->num_oss_frames_processed - self->last_count_in_time);
              }
            if(self->count_in_count >= self->count_in_n)
              {
                btt_init_tempo(self, LAG_TO_BPM(online_average_mean(self->count_in_average)));
                btt_set_tracking_mode(self, self->tracking_mode_before_count_in);
              }
            self->last_count_in_time = self->num_oss_frames_processed;
          }
      }
}

/*--------------------------------------------------------------------*/
void btt_tempo_tracking              (BTT* self)
{
  int i, j;
  
  //perform autocorrelation
  memset(self->autocorrelation_real, 0, 2 * self->oss_length * sizeof(*self->autocorrelation_real));
  
  for(i=0; i<self->oss_length; i++)
    self->autocorrelation_real[i] = self->oss[(self->oss_index+i) % self->oss_length];

  rdft_real_generalized_autocorrelation  (self->autocorrelation_real, 2*self->oss_length, self->autocorrelation_exponent);
  
  
  
  
  
  
  /*
  fprintf(stderr, "----------\r\n");
  for(i=0; i<self->oss_length; i++)
    fprintf(stderr, "%f\r\n", self->autocorrelation_real[i]);
  exit(-1);
  */
  
  
  
  
  
  
  //the oss signal only has silence
  if(self->autocorrelation_real[0] == 0)
    return;

  //enhance autocorreltaion
  for(i=self->min_lag; i<self->max_lag; i++)
    {
      int two_i = i<<1;
      int four_i = two_i<<1;
    
      if(two_i < self->oss_length)
        self->autocorrelation_real[i] += self->autocorrelation_real[two_i];
      if(four_i < self->oss_length)
        self->autocorrelation_real[i] += self->autocorrelation_real[four_i];
    }
  
  //pick peaks from autocorrelation
  float old_derivative = 1, new_derivative;
  int candidate_tempo_lags[BTT_MAX_TEMPO_CANDIDATES];
  for(i=0; i<self->num_tempo_candidates; candidate_tempo_lags[i++]=self->min_lag);
  int min_lag_index = 0;
  for(i=self->min_lag; i<self->max_lag-1; i++)
    {
      new_derivative = self->autocorrelation_real[i+1] - self->autocorrelation_real[i];
      if((old_derivative > 0) && (new_derivative < 0))
        {
          if(self->autocorrelation_real[i] > self->autocorrelation_real[candidate_tempo_lags[min_lag_index]])
            {
              candidate_tempo_lags[min_lag_index] = i;
              min_lag_index = 0;
              for(j=1; j<self->num_tempo_candidates; j++)
                if(self->autocorrelation_real[candidate_tempo_lags[j]] < self->autocorrelation_real[candidate_tempo_lags[min_lag_index]])
                  min_lag_index = j;
            }
        }
      old_derivative = new_derivative;
    }

  //cross correlate with pulses and get score components
  float score_max     [BTT_MAX_TEMPO_CANDIDATES];
  float score_variance[BTT_MAX_TEMPO_CANDIDATES];
  float sum_of_score_max      = 0;
  float sum_of_score_variance = 0;
  int   num_pulses            = BTT_DEFAULT_XCORR_NUM_PULSES;
  float pulse_locations[]     = BTT_DEFAULT_XCORR_PULSE_LOCATIONS;
  float pulse_values[]        = BTT_DEFAULT_XCORR_PULSE_VALUES;
  
  //7% realtime spent in this loop (dft-free cross correlation)
  for(i=0; i<self->num_tempo_candidates; i++)
    {
      int phi;
      score_max[i] = 0;
      online_average_init(self->tempo_score_variance);
      for(phi=0; phi<candidate_tempo_lags[i]; phi++)
        {
          int pulse;
          float x_corr = 0;
          for(pulse=0; pulse<num_pulses; pulse++)
            {
              int index = phi + pulse_locations[pulse] * candidate_tempo_lags[i];
              if(index < self->oss_length)
                x_corr += self->oss[(index + self->oss_index) % self->oss_length] * pulse_values[pulse];
            }
          online_average_update(self->tempo_score_variance, x_corr);
          if(x_corr > score_max[i])
            score_max[i] = x_corr;
        }
      score_variance[i] = online_average_variance(self->tempo_score_variance);
      sum_of_score_max += score_max[i];
      sum_of_score_variance += score_variance[i];
    }
  
  if((sum_of_score_max == 0) || sum_of_score_variance == 0)
    return;
  
  //score candidates based on xcorr peak value and varience
  float score, max_score =- 1;
  int   index_of_max_score = 0;
  for(i=0; i<self->num_tempo_candidates; i++)
    {
      score = (score_max[i] / sum_of_score_max) + (score_variance[i] / sum_of_score_variance);
      if(score >= max_score)
        {
          index_of_max_score = i;
          max_score = score;
        }
    }

  //this is way more likely to be because there were no peaks in the autocorrelation
  //(i.e. during silence) than because it was the the true tempo, so reject it.
  //in the future we might need to explicitely count non-zero samples in the OSS
  if(index_of_max_score == self->min_lag)
    return;
  
  //store decaying gaussian histogram
  int index_of_max_gaussian = 0;
  float gaussian, log_gaussian;;
  log_gaussian = log2(candidate_tempo_lags[index_of_max_score] * self->log_gaussian_tempo_weight_mean);
  log_gaussian *= log_gaussian;
  log_gaussian = exp(-log_gaussian / self->log_gaussian_tempo_weight_width) * self->one_minus_gaussian_tempo_histogram_decay;

  for(i=self->min_lag; i<self->max_lag; i++)
    {
      gaussian = i-candidate_tempo_lags[index_of_max_score];
      gaussian *= gaussian;
      gaussian = exp(-gaussian / self->gaussian_tempo_histogram_width);

      self->gaussian_tempo_histogram[i] *= self->gaussian_tempo_histogram_decay;
      self->gaussian_tempo_histogram[i] += gaussian * log_gaussian;
      if(self->gaussian_tempo_histogram[i] > self->gaussian_tempo_histogram[index_of_max_gaussian])
        index_of_max_gaussian = i;
    }
  
  self->beat_period_oss_samples = index_of_max_gaussian;
}

/*--------------------------------------------------------------------*/
void btt_beat_tracking               (BTT* self)
{
  int i;
  
  //calculate cbss
  float phi         = 0;
  float max_phi     = 0;
  int   start_index = -2   * self->beat_period_oss_samples;
  int   end_index   = -0.5 * self->beat_period_oss_samples;
  int   oss_index = (self->oss_index + self->oss_length - 1) % self->oss_length;
  int   base_cbss_index = self->cbss_index + (2*self->cbss_length);
  int   cbss_index;
  
  if(start_index < -self->cbss_length)
    start_index = -self->cbss_length;
  if(end_index < -self->cbss_length)
    end_index = -self->cbss_length;
  
  //equation 5, search for score of previous beat
  for(i=start_index; i<end_index; i++)
    {
      cbss_index = (base_cbss_index + i) % self->cbss_length;
      phi = log2(-(i/(float)self->beat_period_oss_samples));
      phi *= phi;
      phi *= self->cbss_eta;
      phi = exp(phi);
      phi *= self->cbss[cbss_index];
    
      if(phi > max_phi)
        max_phi = phi;
    }
  
  //equation 6, blend score of previous beat with oss
  self->cbss[self->cbss_index] = (self->one_minus_cbss_alpha * self->oss[oss_index]) + (self->cbss_alpha * max_phi);
  
  ++self->cbss_index; self->cbss_index %= self->cbss_length;
  
  //cross-correlate cbss with beat pulses
  int    num_pulses            = 4;
  float  pulse_locations[]     = {0, 1, 2, 3};
  float  pulse_values[]        = {1, 1, 1, 1};
  float* signal                = self->cbss;
  int    signal_length         = self->cbss_length;
  int    signal_index          = self->cbss_index;
  int    phase;
  float  max_phase = 0;
  float  val_of_max_phase = -1;

  for(phase=0; phase<self->beat_period_oss_samples; phase++)
    {
      int pulse;
      float x_corr = 0;
      for(pulse=0; pulse<num_pulses; pulse++)
        {
          int index = phase + pulse_locations[pulse] * self->beat_period_oss_samples + 1;
          if(index < signal_length)
            x_corr += signal[MODULO(signal_index-index, signal_length)] * pulse_values[pulse];
        }
      if(x_corr > val_of_max_phase)
        {
          val_of_max_phase = x_corr;
          max_phase = phase;
        }
    }
  
  //float test_orig_max_phase = max_phase;
  
  max_phase = self->beat_period_oss_samples - max_phase;
  max_phase -= self->beat_prediction_adjustment + self->analysis_latency_beat_adjustment_coarse - self->predicted_beat_trigger_index;
  max_phase =  fmod(max_phase, self->beat_period_oss_samples);
  
  //add this into the probabilities of future beats
  float gaussian;
  int   index, max_index=0;
  float max_value=-1;
  for(i=0; i<self->cbss_length; i++)
    {
      gaussian = (i % self->beat_period_oss_samples)-max_phase;
      gaussian *= gaussian;
      gaussian = exp(-gaussian / self->predicted_beat_gaussian_width);
      index = (self->predicted_beat_index + i) % self->cbss_length;
      self->predicted_beat_signal[index] += gaussian;
      if(self->predicted_beat_signal[index] > max_value)
        {
          max_value = self->predicted_beat_signal[index];
          max_index = i;
        }
    }
  
  self->predicted_beat_signal[self->predicted_beat_index] = 0;
  ++self->predicted_beat_index; self->predicted_beat_index %= self->cbss_length;
  
  //pick peak and call callback
  if(max_index == self->predicted_beat_trigger_index)
    {
      if(self->num_oss_frames_processed >= self->ignore_beats_until)
        {
          self->ignore_beats_until = (int)(self->num_oss_frames_processed + (self->beat_period_oss_samples * self->ignore_spurious_beats_duration));
          if(self->beat_callback != NULL)
            {
              unsigned long long t = self->num_audio_samples_processed;
              t += self->beat_prediction_adjustment * stft_get_hop(self->spectral_flux_stft);
              t -= self->analysis_latency_beat_adjustment_fine;
              self->beat_callback (self->beat_callback_self, t);
            }
        }
    }
  
    //fprintf(stderr, "%i\t%f\t%f\r\n", self->oss_index, self->oss[(self->oss_index + 1023) % 1024], //adaptive_threshold_threshold_value(self->onset_threshold));
    
/*
    if((self->cbss[(self->cbss_index+100) % self->oss_length] > 4.76103) && (self->cbss[(self->cbss_index+100 ) % self->oss_length] < 4.76104))
      {
        fprintf(stderr, "--------------------------------\r\n");
        fprintf(stderr, "bpm: %f\r\n", LAG_TO_BPM(self->beat_period_oss_samples));
        fprintf(stderr, "beat_period:%i\tmax_phase:%f\torig_max_phase:%f\r\n", self->beat_period_oss_samples, max_phase, test_orig_max_phase);
        for(i=0; i<self->oss_length; i++)
        {
          fprintf(stderr, "%f\t%f\t%f\r\n", self->cbss[(self->cbss_index+i) % self->oss_length], self->oss[(oss_index+i) % self->oss_length],       self->predicted_beat_signal[(self->predicted_beat_index + i) % self->cbss_length]);
         }
      }
*/
}

/*--------------------------------------------------------------------*/
void btt_spectral_flux_stft_callback(void* SELF, dft_sample_t* real, int N)
{
  BTT* self = (BTT*)SELF;

  if(self->tracking_mode >= BTT_COUNT_IN_TRACKING)
   btt_onset_tracking (self, real, N);
  ++self->num_oss_frames_processed;

  if(self->tracking_mode == BTT_METRONOME_MODE)
    {
      ++self->metronome_clock;
      if(self->metronome_clock >= self->metronome_lag)
        {
          unsigned long long t = self->num_audio_samples_processed;
          t += 0.5 * stft_get_hop(self->spectral_flux_stft);
          self->beat_callback (self->beat_callback_self, t);
          self->metronome_clock = 0;
        }
      return;
    }
  
  
  if((self->tracking_mode == BTT_ONSET_AND_TEMPO_TRACKING) ||
     (self->tracking_mode == BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING))
      if(self->num_oss_frames_processed >= self->oss_length)
        btt_tempo_tracking(self);

  if(self->tracking_mode >= BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING)
    if(self->beat_period_oss_samples > 0)
      btt_beat_tracking(self);
}

/*--------------------------------------------------------------------*/
/* resynthesized samples, if any, returned in real_input */
void btt_process(BTT* self, dft_sample_t* input, int num_samples)
{
  stft_process(self->spectral_flux_stft, input, num_samples, btt_spectral_flux_stft_callback, self);
  self->num_audio_samples_processed += num_samples;
}

/*--------------------------------------------------------------------*/
double    btt_get_sample_rate                  (BTT* self)
{
  return self->sample_rate;
}

/*--------------------------------------------------------------------*/
void      btt_set_use_amplitude_normalization  (BTT* self, int use)
{
  if(use < 0) use = 0;
  if(use > 1) use = 1;
  self->should_normalize_amplitude = use;
}

/*--------------------------------------------------------------------*/
int       btt_get_use_amplitude_normalization  (BTT* self)
{
  return self->should_normalize_amplitude;
}

/*--------------------------------------------------------------------*/
void      btt_set_spectral_compression_gamma   (BTT* self, double gamma)
{
  if(gamma < 0) gamma = 0;
  self->spectral_compression_gamma = gamma;
}

/*--------------------------------------------------------------------*/
double    btt_get_spectral_compression_gamma   (BTT* self)
{
  return self->spectral_compression_gamma;
}

/*--------------------------------------------------------------------*/
void      btt_set_oss_filter_cutoff            (BTT* self, double Hz)
{
  filter_set_cutoff(self->oss_filter, Hz);
}

/*--------------------------------------------------------------------*/
double    btt_get_oss_filter_cutoff            (BTT* self)
{
  return filter_get_cutoff(self->oss_filter);
}

/*--------------------------------------------------------------------*/
void      btt_set_autocorrelation_exponent     (BTT* self, double exponent)
{
  self->autocorrelation_exponent = exponent;
}

/*--------------------------------------------------------------------*/
double    btt_get_autocorrelation_exponent     (BTT* self)
{
  return self->autocorrelation_exponent;
}

/*--------------------------------------------------------------------*/
void      btt_set_min_tempo                    (BTT* self, double min_tempo)
{
  int max_lag = BPM_TO_LAG(min_tempo) + 1;
  if(max_lag < 0) max_lag = 0;
  
  if(max_lag > self->oss_length) max_lag = self->oss_length;
  self->max_lag = max_lag;
}

/*--------------------------------------------------------------------*/
double    btt_get_min_tempo                    (BTT* self)
{
  return LAG_TO_BPM(self->max_lag);
}

/*--------------------------------------------------------------------*/
void      btt_set_max_tempo                    (BTT* self, double max_tempo)
{
  int min_lag = BPM_TO_LAG(max_tempo);
  if(min_lag < 0) min_lag = 0;
  self->min_lag = min_lag;
}

/*--------------------------------------------------------------------*/
double    btt_get_max_tempo                    (BTT* self)
{
  return LAG_TO_BPM(self->min_lag);
}

/*--------------------------------------------------------------------*/
void      btt_set_onset_threshold              (BTT* self, double num_std_devs)
{
  adaptive_threshold_set_threshold(self->onset_threshold, num_std_devs);
}

/*--------------------------------------------------------------------*/
double    btt_get_onset_threshold              (BTT* self)
{
  return adaptive_threshold_threshold(self->onset_threshold);
}

/*--------------------------------------------------------------------*/
void      btt_set_onset_threshold_min            (BTT* self, double value)
{
  adaptive_threshold_set_threshold_min(self->onset_threshold, value);
}

/*--------------------------------------------------------------------*/
double    btt_get_onset_threshold_min            (BTT* self)
{
  return adaptive_threshold_threshold_min(self->onset_threshold);
}

/*--------------------------------------------------------------------*/
void      btt_set_num_tempo_candidates         (BTT* self, int num_candidates)
{
  if(num_candidates < 1)
    num_candidates = 1;
  self->num_tempo_candidates = num_candidates;
}

/*--------------------------------------------------------------------*/
int       btt_get_num_tempo_candidates         (BTT* self)
{
  return self->num_tempo_candidates;
}

/*--------------------------------------------------------------------*/
void      btt_set_noise_cancellation_threshold (BTT* self, double thresh)
{
  if(thresh > 0) thresh = 0;
  thresh = pow(10, thresh/20.0);
  self->noise_cancellation_threshold = thresh * stft_get_N (self->spectral_flux_stft);
}

/*--------------------------------------------------------------------*/
double    btt_get_noise_cancellation_threshold (BTT* self)
{
  double result = self->noise_cancellation_threshold / (double) stft_get_N (self->spectral_flux_stft);
  result = 20 * log10(result);
  return result;
}

/*--------------------------------------------------------------------*/
void      btt_set_cbss_alpha                   (BTT* self, double alpha)
{
  if(alpha < 0) alpha = 0;
  if(alpha > 1) alpha = 1;
  self->cbss_alpha = alpha;
  self->one_minus_cbss_alpha = 1 - alpha;
}

/*--------------------------------------------------------------------*/
double    btt_get_cbss_alpha                   (BTT* self)
{
  return self->cbss_alpha;
}

/*--------------------------------------------------------------------*/
void      btt_set_cbss_eta                     (BTT* self, double eta)
{
  if(eta < 0) eta = 0;
  self->cbss_eta = -eta;
}

/*--------------------------------------------------------------------*/
double    btt_get_cbss_eta                     (BTT* self)
{
  return -self->cbss_eta;
}

/*--------------------------------------------------------------------*/
void      btt_set_gaussian_tempo_histogram_decay         (BTT* self, double coefficient)
{
  if(coefficient < 0) coefficient = 0;
  if(coefficient > 0.999999) coefficient = 0.999999;
  self->gaussian_tempo_histogram_decay = coefficient;
  self->one_minus_gaussian_tempo_histogram_decay = 1 - coefficient;
}

/*--------------------------------------------------------------------*/
double    btt_get_gaussian_tempo_histogram_decay         (BTT* self)
{
  return self->gaussian_tempo_histogram_decay;
}

/*--------------------------------------------------------------------*/
void      btt_set_gaussian_tempo_histogram_width         (BTT* self, double width)
{
  if(width < 1) width = 1;
  self->gaussian_tempo_histogram_width = 2 * (width * width);
}

/*--------------------------------------------------------------------*/
double    btt_get_gaussian_tempo_histogram_width         (BTT* self)
{
  return sqrt(self->gaussian_tempo_histogram_width/2);
}

/*--------------------------------------------------------------------*/
void      btt_set_log_gaussian_tempo_weight_mean (BTT* self, double bpm)
{
  if(bpm < 1) bpm = 1;
  double width = btt_get_log_gaussian_tempo_weight_width(self);
  
  self->log_gaussian_tempo_weight_mean = 1.0 / BPM_TO_LAG(bpm);
  btt_set_log_gaussian_tempo_weight_width(self, width);
}

/*--------------------------------------------------------------------*/
double    btt_get_log_gaussian_tempo_weight_mean (BTT* self)
{
  return LAG_TO_BPM(1 / self->log_gaussian_tempo_weight_mean);
}

/*--------------------------------------------------------------------*/
void      btt_set_log_gaussian_tempo_weight_width(BTT* self, double bpm)
{
  if(bpm <= 0) return;
  
  double mean = btt_get_log_gaussian_tempo_weight_mean(self);
  if(mean <= 0) return;
  
  double denominator = bpm / mean;
  self->log_gaussian_tempo_weight_width = denominator * denominator;
}

/*--------------------------------------------------------------------*/
double    btt_get_log_gaussian_tempo_weight_width(BTT* self)
{
  return btt_get_log_gaussian_tempo_weight_mean(self) * sqrt(self->log_gaussian_tempo_weight_width);
}

/*--------------------------------------------------------------------*/
void      btt_set_beat_prediction_adjustment     (BTT* self, int oss_samples_earlier)
{
  self->beat_prediction_adjustment = oss_samples_earlier;
}

/*--------------------------------------------------------------------*/
int       btt_get_beat_prediction_adjustment     (BTT* self)
{
  return self->beat_prediction_adjustment;
}

/*--------------------------------------------------------------------*/
int       btt_get_beat_prediction_adjustment_audio_samples (BTT* self)
{
  return self->beat_prediction_adjustment * stft_get_hop(self->spectral_flux_stft);
}

/*--------------------------------------------------------------------*/
void      btt_set_predicted_beat_trigger_index   (BTT* self, int index)
{
  if(index < 0) index = 0;
  if(index >= self->cbss_length) index = self->cbss_length - 1;
  
  self->predicted_beat_trigger_index = index;
}

/*--------------------------------------------------------------------*/
int       btt_get_predicted_beat_trigger_index   (BTT* self)
{
  return self->predicted_beat_trigger_index;
}

/*--------------------------------------------------------------------*/
void      btt_set_predicted_beat_gaussian_width  (BTT* self, double width)
{
  if(width < 1) width = 1;
  self->predicted_beat_gaussian_width = 2 * width * width;
}

/*--------------------------------------------------------------------*/
double    btt_get_predicted_beat_gaussian_width  (BTT* self)
{
  return sqrt(self->predicted_beat_gaussian_width / 2);
}

/*--------------------------------------------------------------------*/
void      btt_set_ignore_spurious_beats_duration (BTT* self, double percent_of_tempo)
{
  if(percent_of_tempo < 0) percent_of_tempo = 0;
  self->ignore_spurious_beats_duration = percent_of_tempo * 0.01;
}

/*--------------------------------------------------------------------*/
double    btt_get_ignore_spurious_beats_duration (BTT* self)
{
  return self->ignore_spurious_beats_duration * 100;
}

/*--------------------------------------------------------------------*/
void      btt_set_analysis_latency_onset_adjustment    (BTT* self, int adjustment)
{
  self->analysis_latency_onset_adjustment = adjustment;
}

/*--------------------------------------------------------------------*/
int       btt_get_analysis_latency_onset_adjustment    (BTT* self)
{
  return self->analysis_latency_onset_adjustment;
}

/*--------------------------------------------------------------------*/
void      btt_set_analysis_latency_beat_adjustment(BTT* self, int adjustment)
{
  double hop = stft_get_hop(self->spectral_flux_stft);
  self->analysis_latency_beat_adjustment_coarse = round(adjustment / hop);
  self->analysis_latency_beat_adjustment_fine   = adjustment - hop*self->analysis_latency_beat_adjustment_coarse;
}

/*--------------------------------------------------------------------*/
int       btt_get_analysis_latency_beat_adjustment(BTT* self)
{
  return self->analysis_latency_beat_adjustment_coarse*stft_get_hop(self->spectral_flux_stft) + self->analysis_latency_beat_adjustment_fine;
}

/*--------------------------------------------------------------------*/
void       btt_set_count_in_n(BTT* self, int n)
{
  if(n < 1) n = 1;
  self->count_in_n = n;
}

/*--------------------------------------------------------------------*/
int      btt_get_count_in_n(BTT* self)
{
  return self->count_in_n;
}

/*--------------------------------------------------------------------*/
void                      btt_set_tracking_mode            (BTT* self, btt_tracking_mode_t mode)
{
  if((signed int) mode < 0)
    mode = (btt_tracking_mode_t)0;
  if(mode >= BTT_NUM_TRACKING_MODES)
    mode = (btt_tracking_mode_t)(BTT_NUM_TRACKING_MODES-1);
  
  if((self->tracking_mode == BTT_TEMPO_LOCKED_BEAT_TRACKING) && (mode != BTT_TEMPO_LOCKED_BEAT_TRACKING))
    btt_set_cbss_alpha(self, self->cbss_alpha_before_lock);
  if((mode == BTT_TEMPO_LOCKED_BEAT_TRACKING) && (self->tracking_mode != BTT_TEMPO_LOCKED_BEAT_TRACKING))
    {
      self->cbss_alpha_before_lock = btt_get_cbss_alpha(self);
      btt_set_cbss_alpha(self, 1);
    }
  if((mode == BTT_COUNT_IN_TRACKING) && (self->tracking_mode != BTT_COUNT_IN_TRACKING))
    {
      self->tracking_mode_before_count_in = self->tracking_mode;
      self->count_in_count              = 0;
      online_average_init (self->count_in_average);
    }
  
  self->tracking_mode = mode;
}

/*--------------------------------------------------------------------*/
btt_tracking_mode_t    btt_get_tracking_mode            (BTT* self)
{
  return self->tracking_mode;
}

/*--------------------------------------------------------------------*/
const char*            btt_get_tracking_mode_string     (BTT* self)
{
  btt_tracking_mode_t mode = btt_get_tracking_mode(self);
  
  if(((signed int)mode < 0) || (mode >= BTT_NUM_TRACKING_MODES))
    mode = BTT_NUM_TRACKING_MODES;
  
  const char* names[] =
  {
    "BTT_COUNT_IN_TRACKING",
    "BTT_ONSET_TRACKING",
    "BTT_ONSET_AND_TEMPO_TRACKING",
    "BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING",
    "BTT_TEMPO_LOCKED_BEAT_TRACKING",
    "BTT_METRONOME_MODE",
    "INVALID TRACKING MODE"
  };
  return names[mode];
}


/*--------------------------------------------------------------------*/
void                      btt_set_onset_tracking_callback  (BTT* self, btt_onset_callback_t callback, void* callback_self)
{
  self->onset_callback = callback;
  self->onset_callback_self = callback_self;
}

/*--------------------------------------------------------------------*/
btt_onset_callback_t   btt_get_onset_tracking_callback  (BTT* self, void** returned_callback_self)
{
  return self->onset_callback;
  *returned_callback_self = self->onset_callback_self;
}

/*--------------------------------------------------------------------*/
void                      btt_set_beat_tracking_callback   (BTT* self, btt_beat_callback_t callback, void* callback_self)
{
  self->beat_callback = callback;
  self->beat_callback_self = callback_self;
}

/*--------------------------------------------------------------------*/
btt_beat_callback_t    btt_get_beat_tracking_callback   (BTT* self, void** returned_callback_self)
{
  return self->beat_callback;
  *returned_callback_self = self->beat_callback_self;
}



#ifdef __cplusplus
} // extern "C"
#endif

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
