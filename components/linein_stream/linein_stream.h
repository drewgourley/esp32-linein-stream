#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <driver/i2s_std.h>

#include <atomic>

namespace esphome {
namespace linein_stream {

class LineInStreamComponent : public Component {
 public:
  void setup() override;
  void dump_config() override;
  // Start late so Wi-Fi / network stack are up before we open the socket.
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_bclk_pin(int pin) { this->bclk_pin_ = pin; }
  void set_lrclk_pin(int pin) { this->lrclk_pin_ = pin; }
  void set_din_pin(int pin) { this->din_pin_ = pin; }
  void set_mclk_pin(int pin) {
    this->mclk_pin_ = pin;
    this->has_mclk_ = true;
  }
  void set_mclk_multiple(int multiple) { this->mclk_multiple_ = multiple; }
  void set_master(bool master) { this->master_ = master; }
  void set_i2s_port(int port) { this->i2s_port_ = port; }
  void set_use_msb(bool use_msb) { this->use_msb_ = use_msb; }
  void set_gain(float gain) { this->gain_ = gain; }
  // Adds an EQ band at config time. type: 0=peaking, 1=low shelf, 2=high shelf.
  void add_eq_band(int type, float freq, float q, float gain_db);
  // Runtime-adjust a band's gain in dB (e.g. from a Home Assistant number).
  void set_band_gain(int index, float gain_db);
  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }
  void set_channels(uint8_t channels) { this->channels_ = channels; }
  void set_port(uint16_t port) { this->port_ = port; }

 protected:
  static constexpr int MAX_CLIENTS = 4;
  static constexpr int MAX_EQ_BANDS = 8;

  // A single RBJ biquad band (Direct Form II Transposed) with stereo state.
  struct EqBand {
    int type{0};  // 0=peaking, 1=low shelf, 2=high shelf
    float freq{1000.0f};
    float q{0.707f};
    float gain_db{0.0f};
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    float z1[2]{0.0f, 0.0f};
    float z2[2]{0.0f, 0.0f};
    std::atomic<float> pending_gain_db{0.0f};
    std::atomic<bool> dirty{false};
  };

  bool init_i2s_();
  void update_band_coeffs_(EqBand &band);
  void apply_pending_eq_updates_();
  float eq_process_(float x, int ch);
  void build_wav_header_(uint8_t header[44]);
  void broadcast_(const uint8_t *data, size_t len);

  static void i2s_task_trampoline(void *arg);
  static void server_task_trampoline(void *arg);
  // Runs init_i2s_() on Core 0 so the DMA interrupt is allocated there,
  // keeping Core 1 free for the Sendspin audio pipeline.
  struct I2SInitCtx_ {
    LineInStreamComponent *self;
    volatile bool done;
    volatile bool ok;
  };
  static void i2s_init_trampoline_(void *arg);
  void i2s_task_();
  void server_task_();

  int bclk_pin_{0};
  int lrclk_pin_{0};
  int din_pin_{0};
  int mclk_pin_{0};
  bool has_mclk_{false};
  int mclk_multiple_{256};
  bool master_{true};
  int i2s_port_{0};
  bool use_msb_{false};
  float gain_{1.0f};
  EqBand eq_bands_[MAX_EQ_BANDS];
  int num_eq_bands_{0};

  uint32_t sample_rate_{44100};
  uint8_t channels_{2};
  uint16_t port_{8080};

  i2s_chan_handle_t rx_handle_{nullptr};

  // Connected client socket fds (-1 when the slot is free).
  int clients_[MAX_CLIENTS];
  SemaphoreHandle_t clients_lock_{nullptr};
};

}  // namespace linein_stream
}  // namespace esphome
