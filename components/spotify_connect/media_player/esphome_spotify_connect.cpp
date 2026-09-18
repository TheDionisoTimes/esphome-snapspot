#include "esphome/core/defines.h"
#ifdef USE_ESP32
#ifndef USE_I2S_LEGACY

#include "esphome_spotify_connect.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esphome/components/network/util.h"
#include "esphome/components/media_player/media_player.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "mdns.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_SHARED_AUDIO_EQ
#include "esphome/components/snapspot/shared_audio_eq.h"
#endif
#include "esphome/components/snapspot/cspot_gate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <atomic>

#include "AccessKeyFetcher.h"
#include "HTTPClient.h"
#include "cJSON.h"

namespace esphome::spotify_connect {

struct SpotifyMetaUpdate {
  char title[128];
  char artist[96];
  char album[96];
  char image_url[256];
};

static constexpr const char *NVS_NAMESPACE = "spotify_connect";
static constexpr const char *NVS_KEY_VOLUME = "spot_vol";
static constexpr const char *NVS_KEY_CRED = "spot_cred"; // Auto-reconnect credentials (JSON)

static float nvs_load_float(const char *key, float default_val) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return default_val;
  uint32_t raw = 0;
  float out = default_val;
  if (nvs_get_u32(handle, key, &raw) == ESP_OK)
    memcpy(&out, &raw, sizeof(float));
  nvs_close(handle);
  return out;
}

static void nvs_save_float(const char *key, float value) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return;
  uint32_t raw = 0;
  memcpy(&raw, &value, sizeof(float));
  nvs_set_u32(handle, key, raw);
  nvs_commit(handle);
  nvs_close(handle);
}


static std::string nvs_load_string_blob(const char *key) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return "";
  size_t required_size = 0;
  if (nvs_get_blob(handle, key, nullptr, &required_size) != ESP_OK || required_size == 0) {
    nvs_close(handle);
    return "";
  }
  std::string out(required_size, '\0');
  if (nvs_get_blob(handle, key, out.data(), &required_size) != ESP_OK) {
    nvs_close(handle);
    return "";
  }
  nvs_close(handle);
  return out;
}

static void nvs_save_string_blob(const char *key, const std::string &data) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_set_blob(handle, key, data.data(), data.size());
  nvs_commit(handle);
  nvs_close(handle);
}

static void nvs_erase_cred(const char *key) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_erase_key(handle, key);
  nvs_commit(handle);
  nvs_close(handle);
}

void SpotifyConnectComponent::setup() {
  ESP_LOGV(TAG, "setup() executing");

  if (this->source_) {
    this->source_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, 44100));
    ESP_LOGI(TAG, "set_audio_stream_info: 44100 Hz 16-bit stereo (source_=%p)", this->source_);
  } else {
    ESP_LOGW(TAG, "source_ is NULL in setup() — set_audio_stream_info skipped!");
  }

  this->volume_ = nvs_load_float(NVS_KEY_VOLUME, 0.2f);
  ESP_LOGD(TAG, "Restored volume from NVS: %.3f", this->volume_);

  this->set_volume_(this->volume_, true);

  bell::setDefaultLogger();

  this->login_blob_ = std::make_shared<cspot::LoginBlob>(this->device_name_);

  httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
  http_config.server_port = 8080;
  http_config.ctrl_port = 32769;
  http_config.max_uri_handlers = 4;

  httpd_uri_t get_info = {
    .uri = "/spotify_info",
    .method = HTTP_GET,
    .handler = [](httpd_req_t *req) -> esp_err_t {
      int64_t t0 = esp_timer_get_time();
      auto *self = static_cast<SpotifyConnectComponent *>(req->user_ctx);
      std::string info = self->login_blob_->buildZeroconfInfo();
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send(req, info.c_str(), info.size());
      int64_t dur_ms = (esp_timer_get_time() - t0) / 1000;
      if (dur_ms > 20) {
        ESP_LOGW("cspot", "GET /spotify_info took %lldms", (long long)dur_ms);
      }
      return ESP_OK;
    },
    .user_ctx = this,
  };

  httpd_uri_t post_info = {
    .uri = "/spotify_info",
    .method = HTTP_POST,
    .handler = [](httpd_req_t *req) -> esp_err_t {
      int64_t t0 = esp_timer_get_time();
      auto *self = static_cast<SpotifyConnectComponent *>(req->user_ctx);
      char buf[1024];
      int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
      if (len <= 0) return ESP_FAIL;
      buf[len] = '\0';
      ESP_LOGW("cspot", "POST /spotify_info received %d bytes", len);
      self->handle_zeroconf_post_(buf, len);
      httpd_resp_send(req, "{\"status\":101,\"statusString\":\"ERROR-OK\",\"spotifyError\":0}", HTTPD_RESP_USE_STRLEN);
      int64_t dur_ms = (esp_timer_get_time() - t0) / 1000;
      ESP_LOGW("cspot", "POST /spotify_info took %lldms", (long long)dur_ms);
      return ESP_OK;
    },
    .user_ctx = this,
  };

  if (httpd_start(&this->http_server_, &http_config) == ESP_OK) {
    httpd_register_uri_handler(this->http_server_, &get_info);
    httpd_register_uri_handler(this->http_server_, &post_info);
    ESP_LOGI(TAG, "Zeroconf HTTP server started on port 8080");
    ESP_LOGI(TAG, "Heap: internal free=%u largest=%u  PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  } else {
    ESP_LOGE(TAG, "Failed to start Zeroconf HTTP server");
  }

  this->network_initialized_ = false;
  this->state = media_player::MEDIA_PLAYER_STATE_OFF;

  esphome::snapspot::cspot_callbacks.ctx = this;
  esphome::snapspot::cspot_callbacks.close_connection = [](void *c) {
    static_cast<SpotifyConnectComponent *>(c)->close_connection_impl();
  };
  esphome::snapspot::cspot_callbacks.are_tasks_exited = [](void *c) -> bool {
    return static_cast<SpotifyConnectComponent *>(c)->are_tasks_exited_impl();
  };
  esphome::snapspot::cspot_callbacks.finalize_cleanup = [](void *c) {
    static_cast<SpotifyConnectComponent *>(c)->finalize_cleanup_impl();
  };
  esphome::snapspot::cspot_callbacks.force_cleanup = [](void *c) {
    static_cast<SpotifyConnectComponent *>(c)->force_cleanup_impl();
  };

  if (this->track_name_sensor_) this->track_name_sensor_->publish_state("");
  if (this->artist_sensor_)     this->artist_sensor_->publish_state("");
  if (this->album_sensor_)      this->album_sensor_->publish_state("");
  if (this->album_art_url_sensor_) this->album_art_url_sensor_->publish_state("");
  this->meta_q_hdl_ = xQueueCreate(1, sizeof(SpotifyMetaUpdate));
}

void SpotifyConnectComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Spotify Connect Media Player:");
  ESP_LOGCONFIG(TAG, "  Device Name: %s", this->device_name_.c_str());
#ifdef USE_AUDIO_DAC
  ESP_LOGCONFIG(TAG, "  Audio DAC: %s",
                this->audio_dac_ != nullptr ? "configured" : "None");
#else
  ESP_LOGCONFIG(TAG, "  Audio DAC: None");
#endif
}

static void url_decode_inplace(char *str) {
  if (!str) return;
  char *d = str, *s = str;
  char hex[3] = {0};
  while (*s) {
    if (*s == '%' && s[1] && s[2]) {
      hex[0] = s[1];
      hex[1] = s[2];
      *d++ = static_cast<char>(strtol(hex, nullptr, 16));
      s += 3;
    } else if (*s == '+') {
      *d++ = ' ';
      s++;
    } else {
      *d++ = *s++;
    }
  }
  *d = '\0';
}

void SpotifyConnectComponent::auth_task_wrapper_(void *param) {
  auto *self = static_cast<SpotifyConnectComponent *>(param);
  try {
    self->run_auth_task_();
  } catch (const std::exception &e) {
    ESP_LOGE(TAG, "Auth task crashed: %s", e.what());
  } catch (...) {
    ESP_LOGE(TAG, "Auth task crashed with unknown exception");
  }
  self->auth_in_progress_ = false;
  vTaskDelete(nullptr);
}

void SpotifyConnectComponent::run_auth_task_() {
  std::string new_user = this->login_blob_->getUserName();
  if (this->spirc_handler_ && !this->last_auth_username_.empty() &&
      new_user == this->last_auth_username_) {
    ESP_LOGI(TAG, "Credentials unchanged and session active - ignoring duplicate auth");
    this->auth_in_progress_ = false;
    return;
  }

  if (this->packet_task_handle_ || this->playback_task_handle_) {
    this->pending_shutdown_ = true;
    this->is_playing_ = false;
    this->draining_ = false;
    if (this->cspot_context_ && this->cspot_context_->session) {
      this->cspot_context_->session->closeConnection();
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    if (this->packet_task_handle_) {
      vTaskDelete(this->packet_task_handle_);
      this->packet_task_handle_ = nullptr;
    }
    if (this->playback_task_handle_) {
      vTaskDelete(this->playback_task_handle_);
      this->playback_task_handle_ = nullptr;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    this->spirc_handler_.reset();
    this->cspot_context_.reset();
    this->audio_buffer_.reset();
    this->last_auth_username_.clear();
    if (this->pkt_task_stack_)  { heap_caps_free(this->pkt_task_stack_);  this->pkt_task_stack_ = nullptr; }
    if (this->pkt_task_buf_)    { heap_caps_free(this->pkt_task_buf_);    this->pkt_task_buf_ = nullptr; }
    if (this->play_task_stack_) { heap_caps_free(this->play_task_stack_); this->play_task_stack_ = nullptr; }
    if (this->play_task_buf_)   { heap_caps_free(this->play_task_buf_);   this->play_task_buf_ = nullptr; }
    this->pending_shutdown_ = false;
    this->disconnect_called_ = false;
    esphome::snapspot::cspot_shutdown_pending = false;
  }

  bell::setDefaultLogger();

  if (this->auto_reconnect_from_nvs_) {
    // === AUTO-RECONNECT: load saved credentials from NVS ===
    std::string saved_json = this->nvs_load_cred_();
    if (saved_json.empty()) {
      ESP_LOGE(TAG, "Auto-reconnect: no saved credentials in NVS — aborting");
      this->auth_in_progress_ = false;
      this->auto_reconnect_from_nvs_ = false;
      return;
    }
    ESP_LOGI(TAG, "Auto-reconnect: loading saved credentials from NVS (%u bytes)",
             (unsigned)saved_json.size());
    this->login_blob_->loadJson(saved_json);
    this->auto_reconnect_from_nvs_ = false;
  } else {
    // === NORMAL: load from Zeroconf POST (Spotify app sent credentials) ===
    this->login_blob_->loadZeroconfQuery(this->pending_auth_params_);
  }

  if (this->login_blob_->getUserName().empty()) {
    ESP_LOGE(TAG, "Auth failed (corrupt blob?) — aborting");
    this->auth_in_progress_ = false;
    return;
  }

  esphome::snapspot::cspot_active.store(true, std::memory_order_release);
  esphome::snapspot::cspot_fully_stopped.store(false, std::memory_order_release);
  esphome::snapspot::cspot_tasks_exited.store(false, std::memory_order_release);

  {
    uint32_t t0 = millis();
    while (!esphome::snapspot::audio_path_free.load(std::memory_order_acquire)) {
      if (millis() - t0 > 30000) {
        ESP_LOGW(TAG, "Timeout waiting for audio_path_free — proceeding anyway");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "audio_path_free after %ums (heap=%u)", (unsigned)(millis() - t0),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }

  this->cspot_context_ = cspot::Context::createFromBlob(this->login_blob_);
  this->cspot_context_->config.audioFormat = AudioFormat_OGG_VORBIS_160;

  bool connected = false;
  for (int attempt = 0; attempt < 3 && !connected; attempt++) {
    try {
      this->cspot_context_->session->connectWithRandomAp();
      auto token = this->cspot_context_->session->authenticate(this->login_blob_);
      if (!token.empty()) {
        connected = true;
      }
    } catch (const std::exception &e) {
      ESP_LOGW(TAG, "Auth attempt %d failed: %s", attempt + 1, e.what());
      if (attempt < 2) {
        this->cspot_context_ = cspot::Context::createFromBlob(this->login_blob_);
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
    }
  }

  if (!connected) {
    ESP_LOGE(TAG, "Authentication failed after 3 attempts");
    esphome::snapspot::cspot_active.store(false, std::memory_order_release);
    esphome::snapspot::cspot_fully_stopped.store(true, std::memory_order_release);
    esphome::snapspot::cspot_tasks_exited.store(true, std::memory_order_release);
    this->auth_in_progress_ = false;
    return;
  }

  ESP_LOGI(TAG, "Spotify authenticated as: %s",
           this->cspot_context_->config.username.c_str());
  this->last_auth_username_ = this->cspot_context_->config.username;
  this->pending_shutdown_ = false;
  this->idle_since_ms_ = 0;

  // === SAVE CREDENTIALS TO NVS FOR AUTO-RECONNECT ===
  {
    std::string cred_json = this->login_blob_->toJson();
    if (!cred_json.empty()) {
      this->nvs_save_cred_(cred_json);
      ESP_LOGI(TAG, "Auto-reconnect: credentials saved to NVS (%u bytes)",
               (unsigned)cred_json.size());
    }
  }

  this->cspot_context_->session->startTask();

  this->audio_buffer_ = std::make_unique<bell::CircularBuffer>(1536 * 1024);

  this->spirc_handler_ = std::make_shared<cspot::SpircHandler>(this->cspot_context_);

  this->spirc_handler_->getTrackPlayer()->setDataCallback(
    [this, last_id = std::string{}](uint8_t *data, size_t bytes, std::string_view track_id) mutable -> size_t {
      if (!track_id.empty() && track_id != last_id) {
        last_id = std::string(track_id);
        if (this->spirc_handler_) {
          this->spirc_handler_->notifyAudioReachedPlayback();
        }
      }
      return this->audio_buffer_ ? this->audio_buffer_->write(data, bytes) : 0;
    });

  this->spirc_handler_->setEventHandler(
    [this](std::unique_ptr<cspot::SpircHandler::Event> event) {
      this->handle_spirc_event_(std::move(event));
    });

  uint32_t vol_raw = static_cast<uint32_t>(this->volume_ * 65535.0f);
  this->spirc_handler_->setRemoteVolume(static_cast<int>(vol_raw));

  const uint32_t stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

  this->pkt_task_exited_.store(false, std::memory_order_release);
  this->pkt_task_stack_ = static_cast<StackType_t *>(
    heap_caps_malloc(12288, stack_caps));
  this->pkt_task_buf_ = static_cast<StaticTask_t *>(
    heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!this->pkt_task_stack_ || !this->pkt_task_buf_) {
    ESP_LOGE(TAG, "Failed to allocate packet task memory");
    heap_caps_free(this->pkt_task_stack_);  this->pkt_task_stack_ = nullptr;
    heap_caps_free(this->pkt_task_buf_);    this->pkt_task_buf_ = nullptr;
    esphome::snapspot::cspot_active.store(false, std::memory_order_release);
    esphome::snapspot::cspot_fully_stopped.store(true, std::memory_order_release);
    esphome::snapspot::cspot_tasks_exited.store(true, std::memory_order_release);
    this->auth_in_progress_ = false;
    return;
  }
  this->packet_task_handle_ = xTaskCreateStaticPinnedToCore(
    packet_task_wrapper_, "spotify_pkt", 12288, this, 10,
    this->pkt_task_stack_, this->pkt_task_buf_, 0);

  this->play_task_exited_.store(false, std::memory_order_release);
  this->play_task_stack_ = static_cast<StackType_t *>(
    heap_caps_malloc(8192, stack_caps));
  this->play_task_buf_ = static_cast<StaticTask_t *>(
    heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!this->play_task_stack_ || !this->play_task_buf_) {
    ESP_LOGE(TAG, "Failed to allocate playback task memory");
    heap_caps_free(this->play_task_stack_);  this->play_task_stack_ = nullptr;
    heap_caps_free(this->play_task_buf_);    this->play_task_buf_ = nullptr;
    esphome::snapspot::cspot_active.store(false, std::memory_order_release);
    esphome::snapspot::cspot_fully_stopped.store(true, std::memory_order_release);
    esphome::snapspot::cspot_tasks_exited.store(true, std::memory_order_release);
    this->auth_in_progress_ = false;
    return;
  }
  this->playback_task_handle_ = xTaskCreateStaticPinnedToCore(
    playback_task_wrapper_, "spotify_play", 8192, this, 20,
    this->play_task_stack_, this->play_task_buf_, 1);

  ESP_LOGI(TAG, "Spotify Connect ready -- waiting for playback");
}

void SpotifyConnectComponent::packet_task_wrapper_(void *param) {
  auto *self = static_cast<SpotifyConnectComponent *>(param);
  bell::setDefaultLogger();
  while (!self->pending_shutdown_) {
    try {
      self->cspot_context_->session->handlePacket();
    } catch (const std::exception &e) {
      if (self->pending_shutdown_) break;
      ESP_LOGW(TAG, "Packet error: %s", e.what());
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  ESP_LOGI(TAG, "Packet task: signaling exit");
  self->pkt_task_exited_.store(true, std::memory_order_release);
  vTaskSuspend(nullptr);
}

void SpotifyConnectComponent::playback_task_wrapper_(void *param) {
  auto *self = static_cast<SpotifyConnectComponent *>(param);
  uint8_t pcm_buf[1024];
  bool prebuffered = false;

  while (!self->pending_shutdown_) {
    if (!self->is_playing_) {
      prebuffered = false;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (!prebuffered) {
      if (self->audio_buffer_->size() < 352800) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      prebuffered = true;
      ESP_LOGI(TAG, "Prebuffer complete, starting output");
    }

    if (self->draining_ && self->audio_buffer_->size() == 0) {
      ESP_LOGI(TAG, "DRAIN: buffer empty — going idle");
      self->draining_ = false;
      self->is_playing_ = false;
      self->audio_buffer_->emptyBuffer();
      self->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      self->mute_state_ = true;
      self->current_track_.clear();
      self->current_artist_.clear();
      self->current_album_.clear();
      self->current_image_url_.clear();
      self->current_duration_ms_ = 0;
      self->media_position_ms_ = 0;
      if (self->track_name_sensor_) self->track_name_sensor_->publish_state("");
      if (self->artist_sensor_) self->artist_sensor_->publish_state("");
      if (self->album_sensor_) self->album_sensor_->publish_state("");
      if (self->album_art_url_sensor_) self->album_art_url_sensor_->publish_state("");
      if (self->duration_sensor_) self->duration_sensor_->publish_state(0.0f);
      if (self->position_sensor_) self->position_sensor_->publish_state(0.0f);
      self->publish_state();
      continue;
    }

    size_t bytes = self->audio_buffer_->read(pcm_buf, sizeof(pcm_buf));
    if (bytes == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

#ifdef USE_SHARED_AUDIO_EQ
    if (bytes >= 4) {
      eq_process_with_volume((int16_t*)pcm_buf, bytes / 4, EQ_SOURCE_CSPOT);
    }
#endif

    if (self->source_) {

      if (!self->cspot_format_set_) {
        self->source_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, 44100));
        ESP_LOGI(TAG, "set_audio_stream_info at first play: 44100 Hz 16-bit stereo");
        self->cspot_format_set_ = true;
      }
      size_t wr = self->source_->play(pcm_buf, bytes, pdMS_TO_TICKS(100));
      static uint32_t cspot_wr_cnt = 0;
      static uint32_t cspot_wr_zero = 0;
      if (wr == 0) cspot_wr_zero++;
      if (++cspot_wr_cnt >= 200) {
        ESP_LOGD(TAG, "play: last=%u/%u zeros=%lu buf=%u core=%d",
                 (unsigned)wr, (unsigned)bytes, (unsigned long)cspot_wr_zero,
                 (unsigned)self->audio_buffer_->size(), xPortGetCoreID());
        cspot_wr_cnt = 0;
        cspot_wr_zero = 0;
      }
    }
  }
  ESP_LOGI(TAG, "Playback task: signaling exit");
  self->play_task_exited_.store(true, std::memory_order_release);
  vTaskSuspend(nullptr);
}

void SpotifyConnectComponent::handle_spirc_event_(
    std::unique_ptr<cspot::SpircHandler::Event> event) {
  if (this->pending_shutdown_) {
    ESP_LOGD(TAG, "SPIRC event %d ignored (shutting down)",
             static_cast<int>(event->eventType));
    return;
  }
  switch (event->eventType) {

    case cspot::SpircHandler::EventType::PLAYBACK_START:
      ESP_LOGI(TAG, "SPIRC: PLAYBACK_START");
      this->audio_buffer_->emptyBuffer();
      this->draining_ = false;
      this->is_playing_ = true;
      this->pending_disc_ = false;
      this->disc_hold_until_ms_ = millis() + 5000;
      this->mute_state_ = false;
#ifdef USE_SHARED_AUDIO_EQ
      eq_set_sample_rate(44100);
#endif
      this->set_volume_(this->volume_, false);
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
      this->publish_state();
      break;

    case cspot::SpircHandler::EventType::PLAY_PAUSE: {
      bool paused = std::get<bool>(event->data);
      ESP_LOGI(TAG, "SPIRC: PLAY_PAUSE: %s", paused ? "paused" : "resumed");
      if (paused) {
        this->is_playing_ = false;
        this->audio_buffer_->emptyBuffer();
        if (this->source_) this->source_->stop();
        this->mute_state_ = true;
        this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
      } else {
        this->is_playing_ = true;
        this->mute_state_ = false;
        this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
      }
      this->publish_state();
      break;
    }

    case cspot::SpircHandler::EventType::VOLUME: {
      int vol_raw = std::get<int>(event->data);
      float vol_float = static_cast<float>(vol_raw) / 65535.0f;
      ESP_LOGD(TAG, "SPIRC: VOLUME %d (%.0f%%)", vol_raw, vol_float * 100.0f);
      uint32_t now = millis();
      if (now - this->last_ha_volume_ms_ < 2000) {
        ESP_LOGD(TAG, "SPIRC VOLUME: debounced (HA echo)");
        break;
      }
      this->last_server_volume_ms_ = now;
      this->set_volume_(vol_float, true);
      break;
    }

    case cspot::SpircHandler::EventType::TRACK_INFO: {
      auto info = std::get<cspot::TrackInfo>(event->data);
      ESP_LOGI(TAG, "SPIRC: TRACK_INFO: %s - %s (%s) [%u ms]",
               info.artist.c_str(), info.name.c_str(),
               info.album.c_str(), info.duration);
      this->current_track_ = info.name;
      this->current_artist_ = info.artist;
      this->current_album_ = info.album;
      this->current_image_url_ = info.imageUrl;
      this->current_duration_ms_ = info.duration;
      this->media_position_ms_ = 0;
      this->media_position_updated_at_ = millis();
      this->last_position_publish_ms_ = millis();
      this->disc_hold_until_ms_ = millis() + 8000;
      this->pending_disc_ = false;
      if (this->meta_q_hdl_) {
        SpotifyMetaUpdate upd{};
        strncpy(upd.title,     info.name.c_str(),     sizeof(upd.title)     - 1);
        strncpy(upd.artist,    info.artist.c_str(),   sizeof(upd.artist)    - 1);
        strncpy(upd.album,     info.album.c_str(),    sizeof(upd.album)     - 1);
        strncpy(upd.image_url, info.imageUrl.c_str(), sizeof(upd.image_url) - 1);
        xQueueOverwrite(this->meta_q_hdl_, &upd);
      }
      if (this->duration_sensor_)
        this->duration_sensor_->publish_state(static_cast<float>(info.duration) / 1000.0f);
      if (this->position_sensor_) this->position_sensor_->publish_state(0.0f);
      break;
    }

    case cspot::SpircHandler::EventType::FLUSH:
    case cspot::SpircHandler::EventType::SEEK:
      this->draining_ = false;
      this->audio_buffer_->emptyBuffer();
      this->pending_disc_ = false;
      this->disc_hold_until_ms_ = millis() + 8000;
      break;

    case cspot::SpircHandler::EventType::DISC: {
      ESP_LOGI(TAG, "SPIRC: DISC (disconnected)");
      uint32_t now = millis();
      if (this->is_playing_ || (this->disc_hold_until_ms_ != 0 && now < this->disc_hold_until_ms_)) {
        if (!this->pending_disc_) {
          ESP_LOGW(TAG, "DISC deferred 500ms (spurious check)");
          this->pending_disc_ = true;
          this->pending_disc_at_ms_ = now;
        }
        break;
      }
      this->disc_hold_until_ms_ = 0;
      this->draining_ = false;
      this->is_playing_ = false;
      this->audio_buffer_->emptyBuffer();
      if (this->source_) this->source_->stop();
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      this->current_track_.clear();
      this->current_artist_.clear();
      this->current_album_.clear();
      this->current_image_url_.clear();
      this->current_duration_ms_ = 0;
      this->media_position_ms_ = 0;
      if (this->track_name_sensor_) this->track_name_sensor_->publish_state("");
      if (this->artist_sensor_) this->artist_sensor_->publish_state("");
      if (this->album_sensor_) this->album_sensor_->publish_state("");
      if (this->album_art_url_sensor_) this->album_art_url_sensor_->publish_state("");
      if (this->duration_sensor_) this->duration_sensor_->publish_state(0.0f);
      if (this->position_sensor_) this->position_sensor_->publish_state(0.0f);
      this->mute_state_ = true;
      this->publish_state();
      this->pending_shutdown_ = true;
      esphome::snapspot::cspot_shutdown_pending = true;
      break;
    }

    case cspot::SpircHandler::EventType::DEPLETED:
      ESP_LOGI(TAG, "SPIRC: DEPLETED (queue empty) — draining buffer");
      this->draining_ = true;
      break;

    default:
      ESP_LOGD(TAG, "SPIRC: unhandled event %d", static_cast<int>(event->eventType));
      break;
  }
}

void SpotifyConnectComponent::handle_zeroconf_post_(const char *body, int len) {
  if (this->pending_shutdown_) {
    ESP_LOGW(TAG, "Session shutting down, ignoring Zeroconf POST");
    return;
  }
  if (this->auth_in_progress_) {
    ESP_LOGW(TAG, "Auth already in progress, ignoring duplicate POST");
    return;
  }

  std::vector<char> buf(body, body + len + 1);
  this->pending_auth_params_.clear();

  char *tok = strtok(buf.data(), "&");
  while (tok) {
    char *eq = strchr(tok, '=');
    if (eq) {
      *eq = '\0';
      char *val = eq + 1;
      url_decode_inplace(val);
      this->pending_auth_params_[tok] = val;
    }
    tok = strtok(nullptr, "&");
  }

  ESP_LOGI(TAG, "Zeroconf POST: user=%s",
           this->pending_auth_params_.count("userName")
             ? this->pending_auth_params_["userName"].c_str() : "(none)");

  this->auth_in_progress_ = true;

  if (this->auth_task_stack_) {
    heap_caps_free(this->auth_task_stack_);
    this->auth_task_stack_ = nullptr;
  }
  if (this->auth_task_buf_) {
    heap_caps_free(this->auth_task_buf_);
    this->auth_task_buf_ = nullptr;
  }

  this->auth_task_buf_ = static_cast<StaticTask_t *>(
    heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  this->auth_task_stack_ = static_cast<StackType_t *>(
    heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!this->auth_task_buf_ || !this->auth_task_stack_) {
    heap_caps_free(this->auth_task_buf_);  this->auth_task_buf_   = nullptr;
    heap_caps_free(this->auth_task_stack_); this->auth_task_stack_ = nullptr;
    this->auth_in_progress_ = false;
    ESP_LOGE(TAG, "Failed to allocate auth task stack");
    return;
  }
  xTaskCreateStaticPinnedToCore(
    auth_task_wrapper_, "spotify_auth", 16384, this, 5,
    this->auth_task_stack_, this->auth_task_buf_, 0);
}

void SpotifyConnectComponent::close_connection_impl() {
  ESP_LOGI(TAG, "close_connection: closing socket to unblock tasks");
  this->pending_shutdown_ = true;
  esphome::snapspot::cspot_tasks_exited.store(false, std::memory_order_release);
  if (this->cspot_context_ && this->cspot_context_->session) {
    this->cspot_context_->session->closeConnection();
  }
}

bool SpotifyConnectComponent::are_tasks_exited_impl() {
  return this->pkt_task_exited_.load(std::memory_order_acquire) &&
         this->play_task_exited_.load(std::memory_order_acquire);
}

void SpotifyConnectComponent::finalize_cleanup_impl() {
  ESP_LOGI(TAG, "finalize_cleanup: BEGIN (internal=%u PSRAM=%u)",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (this->packet_task_handle_) {
    vTaskDelete(this->packet_task_handle_);
    this->packet_task_handle_ = nullptr;
  }
  if (this->playback_task_handle_) {
    vTaskDelete(this->playback_task_handle_);
    this->playback_task_handle_ = nullptr;
  }

  this->spirc_handler_.reset();
  this->cspot_context_.reset();
  this->audio_buffer_.reset();
  this->last_auth_username_.clear();

  if (this->pkt_task_stack_)  { heap_caps_free(this->pkt_task_stack_);  this->pkt_task_stack_ = nullptr; }
  if (this->pkt_task_buf_)    { heap_caps_free(this->pkt_task_buf_);    this->pkt_task_buf_ = nullptr; }
  if (this->play_task_stack_) { heap_caps_free(this->play_task_stack_); this->play_task_stack_ = nullptr; }
  if (this->play_task_buf_)   { heap_caps_free(this->play_task_buf_);   this->play_task_buf_ = nullptr; }

  this->pending_shutdown_ = false;
  this->disconnect_called_ = false;
  this->idle_since_ms_ = 0;
  this->mute_state_ = true;
  this->cspot_format_set_ = false;
  this->pending_disc_ = false;
  this->disc_hold_until_ms_ = 0;

  esphome::snapspot::cspot_shutdown_pending = false;
  esphome::snapspot::cspot_tasks_exited = true;
  esphome::snapspot::cspot_active = false;
  esphome::snapspot::cspot_fully_stopped = true;

  ESP_LOGI(TAG, "finalize_cleanup: DONE (internal=%u PSRAM=%u)",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void SpotifyConnectComponent::force_cleanup_impl() {
  ESP_LOGE(TAG, "force_cleanup: tasks didn't exit in time, force-killing");
  if (this->packet_task_handle_) {
    vTaskDelete(this->packet_task_handle_);
    this->packet_task_handle_ = nullptr;
  }
  if (this->playback_task_handle_) {
    vTaskDelete(this->playback_task_handle_);
    this->playback_task_handle_ = nullptr;
  }
  vTaskDelay(pdMS_TO_TICKS(100));
  this->finalize_cleanup_impl();
}

void SpotifyConnectComponent::loop() {
  if (!this->network_initialized_ && network::is_connected()) {
    this->network_initialized_ = true;
    this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    this->publish_state();
  }

  if (this->network_initialized_ && !this->mdns_registered_) {
    int64_t mdns_t0 = esp_timer_get_time();
    mdns_service_add(this->device_name_.c_str(), "_spotify-connect", "_tcp", 8080, nullptr, 0);
    mdns_service_txt_item_set("_spotify-connect", "_tcp", "VERSION", "1.0");
    mdns_service_txt_item_set("_spotify-connect", "_tcp", "CPath", "/spotify_info");
    mdns_service_txt_item_set("_spotify-connect", "_tcp", "Stack", "SP");
    int64_t mdns_ms = (esp_timer_get_time() - mdns_t0) / 1000;
    this->mdns_registered_ = true;
    ESP_LOGI(TAG, "mDNS _spotify-connect._tcp registered (port 8080) %lldms", (long long)mdns_ms);
  }

  // === AUTO-RECONNECT: if network is up and we have saved creds, auto-authenticate ===
  if (this->network_initialized_ && this->mdns_registered_ &&
      !this->auth_in_progress_ && !this->spirc_handler_ &&
      !this->auto_reconnect_started_ && !this->pending_shutdown_ &&
      !this->auto_reconnect_nvs_checked_) {
    this->auto_reconnect_nvs_checked_ = true;
    std::string saved = this->nvs_load_cred_();
    if (!saved.empty()) {
      this->auto_reconnect_delay_ms_ = millis() + 5000; // 5s delay after network up
      ESP_LOGI(TAG, "Auto-reconnect: found saved credentials in NVS (%u bytes), will reconnect in 5s",
               (unsigned)saved.size());
    } else {
      ESP_LOGI(TAG, "Auto-reconnect: no saved credentials in NVS — waiting for first Zeroconf login");
    }
  }

  // Check if it's time to start auto-reconnect
  if (this->auto_reconnect_nvs_checked_ &&
      this->auto_reconnect_delay_ms_ > 0 &&
      !this->auto_reconnect_started_ &&
      !this->auth_in_progress_ && !this->spirc_handler_ &&
      !this->pending_shutdown_ &&
      millis() >= this->auto_reconnect_delay_ms_) {
    ESP_LOGI(TAG, "Auto-reconnect: starting automatic authentication from saved credentials");
    this->auto_reconnect_started_ = true;
    this->start_auto_reconnect_();
  }

  // === WEB API POLLING: fetch "now playing" from Spotify Web API when we're NOT the active device ===
  this->web_api_poll_();

  // === AUTO-TRANSFER: after Spotify Connect is ready, try to transfer playback here ===
  this->try_auto_transfer_();

  if (this->meta_q_hdl_) {
    SpotifyMetaUpdate meta_upd{};
    if (xQueueReceive(this->meta_q_hdl_, &meta_upd, 0) == pdTRUE) {
      if (this->track_name_sensor_) this->track_name_sensor_->publish_state(meta_upd.title);
      if (this->artist_sensor_)     this->artist_sensor_->publish_state(meta_upd.artist);
      if (this->album_sensor_)      this->album_sensor_->publish_state(meta_upd.album);
      if (this->album_art_url_sensor_) this->album_art_url_sensor_->publish_state(meta_upd.image_url);
    }
  }

  if (this->pending_disc_ && (millis() - this->pending_disc_at_ms_ >= 500)) {
    this->pending_disc_ = false;
    ESP_LOGI(TAG, "Deferred DISC: executing — requesting session shutdown");
    this->disc_hold_until_ms_ = 0;
    this->draining_ = false;
    this->is_playing_ = false;
    if (this->audio_buffer_) this->audio_buffer_->emptyBuffer();
    if (this->source_) this->source_->stop();
    this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    this->current_track_.clear();
    this->current_artist_.clear();
    this->current_album_.clear();
    this->current_image_url_.clear();
    this->current_duration_ms_ = 0;
    this->media_position_ms_ = 0;
    if (this->track_name_sensor_) this->track_name_sensor_->publish_state("");
    if (this->artist_sensor_)     this->artist_sensor_->publish_state("");
    if (this->album_sensor_)      this->album_sensor_->publish_state("");
    if (this->album_art_url_sensor_) this->album_art_url_sensor_->publish_state("");
    if (this->duration_sensor_) this->duration_sensor_->publish_state(0.0f);
    if (this->position_sensor_) this->position_sensor_->publish_state(0.0f);
    this->mute_state_ = true;
    this->publish_state();
    this->pending_shutdown_ = true;
    esphome::snapspot::cspot_shutdown_pending = true;
  }

  if (this->spirc_handler_ && !this->pending_shutdown_) {
    if (this->state == media_player::MEDIA_PLAYER_STATE_IDLE && !this->is_playing_) {
      if (this->idle_since_ms_ == 0) {
        this->idle_since_ms_ = millis();
      }
      // DISABLED: Idle timeout was killing the Spotify Connect session after 60s
      // of no playback, making the device disappear from Spotify's device list.
      // The session should stay alive indefinitely so the user can select it anytime.
    } else {
      this->idle_since_ms_ = 0;
    }
  }

  if (this->state == media_player::MEDIA_PLAYER_STATE_PLAYING && this->current_duration_ms_ > 0) {
    uint32_t now = millis();
    uint32_t delta = now - this->media_position_updated_at_;
    this->media_position_ms_ += delta;
    this->media_position_updated_at_ = now;
    if (this->media_position_ms_ > this->current_duration_ms_)
      this->media_position_ms_ = this->current_duration_ms_;
    if (this->position_sensor_ && (now - this->last_position_publish_ms_ > 5000)) {
      this->position_sensor_->publish_state(static_cast<float>(this->media_position_ms_) / 1000.0f);
      this->last_position_publish_ms_ = now;
    }
  }
  if (this->pending_nvs_volume_.load(std::memory_order_acquire)) {
    this->pending_nvs_volume_.store(false, std::memory_order_relaxed);
    nvs_save_float(NVS_KEY_VOLUME, this->pending_nvs_volume_val_.load(std::memory_order_relaxed));
  }
}

void SpotifyConnectComponent::control(const media_player::MediaPlayerCall &call) {
  if (this->state == media_player::MEDIA_PLAYER_STATE_OFF ||
      this->state == media_player::MEDIA_PLAYER_STATE_NONE) {
    ESP_LOGW(TAG, "Player not ready. Ignoring control command.");
    return;
  }

  if (call.get_volume().has_value()) {
    this->set_volume_(call.get_volume().value());
  }

  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->set_mute_(true);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->set_mute_(false);
        break;
      default:
        break;
    }

    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (this->spirc_handler_) this->spirc_handler_->setPause(false);
        this->is_playing_ = true;
        this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
        this->publish_state();
        break;

      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        if (this->spirc_handler_) this->spirc_handler_->setPause(true);
        this->is_playing_ = false;
        this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        this->publish_state();
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->is_playing_) {
          if (this->spirc_handler_) this->spirc_handler_->setPause(true);
          this->is_playing_ = false;
          this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        } else {
          if (this->spirc_handler_) this->spirc_handler_->setPause(false);
          this->is_playing_ = true;
          this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
        }
        this->publish_state();
        break;

      case media_player::MEDIA_PLAYER_COMMAND_NEXT:
        if (this->spirc_handler_) {
          ESP_LOGI(TAG, "NEXT: calling spirc_handler_->nextSong()");
          this->spirc_handler_->nextSong();
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_PREVIOUS:
        if (this->spirc_handler_) {
          ESP_LOGI(TAG, "PREV: calling spirc_handler_->previousSong()");
          this->spirc_handler_->previousSong();
        }
        break;

      default:
        break;
    }
  }
}

void SpotifyConnectComponent::set_mute_(bool mute) {
  this->mute_state_ = mute;
#ifdef USE_SHARED_AUDIO_EQ
  if (mute) {
    eq_set_source_gain(EQ_SOURCE_CSPOT, 0.0f);
  } else {
    this->set_volume_(this->volume_, false);
  }
#endif
  this->publish_state();
  ESP_LOGD(TAG, "%s", mute ? "Mute" : "Unmute");
}

void SpotifyConnectComponent::set_volume_(float volume, bool publish) {
  volume = std::max(0.0f, std::min(volume, 1.0f));
  this->volume_ = volume;
  this->volume = volume;

  const float scaled = this->min_volume_pct_ +
                       volume * (this->max_volume_pct_ - this->min_volume_pct_);
  const float db = this->spotify_min_db_ +
                   scaled * (this->spotify_max_db_ - this->spotify_min_db_);
  const float linear = powf(10.0f, db / 20.0f);
  const float linear_max = powf(10.0f, this->spotify_max_db_ / 20.0f);
  const float gain = (linear_max > 0.0f) ? (linear / linear_max) : 0.0f;

#ifdef USE_SHARED_AUDIO_EQ
  eq_set_source_gain(EQ_SOURCE_CSPOT, gain);
  ESP_LOGD(TAG, "set_volume_: %.2f (eq gain %.3f, dB %.1f)", volume, gain, db);
#else
  ESP_LOGD(TAG, "set_volume_: %.2f (no EQ, gain %.3f)", volume, gain);
#endif

  if (this->spirc_handler_) {
    uint32_t now = millis();
    if (now - this->last_server_volume_ms_ > 2000) {
      this->last_ha_volume_ms_ = now;
      uint32_t vol_raw = static_cast<uint32_t>(volume * 65535.0f);
      this->spirc_handler_->setRemoteVolume(static_cast<int>(vol_raw));
    }
  }

  this->pending_nvs_volume_val_.store(volume, std::memory_order_relaxed);
  this->pending_nvs_volume_.store(true, std::memory_order_release);

  if (publish) {
    this->publish_state();
  }
}

media_player::MediaPlayerTraits SpotifyConnectComponent::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(true);
  traits.add_feature_flags(media_player::MediaPlayerEntityFeature::NEXT_TRACK |
                           media_player::MediaPlayerEntityFeature::PREVIOUS_TRACK);
  return traits;
}

// === AUTO-RECONNECT: save/load/clear credentials in NVS ===
std::string SpotifyConnectComponent::nvs_load_cred_() {
  return nvs_load_string_blob(NVS_KEY_CRED);
}

void SpotifyConnectComponent::nvs_save_cred_(const std::string &json) {
  nvs_save_string_blob(NVS_KEY_CRED, json);
}

void SpotifyConnectComponent::nvs_clear_cred_() {
  nvs_erase_cred(NVS_KEY_CRED);
  ESP_LOGI(TAG, "Auto-reconnect: cleared saved credentials from NVS");
}

void SpotifyConnectComponent::start_auto_reconnect_() {
  if (this->auth_in_progress_) {
    ESP_LOGW(TAG, "Auto-reconnect: auth already in progress, skipping");
    return;
  }

  this->auto_reconnect_from_nvs_ = true;
  this->auth_in_progress_ = true;

  // Clear any pending zeroconf params — we are using NVS credentials
  this->pending_auth_params_.clear();

  if (this->auth_task_stack_) {
    heap_caps_free(this->auth_task_stack_);
    this->auth_task_stack_ = nullptr;
  }
  if (this->auth_task_buf_) {
    heap_caps_free(this->auth_task_buf_);
    this->auth_task_buf_ = nullptr;
  }

  this->auth_task_buf_ = static_cast<StaticTask_t *>(
    heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  this->auth_task_stack_ = static_cast<StackType_t *>(
    heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!this->auth_task_buf_ || !this->auth_task_stack_) {
    heap_caps_free(this->auth_task_buf_);
    this->auth_task_buf_ = nullptr;
    heap_caps_free(this->auth_task_stack_);
    this->auth_task_stack_ = nullptr;
    this->auth_in_progress_ = false;
    this->auto_reconnect_from_nvs_ = false;
    ESP_LOGE(TAG, "Auto-reconnect: failed to allocate auth task memory");
    return;
  }
  xTaskCreateStaticPinnedToCore(
    auth_task_wrapper_, "spotify_auth", 16384, this, 5,
    this->auth_task_stack_, this->auth_task_buf_, 0);
}

// ============================================================
// === WEB API POLLING: "now playing" when ESP32 is NOT the active output
// ============================================================

std::string SpotifyConnectComponent::get_web_api_token_() {
  if (!this->cspot_context_) {
    ESP_LOGW(TAG, "Web API: no cspot context");
    return "";
  }
  if (!this->web_api_key_fetcher_) {
    this->web_api_key_fetcher_ = std::make_shared<cspot::AccessKeyFetcher>(this->cspot_context_);
  }
  return this->web_api_key_fetcher_->getAccessKey();
}

void SpotifyConnectComponent::web_api_task_wrapper_(void *param) {
  auto *self = static_cast<SpotifyConnectComponent *>(param);
  self->run_web_api_poll_();
  self->web_api_task_running_ = false;
  self->web_api_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void SpotifyConnectComponent::run_web_api_poll_() {
  std::string token = this->get_web_api_token_();
  if (token.empty()) {
    ESP_LOGW(TAG, "Web API: failed to get access token");
    return;
  }

  ESP_LOGD(TAG, "Web API: fetching /v1/me/player ...");

  auto response = bell::HTTPClient::get(
    "https://api.spotify.com/v1/me/player",
    {
      {"Authorization", "Bearer " + token},
      {"Accept", "application/json"}
    }
  );

  if (!response) {
    ESP_LOGW(TAG, "Web API: null response");
    return;
  }

  int status = response->statusCode();
  if (status == 204) {
    // Not playing anything
    ESP_LOGD(TAG, "Web API: 204 (no playback)");
    if (this->web_api_has_data_) {
      this->web_api_has_data_ = false;
      this->web_api_is_playing_ = false;
      this->web_api_track_.clear();
      this->web_api_artist_.clear();
      this->web_api_album_.clear();
      this->web_api_image_url_.clear();
      this->web_api_duration_ms_ = 0;
      this->web_api_position_ms_ = 0;
      this->web_api_position_updated_at_ = millis();
      ESP_LOGD(TAG, "Web API: playback stopped, clearing sensors");
    }
    return;
  }

  if (status == 429) {
    // Rate limited — check Retry-After header, default 30s
    std::string_view ra = response->header("Retry-After");
    uint32_t wait = 30;
    if (!ra.empty()) {
      try { wait = std::stoul(std::string(ra)); } catch (...) {}
    }
    this->web_api_retry_after_ms_ = millis() + wait * 1000;
    ESP_LOGW(TAG, "Web API: 429 rate limited, retry after %us", wait);
    return;
  }

  if (status != 200) {
    ESP_LOGW(TAG, "Web API: HTTP %d", status);
    return;
  }

  std::string body = std::string(response->body());
  if (body.empty()) {
    ESP_LOGW(TAG, "Web API: empty body");
    return;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    ESP_LOGW(TAG, "Web API: JSON parse failed (%d chars)", (int)body.size());
    return;
  }

  // is_playing
  cJSON *j_playing = cJSON_GetObjectItemCaseSensitive(root, "is_playing");
  bool playing = j_playing && cJSON_IsTrue(j_playing);

  // progress_ms
  cJSON *j_progress = cJSON_GetObjectItemCaseSensitive(root, "progress_ms");
  uint32_t progress = j_progress && cJSON_IsNumber(j_progress) ? (uint32_t)j_progress->valuedouble : 0;

  // item
  cJSON *j_item = cJSON_GetObjectItemCaseSensitive(root, "item");
  if (j_item) {
    // track name
    cJSON *j_name = cJSON_GetObjectItemCaseSensitive(j_item, "name");
    std::string name = j_name && cJSON_IsString(j_name) ? j_name->valuestring : "";

    // duration_ms
    cJSON *j_dur = cJSON_GetObjectItemCaseSensitive(j_item, "duration_ms");
    uint32_t duration = j_dur && cJSON_IsNumber(j_dur) ? (uint32_t)j_dur->valuedouble : 0;

    // album
    cJSON *j_album = cJSON_GetObjectItemCaseSensitive(j_item, "album");
    std::string album_name, image_url;
    if (j_album) {
      cJSON *j_album_name = cJSON_GetObjectItemCaseSensitive(j_album, "name");
      album_name = j_album_name && cJSON_IsString(j_album_name) ? j_album_name->valuestring : "";

      // images array - take first (largest)
      cJSON *j_images = cJSON_GetObjectItemCaseSensitive(j_album, "images");
      if (j_images && cJSON_IsArray(j_images)) {
        cJSON *j_img = cJSON_GetArrayItem(j_images, 0);
        if (j_img) {
          cJSON *j_url = cJSON_GetObjectItemCaseSensitive(j_img, "url");
          image_url = j_url && cJSON_IsString(j_url) ? j_url->valuestring : "";
        }
      }
    }

    // artists array
    std::string artist_name;
    cJSON *j_artists = cJSON_GetObjectItemCaseSensitive(j_item, "artists");
    if (j_artists && cJSON_IsArray(j_artists)) {
      cJSON *j_artist = cJSON_GetArrayItem(j_artists, 0);
      if (j_artist) {
        cJSON *j_artist_name = cJSON_GetObjectItemCaseSensitive(j_artist, "name");
        artist_name = j_artist_name && cJSON_IsString(j_artist_name) ? j_artist_name->valuestring : "";
      }
    }

    // Update state
    bool changed = (name != this->web_api_track_) || (artist_name != this->web_api_artist_);

    this->web_api_track_ = name;
    this->web_api_artist_ = artist_name;
    this->web_api_album_ = album_name;
    this->web_api_image_url_ = image_url;
    this->web_api_duration_ms_ = duration;
    this->web_api_position_ms_ = progress;
    this->web_api_position_updated_at_ = millis();
    this->web_api_is_playing_ = playing;
    this->web_api_has_data_ = true;
    this->web_api_last_success_ms_ = millis();

    ESP_LOGI(TAG, "Web API: %s — %s (%s) [%u/%u ms] playing=%d",
             name.c_str(), artist_name.c_str(), album_name.c_str(),
             progress, duration, playing ? 1 : 0);

    // Publish to sensors ONLY if we're not the active device
    // (when we ARE the active device, SPIRC events handle the sensors)
    if (!this->is_playing_) {
      if (changed || this->meta_q_hdl_) {
        SpotifyMetaUpdate upd{};
        strncpy(upd.title,     name.c_str(),      sizeof(upd.title)     - 1);
        strncpy(upd.artist,    artist_name.c_str(),sizeof(upd.artist)    - 1);
        strncpy(upd.album,     album_name.c_str(), sizeof(upd.album)     - 1);
        strncpy(upd.image_url, image_url.c_str(),  sizeof(upd.image_url) - 1);
        xQueueOverwrite(this->meta_q_hdl_, &upd);
      }
      if (this->duration_sensor_)
        this->duration_sensor_->publish_state(static_cast<float>(duration) / 1000.0f);
      if (this->position_sensor_)
        this->position_sensor_->publish_state(static_cast<float>(progress) / 1000.0f);
    }
    if (this->is_playing_sensor_)
      this->is_playing_sensor_->publish_state(playing);
  }

  cJSON_Delete(root);
}

void SpotifyConnectComponent::web_api_poll_() {
  if (!this->cspot_context_ || !this->spirc_handler_) {
    return; // Not authenticated yet
  }

  // Only poll when we're NOT actively playing (SPIRC handles active playback)
  if (this->is_playing_) {
    return;
  }

  // Only poll if we have network
  if (!this->network_initialized_) {
    return;
  }

  // Rate limit: every 60 seconds, plus respect Retry-After from 429
  uint32_t now = millis();
  // Don't poll for first 10s after boot (let connection stabilize)
  if (now < 10000) return;
  // DISABLED: Web API polling causes 429 rate limiting and token conflicts
  // with cspot's internal AccessKeyFetcher. Display works via SPIRC when
  // ESP32 is the active Spotify Connect output.
  return;
  if (this->web_api_retry_after_ms_ && now < this->web_api_retry_after_ms_) {
    return; // Still in rate-limit cooldown
  }
  if (now - this->web_api_last_poll_ms_ < 60000) {
    return;
  }
  this->web_api_last_poll_ms_ = now;

  // Don't start a new poll if one is running
  if (this->web_api_task_running_) {
    return;
  }

  this->web_api_task_running_ = true;
  xTaskCreate(
    web_api_task_wrapper_, "web_api_poll", 16384, this, 3,
    &this->web_api_task_handle_);
}

// ============================================================
// === AUTO-TRANSFER: transfer playback to this ESP32 at boot
// ============================================================
// After Spotify Connect is ready (Hello sent, session subscribed),
// we call the Spotify Web API "PUT /v1/me/player" with our deviceId
// to transfer the currently playing track to this device.
// This makes the ESP32 the active output — just like pressing
// "Spotify Station" in the phone's device picker, but automatic.
// Unlike the disabled Web API polling, this is a SINGLE call at
// boot, so it won't trigger 429 rate limiting.

void SpotifyConnectComponent::try_auto_transfer_() {
  // Only try once per boot
  if (this->auto_transfer_tried_) return;

  // Need an active Spotify Connect session (spirc_handler_ set, not shutting down)
  if (!this->spirc_handler_ || this->pending_shutdown_) return;
  if (!this->cspot_context_) return;

  // Wait 15s after Spotify Connect ready before transferring
  // (let mDNS propagate, let phone app see the device)
  uint32_t now = millis();
  if (this->auto_transfer_at_ms_ == 0) {
    this->auto_transfer_at_ms_ = now + 15000;
    ESP_LOGI(TAG, "Auto-transfer: will attempt in 15s (waiting for mDNS propagation)");
    return;
  }
  if (now < this->auto_transfer_at_ms_) return;

  // Already playing? No need to transfer
  if (this->is_playing_) {
    this->auto_transfer_tried_ = true;
    return;
  }

  this->auto_transfer_tried_ = true;

  // Get access token from cspot's own AccessKeyFetcher (login5 token)
  std::string token = this->get_web_api_token_();
  if (token.empty()) {
    ESP_LOGW(TAG, "Auto-transfer: no access token, skipping");
    return;
  }

  // Get our Spotify deviceId
  std::string device_id = this->cspot_context_->config.deviceId;

  // Build JSON body: {"device_ids": ["<our_device_id>"], "play": true}
  std::string body = "{\"device_ids\":[\"" + device_id + "\"],\"play\":true}";

  ESP_LOGI(TAG, "Auto-transfer: PUT /v1/me/player device_id=%s", device_id.c_str());

  // Use bell::HTTPClient::put() static method — same pattern as AccessKeyFetcher::post()
  // This avoids the rawRequest() header/impl arg-order mismatch bug
  try {
    bell::HTTPClient::Headers headers = {
      {"Authorization", "Bearer " + token},
      {"Content-Type", "application/json"}
    };
    std::vector<uint8_t> body_bytes(body.begin(), body.end());
    auto response = bell::HTTPClient::put(
      "https://api.spotify.com/v1/me/player", headers, body_bytes);

    int status = response->statusCode();
    if (status == 204 || status == 202) {
      ESP_LOGI(TAG, "Auto-transfer: SUCCESS! Playback transferred to this device (%d)", status);
    } else {
      ESP_LOGW(TAG, "Auto-transfer: HTTP %d (expected 204 or 202)", status);
    }
  } catch (const std::exception &e) {
    ESP_LOGW(TAG, "Auto-transfer: exception: %s", e.what());
  }
}

}

#endif
#endif
