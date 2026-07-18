#include "linein_stream.h"

#include "esphome/core/log.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>
#include <inttypes.h>
#include <cmath>
#include <cstring>

namespace esphome {
namespace linein_stream {

static const char *const TAG = "linein_stream";

// Number of stereo frames read from I2S per DMA read.
// Matches dma_frame_num (512) — one clean wakeup per DMA descriptor.
// Kept at 512 (2 KB PCM sends) so each lwIP send completes quickly,
// leaving Core 0 available to process incoming FLAC data for Sendspin.
static constexpr size_t FRAMES_PER_READ = 512;

void LineInStreamComponent::i2s_init_trampoline_(void *arg) {
  auto *ctx = static_cast<I2SInitCtx_ *>(arg);
  ctx->ok = ctx->self->init_i2s_();
  ctx->done = true;
  vTaskDelete(nullptr);
}

void LineInStreamComponent::setup() {
  for (int i = 0; i < MAX_CLIENTS; i++)
    this->clients_[i] = -1;

  this->clients_lock_ = xSemaphoreCreateMutex();
  if (this->clients_lock_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create client mutex");
    this->mark_failed();
    return;
  }

  // Run I2S init from Core 0 so the DMA interrupt is allocated on Core 0.
  // ESPHome's setup() runs on Core 1; without this, the I2S1 DMA interrupt
  // fires on Core 1 every ~21 ms and disrupts the Sendspin audio pipeline.
  I2SInitCtx_ init_ctx{this, false, false};
  xTaskCreatePinnedToCore(i2s_init_trampoline_, "linein_init", 2048, &init_ctx,
                          10, nullptr, 0);
  while (!init_ctx.done)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (!init_ctx.ok) {
    ESP_LOGE(TAG, "I2S init failed");
    this->mark_failed();
    return;
  }

  // Compute EQ coefficients now that the sample rate is known.
  for (int i = 0; i < this->num_eq_bands_; i++)
    this->update_band_coeffs_(this->eq_bands_[i]);

  // Capture task: reads I2S, converts to 16-bit PCM, pushes to clients.
  // Pinned to Core 0 (alongside the Ethernet/lwIP stack) so it does not compete
  // with the Sendspin audio pipeline, which runs on Core 1.
  xTaskCreatePinnedToCore(LineInStreamComponent::i2s_task_trampoline, "linein_i2s", 4096, this, 5,
                          nullptr, 0);
  // Server task: accepts HTTP connections and registers client sockets.
  xTaskCreatePinnedToCore(LineInStreamComponent::server_task_trampoline, "linein_srv", 4096, this, 4,
                          nullptr, 0);

  ESP_LOGCONFIG(TAG, "Line-in streamer started on port %u", this->port_);
}

void LineInStreamComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Line-In Streamer:");
  ESP_LOGCONFIG(TAG, "  BCLK pin: GPIO%d", this->bclk_pin_);
  ESP_LOGCONFIG(TAG, "  LRCLK pin: GPIO%d", this->lrclk_pin_);
  ESP_LOGCONFIG(TAG, "  DIN pin: GPIO%d", this->din_pin_);
  if (this->has_mclk_)
    ESP_LOGCONFIG(TAG, "  MCLK pin: GPIO%d", this->mclk_pin_);
  ESP_LOGCONFIG(TAG, "  I2S role: %s", this->master_ ? "master" : "slave");
  ESP_LOGCONFIG(TAG, "  I2S port: %d", this->i2s_port_);
  ESP_LOGCONFIG(TAG, "  MCLK multiple: %dx", this->mclk_multiple_);
  ESP_LOGCONFIG(TAG, "  I2S format: %s", this->use_msb_ ? "msb" : "philips");
  ESP_LOGCONFIG(TAG, "  Gain: %.2fx", this->gain_);
  ESP_LOGCONFIG(TAG, "  Sample rate: %" PRIu32 " Hz", this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  Channels: %u", this->channels_);
  ESP_LOGCONFIG(TAG, "  HTTP port: %u", this->port_);
}

bool LineInStreamComponent::init_i2s_() {
  i2s_role_t role = this->master_ ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE;
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t) this->i2s_port_, role);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 512;
  chan_cfg.auto_clear = true;

  if (i2s_new_channel(&chan_cfg, nullptr, &this->rx_handle_) != ESP_OK)
    return false;

  // The ADC sends each sample MSB-first, left-justified in a 32-bit slot
  // (standard Philips I2S). We read stereo 32-bit slots and downshift to 16-bit.
  // The ESP32 drives BCK/WS as master; MCLK is optional (many boards self-clock).
  // NOTE: the *_SLOT_DEFAULT_CONFIG macros expand to braced initializers, which
  // can't be used in a ternary, so pick the format with an explicit if.
  i2s_std_slot_config_t slot_cfg =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  if (this->use_msb_) {
    i2s_std_slot_config_t msb_cfg =
        I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    slot_cfg = msb_cfg;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(this->sample_rate_),
      .slot_cfg = slot_cfg,
      .gpio_cfg =
          {
              .mclk = this->has_mclk_ ? (gpio_num_t) this->mclk_pin_ : I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t) this->bclk_pin_,
              .ws = (gpio_num_t) this->lrclk_pin_,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t) this->din_pin_,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  // 24.576 MHz MCLK at 512x/48k matches what the WM8782 board expects; the
  // multiple is configurable so it can be tuned per ADC.
  std_cfg.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t) this->mclk_multiple_;

  if (i2s_channel_init_std_mode(this->rx_handle_, &std_cfg) != ESP_OK)
    return false;
  if (i2s_channel_enable(this->rx_handle_) != ESP_OK)
    return false;

  return true;
}

void LineInStreamComponent::build_wav_header_(uint8_t header[44]) {
  const uint16_t bits = 16;
  const uint16_t block_align = this->channels_ * (bits / 8);
  const uint32_t byte_rate = this->sample_rate_ * block_align;
  // Streaming WAV: use a max/sentinel size so players read until connection close.
  const uint32_t data_size = 0xFFFFFFFFu;
  const uint32_t riff_size = 0xFFFFFFFFu;

  auto put32 = [](uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
  };
  auto put16 = [](uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
  };

  std::memcpy(header + 0, "RIFF", 4);
  put32(header + 4, riff_size);
  std::memcpy(header + 8, "WAVE", 4);
  std::memcpy(header + 12, "fmt ", 4);
  put32(header + 16, 16);                   // fmt chunk size
  put16(header + 20, 1);                     // PCM
  put16(header + 22, this->channels_);
  put32(header + 24, this->sample_rate_);
  put32(header + 28, byte_rate);
  put16(header + 32, block_align);
  put16(header + 34, bits);
  std::memcpy(header + 36, "data", 4);
  put32(header + 40, data_size);
}

void LineInStreamComponent::broadcast_(const uint8_t *data, size_t len) {
  xSemaphoreTake(this->clients_lock_, portMAX_DELAY);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    int fd = this->clients_[i];
    if (fd < 0)
      continue;

    size_t sent = 0;
    bool ok = true;
    while (sent < len) {
      // MSG_DONTWAIT: return EAGAIN immediately if the socket buffer is full
      // rather than blocking inside the lwIP tcpip task.  That task also
      // delivers incoming FLAC data to Sendspin, so blocking here starves it.
      ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        sent += (size_t) n;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Socket buffer momentarily full — skip this batch for this client.
        // Acceptable for a live stream; beats a multi-hundred-ms stall.
        break;
      } else {
        // Real error (connection reset, etc.) — drop the client.
        ok = false;
        break;
      }
    }
    if (!ok) {
      ESP_LOGD(TAG, "Dropping client fd=%d", fd);
      close(fd);
      this->clients_[i] = -1;
    }
  }
  xSemaphoreGive(this->clients_lock_);
}

void LineInStreamComponent::i2s_task_trampoline(void *arg) {
  static_cast<LineInStreamComponent *>(arg)->i2s_task_();
}

// Clamp a float sample to the signed 16-bit range.
static inline int16_t clamp16(float v) {
  if (v > 32767.0f)
    return 32767;
  if (v < -32768.0f)
    return -32768;
  return (int16_t) v;
}

// raw holds a 24-bit sample MSB-justified in a 32-bit slot (low 8 bits are 0),
// so the capture loop shifts it to a signed 24-bit value, scales to 16-bit,
// applies gain, then runs the EQ cascade below.
void LineInStreamComponent::add_eq_band(int type, float freq, float q, float gain_db) {
  if (this->num_eq_bands_ >= MAX_EQ_BANDS)
    return;
  EqBand &b = this->eq_bands_[this->num_eq_bands_];
  b.type = type;
  b.freq = freq;
  b.q = q;
  b.gain_db = gain_db;
  b.pending_gain_db.store(gain_db);
  this->update_band_coeffs_(b);
  this->num_eq_bands_++;
}

void LineInStreamComponent::set_band_gain(int index, float gain_db) {
  if (index < 0 || index >= this->num_eq_bands_)
    return;
  // Defer the recompute to the audio thread so coefficients never tear.
  this->eq_bands_[index].pending_gain_db.store(gain_db);
  this->eq_bands_[index].dirty.store(true);
}

// RBJ Audio-EQ-Cookbook biquad coefficients, normalized by a0.
void LineInStreamComponent::update_band_coeffs_(EqBand &b) {
  const float pi = 3.14159265358979f;
  const float fs = (float) this->sample_rate_;
  float A = powf(10.0f, b.gain_db / 40.0f);
  float w0 = 2.0f * pi * b.freq / fs;
  float cosw = cosf(w0);
  float sinw = sinf(w0);
  float alpha = sinw / (2.0f * b.q);

  float b0, b1, b2, a0, a1, a2;
  if (b.type == 1) {  // low shelf
    float ap1 = A + 1.0f, am1 = A - 1.0f, beta = 2.0f * sqrtf(A) * alpha;
    b0 = A * (ap1 - am1 * cosw + beta);
    b1 = 2.0f * A * (am1 - ap1 * cosw);
    b2 = A * (ap1 - am1 * cosw - beta);
    a0 = ap1 + am1 * cosw + beta;
    a1 = -2.0f * (am1 + ap1 * cosw);
    a2 = ap1 + am1 * cosw - beta;
  } else if (b.type == 2) {  // high shelf
    float ap1 = A + 1.0f, am1 = A - 1.0f, beta = 2.0f * sqrtf(A) * alpha;
    b0 = A * (ap1 + am1 * cosw + beta);
    b1 = -2.0f * A * (am1 + ap1 * cosw);
    b2 = A * (ap1 + am1 * cosw - beta);
    a0 = ap1 - am1 * cosw + beta;
    a1 = 2.0f * (am1 - ap1 * cosw);
    a2 = ap1 - am1 * cosw - beta;
  } else {  // peaking
    b0 = 1.0f + alpha * A;
    b1 = -2.0f * cosw;
    b2 = 1.0f - alpha * A;
    a0 = 1.0f + alpha / A;
    a1 = -2.0f * cosw;
    a2 = 1.0f - alpha / A;
  }
  b.b0 = b0 / a0;
  b.b1 = b1 / a0;
  b.b2 = b2 / a0;
  b.a1 = a1 / a0;
  b.a2 = a2 / a0;
}

void LineInStreamComponent::apply_pending_eq_updates_() {
  for (int i = 0; i < this->num_eq_bands_; i++) {
    if (this->eq_bands_[i].dirty.exchange(false)) {
      this->eq_bands_[i].gain_db = this->eq_bands_[i].pending_gain_db.load();
      this->update_band_coeffs_(this->eq_bands_[i]);
    }
  }
}

// Direct Form II Transposed cascade for one channel.
float LineInStreamComponent::eq_process_(float x, int ch) {
  for (int i = 0; i < this->num_eq_bands_; i++) {
    EqBand &b = this->eq_bands_[i];
    float y = b.b0 * x + b.z1[ch];
    b.z1[ch] = b.b1 * x - b.a1 * y + b.z2[ch];
    b.z2[ch] = b.b2 * x - b.a2 * y;
    x = y;
  }
  return x;
}

void LineInStreamComponent::i2s_task_() {
  // Raw stereo 32-bit samples straight from I2S.
  static int32_t raw[FRAMES_PER_READ * 2];
  // Converted 16-bit output (stereo interleaved, or mono downmix).
  static int16_t pcm[FRAMES_PER_READ * 2];

  while (true) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(this->rx_handle_, raw, sizeof(raw), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK || bytes_read == 0)
      continue;

    // Read the gain fresh each cycle so a runtime control (e.g. a HA number)
    // takes effect immediately.
    const float gain = this->gain_;

    // Fold in any EQ gain changes (done here, in the audio thread, so filter
    // coefficients are never read mid-update).
    this->apply_pending_eq_updates_();

    size_t frames = bytes_read / (2 * sizeof(int32_t));
    size_t out_bytes;

    if (this->channels_ == 2) {
      for (size_t f = 0; f < frames; f++) {
        float l = (float) (raw[2 * f] >> 8) * gain / 256.0f;
        float r = (float) (raw[2 * f + 1] >> 8) * gain / 256.0f;
        pcm[2 * f] = clamp16(this->eq_process_(l, 0));
        pcm[2 * f + 1] = clamp16(this->eq_process_(r, 1));
      }
      out_bytes = frames * 2 * sizeof(int16_t);
    } else {
      for (size_t f = 0; f < frames; f++) {
        int32_t l = raw[2 * f] >> 8;
        int32_t r = raw[2 * f + 1] >> 8;
        // Average the two 24-bit channels (/256 scale, /2 average) then gain.
        float m = (float) (l + r) * gain / 512.0f;
        pcm[f] = clamp16(this->eq_process_(m, 0));
      }
      out_bytes = frames * sizeof(int16_t);
    }

    this->broadcast_(reinterpret_cast<uint8_t *>(pcm), out_bytes);
  }
}

void LineInStreamComponent::server_task_trampoline(void *arg) {
  static_cast<LineInStreamComponent *>(arg)->server_task_();
}

void LineInStreamComponent::server_task_() {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "socket() failed: %d", errno);
    vTaskDelete(nullptr);
    return;
  }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(this->port_);

  if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed: %d", errno);
    close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }
  if (listen(listen_fd, 2) < 0) {
    ESP_LOGE(TAG, "listen() failed: %d", errno);
    close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }

  uint8_t wav[44];
  this->build_wav_header_(wav);

  const char *http_headers =
      "HTTP/1.0 200 OK\r\n"
      "Content-Type: audio/wav\r\n"
      "Cache-Control: no-cache, no-store\r\n"
      "Connection: close\r\n"
      "\r\n";

  while (true) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fd = accept(listen_fd, (struct sockaddr *) &client_addr, &client_len);
    if (fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Drain the request line(s); we serve the same stream regardless of path.
    char reqbuf[256];
    recv(fd, reqbuf, sizeof(reqbuf), 0);

    // Last-resort timeout for sends that somehow bypass MSG_DONTWAIT.
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 5 * 1000;  // 5 ms
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Send HTTP + WAV header up front (blocking).
    if (send(fd, http_headers, std::strlen(http_headers), MSG_NOSIGNAL) < 0 ||
        send(fd, wav, sizeof(wav), MSG_NOSIGNAL) < 0) {
      close(fd);
      continue;
    }

    // Register the client so the I2S task starts streaming PCM to it.
    bool added = false;
    xSemaphoreTake(this->clients_lock_, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (this->clients_[i] < 0) {
        this->clients_[i] = fd;
        added = true;
        break;
      }
    }
    xSemaphoreGive(this->clients_lock_);

    if (!added) {
      ESP_LOGW(TAG, "Max clients reached, rejecting connection");
      close(fd);
    } else {
      ESP_LOGD(TAG, "New client fd=%d", fd);
    }
  }
}

}  // namespace linein_stream
}  // namespace esphome
