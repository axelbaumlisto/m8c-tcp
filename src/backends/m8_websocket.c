// Copyright 2026 M8 WebSocket Backend
// WebSocket backend for connecting to m8-display server
// Uses separate channels for display (SLIP) and audio (PCM)
// Released under the MIT licence

#ifdef USE_WEBSOCKET
#include <SDL3/SDL.h>
#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "../command.h"
#include "../config.h"
#include "audio.h"
#include "m8.h"
#include "queue.h"
#include "slip.h"

#define WS_READ_SIZE 4096
#define WS_READ_DELAY_MS 4

// WebSocket connection state
static struct lws_context *ws_context = NULL;
static struct lws *ws_display = NULL;
static struct lws *ws_audio = NULL;
static struct lws *ws_control = NULL;
static int ws_audio_initialized = 0;

// Bitmask to key name mapping (matches m8-display server)
static const char *bitmask_to_key[] = {
  "edit",   // bit 0 = 0x01
  "opt",    // bit 1 = 0x02
  "right",  // bit 2 = 0x04
  "start",  // bit 3 = 0x08
  "shift",  // bit 4 = 0x10
  "down",   // bit 5 = 0x20
  "up",     // bit 6 = 0x40
  "left",   // bit 7 = 0x80
};

// Buffers
static uint8_t slip_buffer[WS_READ_SIZE] = {0};
static slip_handler_s slip;
message_queue_s queue;

// Thread
SDL_Thread *ws_thread = NULL;

typedef struct {
  int should_stop;
} thread_params_s;

static thread_params_s thread_params;

// Server address from config
static char server_host[256] = "127.0.0.1";
static int server_port = 8080;

// Protocol names
#define PROTO_DISPLAY "display"
#define PROTO_AUDIO "audio"
#define PROTO_CONTROL "control"

// Pending write buffer for control messages
static unsigned char control_write_buf[LWS_PRE + 256];
static size_t control_write_len = 0;
static int control_write_pending = 0;

static int send_message_to_queue(uint8_t *data, const uint32_t size) {
  push_message(&queue, data, size);
  return 1;
}

// WebSocket callback for /display endpoint
static int callback_display(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
  (void)user;

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    SDL_Log("WebSocket /display connected");
    ws_display = wsi;
    break;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    // Process raw SLIP data - no need for boundary detection!
    if (in && len > 0) {
      const uint8_t *data = (const uint8_t *)in;
      for (size_t i = 0; i < len; i++) {
        int slip_result = slip_read_byte(&slip, data[i]);
        if (slip_result != SLIP_NO_ERROR) {
          SDL_LogError(SDL_LOG_CATEGORY_ERROR, "SLIP error %d", slip_result);
        }
      }
    }
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "WebSocket /display connection error: %s",
                 in ? (char *)in : "unknown");
    ws_display = NULL;
    break;

  case LWS_CALLBACK_CLOSED:
    SDL_Log("WebSocket /display closed");
    ws_display = NULL;
    break;

  default:
    break;
  }

  return 0;
}

// WebSocket callback for /audio endpoint
static int callback_audio(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len) {
  (void)user;

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    SDL_Log("WebSocket /audio connected");
    ws_audio = wsi;
    break;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    // Audio data has 1-byte prefix: 0x00 = audio, 0x01 = control
    // Skip the prefix byte and feed only PCM data
    if (in && len > 1) {
      audio_feed_tcp((const uint8_t *)in + 1, len - 1);
    }
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "WebSocket /audio connection error: %s",
                 in ? (char *)in : "unknown");
    ws_audio = NULL;
    break;

  case LWS_CALLBACK_CLOSED:
    SDL_Log("WebSocket /audio closed");
    ws_audio = NULL;
    break;

  default:
    break;
  }

  return 0;
}

// WebSocket callback for /control endpoint
static int callback_control(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
  (void)user;
  (void)in;
  (void)len;

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    SDL_Log("WebSocket /control connected");
    ws_control = wsi;
    break;

  case LWS_CALLBACK_CLIENT_WRITEABLE:
    // Send pending control message
    if (control_write_pending && control_write_len > 0) {
      lws_write(wsi, &control_write_buf[LWS_PRE], control_write_len,
                LWS_WRITE_TEXT);
      control_write_pending = 0;
      control_write_len = 0;
    }
    break;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    // Server may send acknowledgments - ignore them
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "WebSocket /control connection error: %s",
                 in ? (char *)in : "unknown");
    ws_control = NULL;
    break;

  case LWS_CALLBACK_CLOSED:
    SDL_Log("WebSocket /control closed");
    ws_control = NULL;
    break;

  default:
    break;
  }

  return 0;
}

// Protocol definitions - different struct sizes for different LWS versions
// LWS 2.x: 4 fields (name, callback, per_session_data_size, rx_buffer_size)
// LWS 4.x: 7 fields (adds id, user, tx_packet_size)
#if LWS_LIBRARY_VERSION_NUMBER >= 4000000
static struct lws_protocols protocols[] = {
    {PROTO_DISPLAY, callback_display, 0, WS_READ_SIZE, 0, NULL, 0},
    {PROTO_AUDIO, callback_audio, 0, WS_READ_SIZE, 0, NULL, 0},
    {PROTO_CONTROL, callback_control, 0, WS_READ_SIZE, 0, NULL, 0},
    {NULL, NULL, 0, 0, 0, NULL, 0}
};
#else
static struct lws_protocols protocols[] = {
    {PROTO_DISPLAY, callback_display, 0, WS_READ_SIZE},
    {PROTO_AUDIO, callback_audio, 0, WS_READ_SIZE},
    {PROTO_CONTROL, callback_control, 0, WS_READ_SIZE},
    {NULL, NULL, 0, 0}
};
#endif

static int disconnect() {
  SDL_Log("Disconnecting WebSocket");

  thread_params.should_stop = 1;
  if (ws_thread) {
    SDL_WaitThread(ws_thread, NULL);
    ws_thread = NULL;
  }
  destroy_queue(&queue);

  if (ws_context) {
    lws_context_destroy(ws_context);
    ws_context = NULL;
    ws_display = NULL;
    ws_audio = NULL;
  }

  // Close audio
  if (ws_audio_initialized) {
    audio_close();
    ws_audio_initialized = 0;
  }

  return 1;
}

// Parse server address from string (host:port or just host)
static void parse_server_address(const char *addr) {
  if (addr == NULL || strlen(addr) == 0) {
    return;
  }

  // Skip ws:// or http:// prefix if present
  const char *start = addr;
  if (strncmp(addr, "ws://", 5) == 0) {
    start = addr + 5;
  } else if (strncmp(addr, "http://", 7) == 0) {
    start = addr + 7;
  }

  // Check for port separator
  const char *colon = strrchr(start, ':');
  if (colon) {
    // Has port
    size_t host_len = colon - start;
    if (host_len > 0 && host_len < sizeof(server_host)) {
      strncpy(server_host, start, host_len);
      server_host[host_len] = '\0';
    }
    server_port = atoi(colon + 1);
    if (server_port <= 0 || server_port > 65535) {
      server_port = 8080;
    }
  } else {
    // Just hostname
    strncpy(server_host, start, sizeof(server_host) - 1);
    server_host[sizeof(server_host) - 1] = '\0';
  }
}

// Connect to WebSocket endpoint
static struct lws *connect_ws(const char *path, const char *protocol) {
  struct lws_client_connect_info ccinfo = {0};

  ccinfo.context = ws_context;
  ccinfo.address = server_host;
  ccinfo.port = server_port;
  ccinfo.path = path;
  ccinfo.host = server_host;
  ccinfo.origin = server_host;
  ccinfo.protocol = protocol;
  ccinfo.ssl_connection = 0; // No SSL for local connections

  struct lws *wsi = lws_client_connect_via_info(&ccinfo);
  if (!wsi) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to connect to %s:%d%s",
                 server_host, server_port, path);
  }
  return wsi;
}

static int thread_process_ws_data(void *data) {
  const thread_params_s *params = data;

  while (!params->should_stop) {
    if (!ws_context) {
      SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "WebSocket context destroyed");
      return 0;
    }

    // Service WebSocket events (non-blocking)
    int result = lws_service(ws_context, 0);
    if (result < 0) {
      SDL_LogError(SDL_LOG_CATEGORY_ERROR, "lws_service error: %d", result);
      return 0;
    }

    SDL_Delay(WS_READ_DELAY_MS);
  }
  return 1;
}

int m8_initialize(const int verbose, const char *preferred_device) {
  if (ws_context) {
    return 1; // Already connected
  }

  // Initialize SLIP decoder
  static const slip_descriptor_s slip_descriptor = {
      .buf = slip_buffer,
      .buf_size = sizeof(slip_buffer),
      .recv_message = send_message_to_queue,
  };
  slip_init(&slip, &slip_descriptor);

  // Parse server address from preferred_device
  // Format: ws://host:port or host:port or just host (default port 8080)
  if (preferred_device != NULL) {
    parse_server_address(preferred_device);
  }

  if (verbose) {
    SDL_Log("Connecting to m8-display WebSocket server at %s:%d", server_host,
            server_port);
  }

  // Create libwebsockets context
  struct lws_context_creation_info info = {0};
  info.port = CONTEXT_PORT_NO_LISTEN; // Client only
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

  ws_context = lws_create_context(&info);
  if (!ws_context) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to create WebSocket context");
    return 0;
  }

  // Initialize queue
  init_queue(&queue);
  thread_params.should_stop = 0;

  // Connect to /display endpoint
  if (!connect_ws("/display", PROTO_DISPLAY)) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to connect to /display");
    lws_context_destroy(ws_context);
    ws_context = NULL;
    return 0;
  }

  // Initialize audio and connect to /audio endpoint
  if (!ws_audio_initialized) {
    if (audio_init_tcp(NULL, 0)) {
      ws_audio_initialized = 1;
      connect_ws("/audio", PROTO_AUDIO);
      SDL_Log("WebSocket Audio streaming enabled");
    } else {
      SDL_Log("WebSocket Audio not available (will display only)");
    }
  }

  // Connect to /control endpoint for input
  if (!connect_ws("/control", PROTO_CONTROL)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Control WebSocket not connected - input will not work");
  } else {
    SDL_Log("WebSocket Control enabled");
  }

  // Start processing thread
  ws_thread =
      SDL_CreateThread(thread_process_ws_data, "WsThread", &thread_params);

  if (!ws_thread) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "SDL_CreateThread Error: %s",
                 SDL_GetError());
    disconnect();
    return 0;
  }

  // Wait for connection to establish
  for (int i = 0; i < 100 && !ws_display; i++) {
    lws_service(ws_context, 10);
    SDL_Delay(10);
  }

  if (!ws_display) {
    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "WebSocket /display failed to connect");
    disconnect();
    return 0;
  }

  SDL_Log("Connected to WebSocket server %s:%d", server_host, server_port);
  return 1;
}

int m8_list_devices(void) {
  SDL_Log("WebSocket mode: Use --dev host:port to specify server address");
  SDL_Log("Example: --dev 192.168.1.100:8080");
  SDL_Log("Example: --dev ws://192.168.1.100:8080");
  SDL_Log("Default: 127.0.0.1:8080");
  return 0;
}

// Send controller input via WebSocket (JSON message)
int m8_send_msg_controller(const uint8_t input) {
  if (!ws_control)
    return -1;

  // Build JSON message based on pressed keys
  char json[128];
  int keys_pressed = 0;
  int first_key = -1, second_key = -1;

  // Count pressed keys and track first two
  for (int i = 0; i < 8; i++) {
    if (input & (1 << i)) {
      if (first_key < 0)
        first_key = i;
      else if (second_key < 0)
        second_key = i;
      keys_pressed++;
    }
  }

  if (keys_pressed == 0) {
    // No keys pressed - nothing to send
    return 1;
  } else if (keys_pressed == 1) {
    // Single key: {"type":"key","key":"up"}
    snprintf(json, sizeof(json), "{\"type\":\"key\",\"key\":\"%s\"}",
             bitmask_to_key[first_key]);
  } else {
    // Combo: {"type":"keys","hold":"shift","press":"down"}
    snprintf(json, sizeof(json),
             "{\"type\":\"keys\",\"hold\":\"%s\",\"press\":\"%s\"}",
             bitmask_to_key[first_key], bitmask_to_key[second_key]);
  }

  // Queue message for sending
  control_write_len = strlen(json);
  memcpy(&control_write_buf[LWS_PRE], json, control_write_len);
  control_write_pending = 1;

  // Request write callback
  lws_callback_on_writable(ws_control);

  return 1;
}

int m8_send_msg_keyjazz(const uint8_t note, uint8_t velocity) {
  if (!ws_control)
    return -1;

  char json[64];

  if (velocity > 0) {
    // Note on: {"type":"note","note":60,"vel":100}
    snprintf(json, sizeof(json), "{\"type\":\"note\",\"note\":%d,\"vel\":%d}",
             note, velocity);
  } else {
    // Note off: {"type":"noteOff"}
    snprintf(json, sizeof(json), "{\"type\":\"noteOff\"}");
  }

  // Queue message for sending
  control_write_len = strlen(json);
  memcpy(&control_write_buf[LWS_PRE], json, control_write_len);
  control_write_pending = 1;

  // Request write callback
  lws_callback_on_writable(ws_control);

  return 1;
}

// Simple HTTP POST to /api/reset (no external libs needed)
static int http_post_reset(void) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return 0;

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server_port);
  inet_pton(AF_INET, server_host, &addr.sin_addr);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return 0;
  }

  const char *request = "POST /api/reset HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
  send(sock, request, strlen(request), 0);

  // Read response (just drain it)
  char buf[256];
  recv(sock, buf, sizeof(buf), 0);
  close(sock);

  return 1;
}

int m8_reset_display() {
  if (!ws_display)
    return 0;

  SDL_Log("Reset display (WebSocket) - sending HTTP reset");
  if (http_post_reset()) {
    SDL_Log("Display reset sent successfully");
  } else {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to send reset");
  }
  return 1;
}

int m8_enable_display(const unsigned char reset_display) {
  if (!ws_display)
    return 0;

  SDL_Log("Enabling M8 display (WebSocket)");
  SDL_Delay(500);

  if (reset_display) {
    return m8_reset_display();
  }
  return 1;
}

int m8_process_data(const config_params_s *conf) {
  static unsigned int empty_cycles = 0;
  (void)conf;

  if (!ws_display) {
    return DEVICE_DISCONNECTED;
  }

  if (queue_size(&queue) > 0) {
    unsigned char *command;
    empty_cycles = 0;
    size_t length = 0;
    while ((command = pop_message(&queue, &length)) != NULL) {
      if (length > 0) {
        process_command(command, length);
      }
      SDL_free(command);
    }
  } else {
    empty_cycles++;
    if (empty_cycles >= conf->wait_packets) {
      // WebSocket is connection-oriented, no ping needed
      // If connection is lost, callbacks will handle it
      empty_cycles = 0;
    }
  }

  return DEVICE_PROCESSING;
}

int m8_close() { return disconnect(); }

int m8_pause_processing(void) { return 1; }
int m8_resume_processing(void) { return 1; }

#endif // USE_WEBSOCKET
