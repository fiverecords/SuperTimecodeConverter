// BTT.h -- Beat-and-Tempo-Tracking public API
// Copyright (c) 2021 Michael Krzyzaniak -- MIT License
// https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking
//
// Standalone header for use with btt_amalgamation.c.
// No btt/ subfolder needed at build time.

#ifndef __BTT__
#define __BTT__ 1

#if defined(__cplusplus)
extern "C"{
#endif

typedef float dft_sample_t;

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

#define BTT_SUGGESTED_SPECTRAL_FLUX_STFT_LEN         1024
#define BTT_SUGGESTED_SPECTRAL_FLUX_STFT_OVERLAP     8
#define BTT_SUGGESTED_OSS_FILTER_ORDER               15
#define BTT_SUGGESTED_OSS_LENGTH                     1024
#define BTT_SUGGESTED_ONSET_THRESHOLD_N              1024
#define BTT_SUGGESTED_SAMPLE_RATE                    44100
#define BTT_SUGGESTED_CBSS_LENGTH                    1024

#define BTT_DEFAULT_ANALYSIS_LATENCY_ONSET_ADJUSTMENT 857
#define BTT_DEFAULT_ANALYSIS_LATENCY_BEAT_ADJUSTMENT  1270

#define BTT_DEFAULT_MIN_TEMPO                        50
#define BTT_DEFAULT_MAX_TEMPO                        200
#define BTT_DEFAULT_SPECTRAL_COMPRESSION_GAMMA       0
#define BTT_DEFAULT_AUTOCORRELATION_EXPONENT         0.5
#define BTT_DEFAULT_NUM_TEMPO_CANDIDATES             10
#define BTT_DEFAULT_TRACKING_MODE                    BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING
#define BTT_DEFAULT_OSS_FILTER_CUTOFF                10
#define BTT_DEFAULT_USE_AMP_NORMALIZATION            0
#define BTT_DEFAULT_ONSET_TREHSHOLD                  0.1
#define BTT_DEFAULT_ONSET_TREHSHOLD_MIN              5.0
#define BTT_DEFAULT_NOISE_CANCELLATION_THRESHOLD     -74
#define BTT_DEFAULT_LOG_GAUSSIAN_TEMPO_WEIGHT_MEAN   120
#define BTT_DEFAULT_LOG_GAUSSIAN_TEMPO_WEIGHT_WIDTH  75
#define BTT_DEFAULT_GAUSSIAN_TEMPO_HISTOGRAM_DECAY   0.999
#define BTT_DEFAULT_GAUSSIAN_TEMPO_HISTOGRAM_WIDTH   5
#define BTT_DEFAULT_CBSS_ALPHA                       0.9
#define BTT_DEFAULT_CBSS_ETA                         300
#define BTT_DEFAULT_BEAT_PREDICTION_ADJUSTMENT       10
#define BTT_DEFAULT_PREDICTED_BEAT_TRIGGER_INDEX      20
#define BTT_DEFAULT_PREDICTED_BEAT_GAUSSIAN_WIDTH    10
#define BTT_DEFAULT_IGNORE_SPURIOUS_BEATS_DURATION   40
#define BTT_DEFAULT_COUNT_IN_N                        2

#define BTT_DEFAULT_XCORR_NUM_PULSES                 8
#define BTT_DEFAULT_XCORR_PULSE_LOCATIONS            {0, 1, 1.5, 2, 3, 4, 4.5, 6}
#define BTT_DEFAULT_XCORR_PULSE_VALUES               {2.0, 1.0, 0.5, 1.5, 1.5, 0.5, 0.5, 0.5}

typedef struct Opaque_BTT_Struct BTT;

typedef void (*btt_onset_callback_t)(void* SELF, unsigned long long sample_time);
typedef void (*btt_beat_callback_t) (void* SELF, unsigned long long sample_time);

BTT*      btt_new                                (int spectral_flux_stft_len, int spectral_flux_stft_overlap,
                                                  int oss_filter_order      , int oss_length,
                                                  int cbss_length           , int onset_threshold_len, double sample_rate,
                                                  int analysis_latency_onset_adjustment, int analysis_latency_beat_adjustment);
BTT*      btt_new_default                        (void);
BTT*      btt_destroy                            (BTT* self);
void      btt_process                            (BTT* self, dft_sample_t* input, int num_samples);
double    btt_get_sample_rate                    (BTT* self);
void      btt_init                               (BTT* self);
void      btt_clear                              (BTT* self);
void      btt_init_tempo                         (BTT* self, double bpm);

int       btt_get_beat_period_audio_samples      (BTT* self);
double    btt_get_tempo_bpm                      (BTT* self);
double    btt_get_tempo_certainty                (BTT* self);
void      btt_set_count_in_n                     (BTT* self, int n);
int       btt_get_count_in_n                     (BTT* self);

void      btt_set_metronome_bpm                  (BTT* self, double bpm);

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
void      btt_set_noise_cancellation_threshold   (BTT* self, double dB);
double    btt_get_noise_cancellation_threshold   (BTT* self);

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

#if defined(__cplusplus)
}
#endif

#endif // __BTT__
