// Copyright 2026 M8 TCP Backend
// TCP backend for connecting to m8-display server
// Released under the MIT licence

#ifdef USE_TCP
#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../command.h"
#include "../config.h"
#include "audio.h"
#include "m8.h"
#include "queue.h"
#include "slip.h"

// Packet type headers (all packets: header + 2-byte BE length + data)
#define DISPLAY_HEADER 0x44  // 'D' - display/SLIP data
#define AUDIO_HEADER 0x41    // 'A' - audio PCM data
#define SLIP_END 0xC0        // SLIP frame delimiter (unused in framed protocol)

#define TCP_READ_SIZE 4096
#define TCP_READ_DELAY_MS 4
#define AUDIO_BUFFER_SIZE 65536

// TCP socket
static int tcp_socket = -1;
static int tcp_audio_initialized = 0;

// Buffers
static uint8_t tcp_buffer[TCP_READ_SIZE] = {0};
static uint8_t slip_buffer[TCP_READ_SIZE] = {0};
static slip_handler_s slip;
message_queue_s queue;

// Packet parsing state
static uint8_t audio_packet_buf[AUDIO_BUFFER_SIZE];
static uint8_t display_packet_buf[TCP_READ_SIZE * 4];
static int audio_packet_len = 0;
static int audio_packet_expected = 0;
static int display_packet_len = 0;
static int display_packet_expected = 0;
static int in_audio_packet = 0;
static int in_display_packet = 0;
static int at_packet_boundary = 1;  // Start at boundary

// Thread
SDL_Thread *tcp_thread = NULL;

typedef struct {
  int should_stop;
} thread_params_s;

static thread_params_s thread_params;

// Server address from config
static char server_host[256] = "127.0.0.1";
static int server_port = 3333;

static int send_message_to_queue(uint8_t *data, const uint32_t size) {
  push_message(&queue, data, size);
  return 1;
}

static int disconnect() {
  SDL_Log("Disconnecting TCP");

  thread_params.should_stop = 1;
  if (tcp_thread) {
    SDL_WaitThread(tcp_thread, NULL);
    tcp_thread = NULL;
  }
  destroy_queue(&queue);

  if (tcp_socket >= 0) {
    // Send disconnect command
    const unsigned char buf[1] = {'D'};
    send(tcp_socket, buf, 1, 0);
    close(tcp_socket);
    tcp_socket = -1;
  }

  // Close TCP audio
  if (tcp_audio_initialized) {
    audio_close();
    tcp_audio_initialized = 0;
  }

  return 1;
}

// Feed display data (SLIP) to the parser
static void process_display_data(const uint8_t *data, int len, slip_handler_s *slip) {
  for (int i = 0; i < len; i++) {
    const int slip_result = slip_read_byte(slip, data[i]);
    if (slip_result != SLIP_NO_ERROR) {
      SDL_LogError(SDL_LOG_CATEGORY_ERROR, "SLIP error %d", slip_result);
    }
  }
}

static int recv_debug_count = 0;
static int recv_audio_pkts = 0;
static int recv_display_pkts = 0;
static Uint64 recv_last_log = 0;

static void process_received_bytes(const uint8_t *buffer, int bytes_read,
                                   slip_handler_s *slip) {
  const uint8_t *cur = buffer;
  const uint8_t *end = buffer + bytes_read;

  // Debug first 10 receives
  if (recv_debug_count < 10) {
    SDL_Log("RECV[%d]: %d bytes, first 8: %02x %02x %02x %02x %02x %02x %02x %02x",
            recv_debug_count, bytes_read,
            bytes_read > 0 ? buffer[0] : 0,
            bytes_read > 1 ? buffer[1] : 0,
            bytes_read > 2 ? buffer[2] : 0,
            bytes_read > 3 ? buffer[3] : 0,
            bytes_read > 4 ? buffer[4] : 0,
            bytes_read > 5 ? buffer[5] : 0,
            bytes_read > 6 ? buffer[6] : 0,
            bytes_read > 7 ? buffer[7] : 0);
    recv_debug_count++;
  }

  while (cur < end) {
    // Continue accumulating audio packet
    if (in_audio_packet) {
      int remaining = audio_packet_expected - audio_packet_len;
      int available = end - cur;
      int to_copy = (remaining < available) ? remaining : available;

      memcpy(audio_packet_buf + audio_packet_len, cur, to_copy);
      audio_packet_len += to_copy;
      cur += to_copy;

      if (audio_packet_len >= audio_packet_expected) {
        recv_audio_pkts++;
        audio_feed_tcp(audio_packet_buf, audio_packet_len);
        in_audio_packet = 0;
        audio_packet_len = 0;
        audio_packet_expected = 0;
        at_packet_boundary = 1;
      }
      continue;
    }

    // Continue accumulating display packet
    if (in_display_packet) {
      int remaining = display_packet_expected - display_packet_len;
      int available = end - cur;
      int to_copy = (remaining < available) ? remaining : available;

      memcpy(display_packet_buf + display_packet_len, cur, to_copy);
      display_packet_len += to_copy;
      cur += to_copy;

      if (display_packet_len >= display_packet_expected) {
        recv_display_pkts++;
        process_display_data(display_packet_buf, display_packet_len, slip);
        in_display_packet = 0;
        display_packet_len = 0;
        display_packet_expected = 0;
        at_packet_boundary = 1;
      }
      continue;
    }

    // At packet boundary - check for packet header
    if (at_packet_boundary && (end - cur) >= 3) {
      uint8_t header = cur[0];
      int expected_len = (cur[1] << 8) | cur[2];

      // Check for valid packet types
      if ((header == AUDIO_HEADER || header == DISPLAY_HEADER) &&
          expected_len > 0 && expected_len <= AUDIO_BUFFER_SIZE) {
        int total_packet_size = 3 + expected_len;

        if ((end - cur) >= total_packet_size) {
          // Full packet available
          if (header == AUDIO_HEADER) {
            audio_feed_tcp(cur + 3, expected_len);
          } else {
            recv_display_pkts++;
            process_display_data(cur + 3, expected_len, slip);
          }
          cur += total_packet_size;
          at_packet_boundary = 1;
          continue;
        } else {
          // Partial packet - buffer it
          int header_and_data = end - cur - 3;
          if (header == AUDIO_HEADER) {
            audio_packet_expected = expected_len;
            in_audio_packet = 1;
            audio_packet_len = 0;
            if (header_and_data > 0) {
              memcpy(audio_packet_buf, cur + 3, header_and_data);
              audio_packet_len = header_and_data;
            }
          } else {
            display_packet_expected = expected_len;
            in_display_packet = 1;
            display_packet_len = 0;
            if (header_and_data > 0) {
              memcpy(display_packet_buf, cur + 3, header_and_data);
              display_packet_len = header_and_data;
            }
          }
          cur = end;
          continue;
        }
      }
    }

    // Invalid or unrecognized data - try to re-sync by finding next valid header
    // Scan for 'A' (0x41) or 'D' (0x44) followed by reasonable length
    const uint8_t *sync_start = cur;
    int found_sync = 0;
    while (cur < end - 2) {
      if ((cur[0] == AUDIO_HEADER || cur[0] == DISPLAY_HEADER)) {
        int len = (cur[1] << 8) | cur[2];
        // Reasonable length check: 1 to 64KB
        if (len > 0 && len <= 65536) {
          found_sync = 1;
          at_packet_boundary = 1;
          if (cur != sync_start) {
            SDL_Log("Re-synced after skipping %ld bytes", (long)(cur - sync_start));
          }
          break;
        }
      }
      cur++;
    }
    if (!found_sync) {
      // Skip remaining bytes, wait for next recv
      cur = end;
      at_packet_boundary = 1;  // Reset for next recv
    }
  }

  // Periodic summary log
  Uint64 now = SDL_GetTicks();
  if (now - recv_last_log > 5000) {
    SDL_Log("RECV summary: audio=%d pkts, display=%d pkts",
            recv_audio_pkts, recv_display_pkts);
    recv_audio_pkts = 0;
    recv_display_pkts = 0;
    recv_last_log = now;
  }
}

static int thread_process_tcp_data(void *data) {
  const thread_params_s *params = data;

  while (!params->should_stop) {
    if (tcp_socket < 0) {
      SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "TCP socket closed");
      return 0;
    }

    // Non-blocking read
    int bytes_read = recv(tcp_socket, tcp_buffer, TCP_READ_SIZE, MSG_DONTWAIT);

    if (bytes_read < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No data available, just delay
        SDL_Delay(TCP_READ_DELAY_MS);
        continue;
      }
      SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "TCP read error: %s",
                      strerror(errno));
      return 0;
    }

    if (bytes_read == 0) {
      SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "TCP connection closed by server");
      return 0;
    }

    if (bytes_read > 0) {
      process_received_bytes(tcp_buffer, bytes_read, &slip);
    }

    SDL_Delay(TCP_READ_DELAY_MS);
  }
  return 1;
}

// Resolve hostname to IP address
static int resolve_hostname(const char *hostname, char *ip_out, size_t ip_len) {
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int err = getaddrinfo(hostname, NULL, &hints, &res);
  if (err != 0) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "getaddrinfo failed: %s",
                 gai_strerror(err));
    return 0;
  }

  struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
  inet_ntop(AF_INET, &addr->sin_addr, ip_out, ip_len);
  freeaddrinfo(res);
  return 1;
}

// Parse server address from string (host:port or just host)
static void parse_server_address(const char *addr) {
  if (addr == NULL || strlen(addr) == 0) {
    return;
  }

  // Check for port separator
  const char *colon = strrchr(addr, ':');
  if (colon) {
    // Has port
    size_t host_len = colon - addr;
    if (host_len > 0 && host_len < sizeof(server_host)) {
      strncpy(server_host, addr, host_len);
      server_host[host_len] = '\0';
    }
    server_port = atoi(colon + 1);
    if (server_port <= 0 || server_port > 65535) {
      server_port = 3333;
    }
  } else {
    // Just hostname
    strncpy(server_host, addr, sizeof(server_host) - 1);
    server_host[sizeof(server_host) - 1] = '\0';
  }
}

static int connect_tcp() {
  char ip_addr[INET_ADDRSTRLEN];

  // Resolve hostname if needed
  if (!resolve_hostname(server_host, ip_addr, sizeof(ip_addr))) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Cannot resolve hostname: %s",
                 server_host);
    return 0;
  }

  SDL_Log("Connecting to %s:%d (resolved: %s)", server_host, server_port,
          ip_addr);

  // Create socket
  tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp_socket < 0) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Cannot create socket: %s",
                 strerror(errno));
    return 0;
  }

  // Set TCP_NODELAY for low latency
  int flag = 1;
  setsockopt(tcp_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

  // Connect
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(server_port);
  inet_pton(AF_INET, ip_addr, &server_addr.sin_addr);

  if (connect(tcp_socket, (struct sockaddr *)&server_addr,
              sizeof(server_addr)) < 0) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Cannot connect: %s",
                 strerror(errno));
    close(tcp_socket);
    tcp_socket = -1;
    return 0;
  }

  SDL_Log("Connected to TCP server %s:%d", server_host, server_port);
  return 1;
}

static int initialize_tcp_thread() {
  init_queue(&queue);
  thread_params.should_stop = 0;

  tcp_thread =
      SDL_CreateThread(thread_process_tcp_data, "TcpThread", &thread_params);

  if (!tcp_thread) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "SDL_CreateThread Error: %s",
                 SDL_GetError());
    return 0;
  }

  return 1;
}

int m8_initialize(const int verbose, const char *preferred_device) {
  if (tcp_socket >= 0) {
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
  // Format: host:port or just host (default port 3333)
  if (preferred_device != NULL) {
    parse_server_address(preferred_device);
  }

  if (verbose) {
    SDL_Log("Connecting to m8-display TCP server at %s:%d", server_host,
            server_port);
  }

  // Connect to TCP server
  if (!connect_tcp()) {
    if (verbose) {
      SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Cannot connect to TCP server");
    }
    return 0;
  }

  // Initialize TCP audio (output only, no M8 input device needed)
  if (!tcp_audio_initialized) {
    if (audio_init_tcp(NULL, 0)) {
      tcp_audio_initialized = 1;
      SDL_Log("TCP Audio streaming enabled");
    } else {
      SDL_Log("TCP Audio not available (will display only)");
    }
  }

  // Start processing thread
  return initialize_tcp_thread();
}

int m8_list_devices(void) {
  SDL_Log("TCP mode: Use --dev host:port to specify server address");
  SDL_Log("Example: --dev 192.168.1.100:3333");
  SDL_Log("Default: 127.0.0.1:3333");
  return 0;
}

static int send_ping() {
  if (tcp_socket < 0)
    return 0;

  const unsigned char buf[1] = {'X'};
  int result = send(tcp_socket, buf, 1, 0);
  if (result != 1) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending ping");
    return 0;
  }
  return 1;
}

int m8_send_msg_controller(const uint8_t input) {
  if (tcp_socket < 0)
    return -1;

  const unsigned char buf[2] = {'C', input};
  int result = send(tcp_socket, buf, 2, 0);
  if (result != 2) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending controller input");
    return -1;
  }
  return 1;
}

int m8_send_msg_keyjazz(const uint8_t note, uint8_t velocity) {
  if (tcp_socket < 0)
    return -1;

  if (velocity > 0x7F)
    velocity = 0x7F;

  // Note off
  if (note == 0xFF && velocity == 0x00) {
    const unsigned char buf[2] = {'K', 0xFF};
    int result = send(tcp_socket, buf, 2, 0);
    if (result != 2) {
      SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending keyjazz note off");
      return -1;
    }
    return 1;
  }

  // Note on
  const unsigned char buf[3] = {'K', note, velocity};
  int result = send(tcp_socket, buf, 3, 0);
  if (result != 3) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending keyjazz note on");
    return -1;
  }
  return 1;
}

int m8_reset_display() {
  if (tcp_socket < 0)
    return 0;

  SDL_Log("Reset display");
  const unsigned char buf[1] = {'R'};
  int result = send(tcp_socket, buf, 1, 0);
  if (result != 1) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error resetting display");
    return 0;
  }
  return 1;
}

int m8_enable_display(const unsigned char reset_display) {
  if (tcp_socket < 0)
    return 0;

  SDL_Log("Enabling M8 display");
  const unsigned char buf[1] = {'E'};
  int result = send(tcp_socket, buf, 1, 0);
  if (result != 1) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error enabling display");
    return 0;
  }

  SDL_Delay(500);

  if (reset_display) {
    return m8_reset_display();
  }
  return 1;
}

int m8_process_data(const config_params_s *conf) {
  static unsigned int empty_cycles = 0;

  if (tcp_socket < 0) {
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
      // Try ping
      if (!send_ping()) {
        SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "TCP connection lost");
        disconnect();
        return DEVICE_DISCONNECTED;
      }
      empty_cycles = 0;
    }
  }

  return DEVICE_PROCESSING;
}

int m8_close() { return disconnect(); }

int m8_pause_processing(void) { return 1; }
int m8_resume_processing(void) { return 1; }

#endif // USE_TCP
