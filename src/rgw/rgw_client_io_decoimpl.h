// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_DECOIMPL_H
#define CEPH_RGW_CLIENT_IO_DECOIMPL_H

#include <type_traits>

#include <boost/optional.hpp>

#include "rgw_common.h"
#include "rgw_client_io.h"

template <typename T>
class RGWRestfulIOAccountingEngine : public RGWDecoratedRestfulIO<T>,
                                     public RGWClientIOAccounter {
  bool enabled;
  uint64_t total_sent;
  uint64_t total_received;

public:
  template <typename U>
  RGWRestfulIOAccountingEngine(U&& decoratee)
    : RGWDecoratedRestfulIO<T>(std::move(decoratee)),
      enabled(false),
      total_sent(0),
      total_received(0) {
  }

  size_t send_status(const int status,
                     const char* const status_name) override {
    const auto sent = RGWDecoratedRestfulIO<T>::send_status(status, status_name);
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  size_t send_100_continue() override {
    const auto sent = RGWDecoratedRestfulIO<T>::send_100_continue();
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  size_t send_header(const boost::string_ref& name,
                     const boost::string_ref& value) override {
    const auto sent = RGWDecoratedRestfulIO<T>::send_header(name, value);
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  size_t send_content_length(const uint64_t len) override {
    const auto sent = RGWDecoratedRestfulIO<T>::send_content_length(len);
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  size_t send_chunked_transfer_encoding() override {
    const auto sent = RGWDecoratedRestfulIO<T>::send_chunked_transfer_encoding();
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  size_t complete_header() override {
    const auto sent = RGWDecoratedRestfulIO<T>::complete_header();
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  size_t recv_body(char* buf, size_t max) override {
    const auto received = RGWDecoratedRestfulIO<T>::recv_body(buf, max);
    if (enabled) {
      total_received += received;
    }
    return received;
  }

  size_t send_body(const char* const buf,
                   const size_t len) override {
    const auto sent = RGWDecoratedRestfulIO<T>::send_body(buf, len);
    if (enabled) {
      total_sent += sent;
    }
    return sent;
  }

  uint64_t get_bytes_sent() const override {
    return total_sent;
  }

  uint64_t get_bytes_received() const override {
    return total_received;
  }

  void set_account(bool enabled) override {
    this->enabled = enabled;
  }
};


/* Filter for in-memory buffering incoming data and calculating the content
 * length header if it isn't present. */
template <typename T>
class RGWRestfulIOBufferingEngine : public RGWDecoratedRestfulIO<T> {
  template<typename Td> friend class RGWDecoratedRestfulIO;
protected:
  ceph::bufferlist data;

  bool has_content_length;
  bool buffer_data;

public:
  template <typename U>
  RGWRestfulIOBufferingEngine(U&& decoratee)
    : RGWDecoratedRestfulIO<T>(std::move(decoratee)),
      has_content_length(false),
      buffer_data(false) {
  }

  size_t send_content_length(const uint64_t len) override;
  size_t send_chunked_transfer_encoding() override;
  size_t complete_header() override;
  size_t send_body(const char* buf, size_t len) override;
  int complete_request() override;
};

template <typename T>
size_t RGWRestfulIOBufferingEngine<T>::send_body(const char* const buf,
                                                 const size_t len)
{
  if (buffer_data) {
    data.append(buf, len);
    return 0;
  }

  return RGWDecoratedRestfulIO<T>::send_body(buf, len);
}

template <typename T>
size_t RGWRestfulIOBufferingEngine<T>::send_content_length(const uint64_t len)
{
  has_content_length = true;
  return RGWDecoratedRestfulIO<T>::send_content_length(len);
}

template <typename T>
size_t RGWRestfulIOBufferingEngine<T>::send_chunked_transfer_encoding()
{
  has_content_length = true;
  return RGWDecoratedRestfulIO<T>::send_chunked_transfer_encoding();
}

template <typename T>
size_t RGWRestfulIOBufferingEngine<T>::complete_header()
{
  if (! has_content_length) {
    /* We will dump everything in complete_request(). */
    buffer_data = true;
    return 0;
  }

  return RGWDecoratedRestfulIO<T>::complete_header();
}

template <typename T>
int RGWRestfulIOBufferingEngine<T>::complete_request()
{
  size_t sent = 0;

  if (! has_content_length) {
    sent += RGWDecoratedRestfulIO<T>::send_content_length(data.length());
    sent += RGWDecoratedRestfulIO<T>::complete_header();
  }

  if (buffer_data) {
    /* We are sending each buffer separately to avoid extra memory shuffling
     * that would occur on data.c_str() to provide a continuous memory area. */
    for (const auto& ptr : data.buffers()) {
      sent += RGWDecoratedRestfulIO<T>::send_body(ptr.c_str(),
                                                  ptr.length());
    }
    data.clear();
    buffer_data = false;
  }

  return sent + RGWDecoratedRestfulIO<T>::complete_request();
}

template <typename T> static inline
RGWRestfulIOBufferingEngine<T> rgw_restful_io_add_buffering(T&& t) {
  return RGWRestfulIOBufferingEngine<T>(std::move(t));
}


template <typename T>
class RGWRestfulIOChunkingEngine : public RGWDecoratedRestfulIO<T> {
  template<typename Td> friend class RGWDecoratedRestfulIO;
protected:
  bool has_content_length;
  bool chunking_enabled;

public:
  template <typename U>
  RGWRestfulIOChunkingEngine(U&& decoratee)
    : RGWDecoratedRestfulIO<T>(std::move(decoratee)),
      has_content_length(false),
      chunking_enabled(false) {
  }

  size_t send_content_length(const uint64_t len) override {
    has_content_length = true;
    return RGWDecoratedRestfulIO<T>::send_content_length(len);
  }

  size_t send_chunked_transfer_encoding() override {
    has_content_length = false;
    chunking_enabled = true;
    return RGWDecoratedRestfulIO<T>::send_header("Transfer-Encoding", "chunked");
  }

  size_t send_body(const char* buf,
                   const size_t len) override {
    if (! chunking_enabled) {
      return RGWDecoratedRestfulIO<T>::send_body(buf, len);
    } else {
      static constexpr char HEADER_END[] = "\r\n";
      char sizebuf[32];
      const auto slen = snprintf(sizebuf, sizeof(buf), "%" PRIx64 "\r\n", len);
      size_t sent = 0;

      sent += RGWDecoratedRestfulIO<T>::send_body(sizebuf, slen);
      sent += RGWDecoratedRestfulIO<T>::send_body(buf, len);
      sent += RGWDecoratedRestfulIO<T>::send_body(HEADER_END,
                                                  sizeof(HEADER_END) - 1);
      return sent;
    }
  }

  int complete_request() override {
    size_t sent = 0;

    if (chunking_enabled) {
      static constexpr char CHUNKED_RESP_END[] = "0\r\n\r\n";
      sent += RGWDecoratedRestfulIO<T>::send_body(CHUNKED_RESP_END,
                                                  sizeof(CHUNKED_RESP_END) - 1);
    }

    return sent + RGWDecoratedRestfulIO<T>::complete_request();
  }
};

template <typename T> static inline
RGWRestfulIOChunkingEngine<T> rgw_restful_io_add_chunking(T&& t) {
  return RGWRestfulIOChunkingEngine<T>(std::move(t));
}


/* Class that controls and inhibits the process of sending Content-Length HTTP
 * header where RFC 7230 requests so. The cases worth our attention are 204 No
 * Content as well as 304 Not Modified. */
template <typename T>
class RGWRestfulIOConLenControllingEngine : public RGWDecoratedRestfulIO<T> {
protected:
  enum class ContentLengthAction {
    FORWARD,
    INHIBIT,
    UNKNOWN
  } action;

public:
  template <typename U>
  RGWRestfulIOConLenControllingEngine(U&& decoratee)
    : RGWDecoratedRestfulIO<T>(std::move(decoratee)),
      action(ContentLengthAction::UNKNOWN) {
  }

  size_t send_status(const int status,
                     const char* const status_name) override {
    if (204 == status || 304 == status) {
      action = ContentLengthAction::INHIBIT;
    } else {
      action = ContentLengthAction::FORWARD;
    }

    return RGWDecoratedRestfulIO<T>::send_status(status, status_name);
  }

  size_t send_content_length(const uint64_t len) override {
    switch(action) {
    case ContentLengthAction::FORWARD:
      return RGWDecoratedRestfulIO<T>::send_content_length(len);
    case ContentLengthAction::INHIBIT:
      return 0;
    case ContentLengthAction::UNKNOWN:
    default:
      return -EINVAL;
    }
  }
};

template <typename T> static inline
RGWRestfulIOConLenControllingEngine<T> rgw_restful_io_add_conlen_controlling(T&& t) {
  return RGWRestfulIOConLenControllingEngine<T>(std::move(t));
}


/* Filter that rectifies the wrong behaviour of some clients of the RGWRestfulIO
 * interface. Should be removed after fixing those clients. */
template <typename T>
class RGWRestfulIOReorderingEngine : public RGWDecoratedRestfulIO<T> {
protected:
  enum class ReorderState {
    RGW_EARLY_HEADERS,  /* Got headers sent before calling send_status. */
    RGW_STATUS_SEEN,    /* Status has been seen. */
    RGW_DATA            /* Header has been completed. */
  } phase;

  boost::optional<uint64_t> content_length;

  std::vector<std::pair<std::string, std::string>> headers;

  size_t send_header(const boost::string_ref& name,
                     const boost::string_ref& value) override {
    switch (phase) {
    case ReorderState::RGW_EARLY_HEADERS:
    case ReorderState::RGW_STATUS_SEEN:
      headers.emplace_back(std::make_pair(name.to_string(),
                                          value.to_string()));
      return 0;
    case ReorderState::RGW_DATA:
      return RGWDecoratedRestfulIO<T>::send_header(name, value);
    }

    return -EIO;
  }

public:
  template <typename U>
  RGWRestfulIOReorderingEngine(U&& decoratee)
    : RGWDecoratedRestfulIO<T>(std::move(decoratee)),
      phase(ReorderState::RGW_EARLY_HEADERS) {
  }

  size_t send_status(const int status,
                     const char* const status_name) override {
    phase = ReorderState::RGW_STATUS_SEEN;

    return RGWDecoratedRestfulIO<T>::send_status(status, status_name);
  }

  size_t send_content_length(const uint64_t len) override {
    if (ReorderState::RGW_EARLY_HEADERS == phase) {
      /* Oh great, someone tries to send content length before status. */
      content_length = len;
      return 0;
    } else {
      return RGWDecoratedRestfulIO<T>::send_content_length(len);
    }
  }

  size_t complete_header() override {
    size_t sent = 0;

    /* Change state in order to immediately send everything we get. */
    phase = ReorderState::RGW_DATA;

    /* Sent content length if necessary. */
    if (content_length) {
      sent += RGWDecoratedRestfulIO<T>::send_content_length(*content_length);
    }

    /* Header data in buffers are already counted. */
    for (const auto& kv : headers) {
      sent += RGWDecoratedRestfulIO<T>::send_header(kv.first, kv.second);
    }
    headers.clear();

    return sent + RGWDecoratedRestfulIO<T>::complete_header();
  }
};

template <typename T> static inline
RGWRestfulIOReorderingEngine<T> rgw_restful_io_add_reordering(T&& t) {
  return RGWRestfulIOReorderingEngine<T>(std::move(t));
}

#endif /* CEPH_RGW_CLIENT_IO_DECOIMPL_H */
