// Copyright 2017 The Brave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/io_buffer.h"
#include "net/socket/socks_auth_username_password.h"

namespace net {

SOCKSAuthUsernamePassword::SOCKSAuthUsernamePassword(
      const std::string& username, const std::string& password)
    : username_(username), password_(password) {
}

SOCKSAuthUsernamePassword::~SOCKSAuthUsernamePassword() = default;

SOCKSAuthState* SOCKSAuthUsernamePassword::Initialize() const {
  return new SOCKSAuthUsernamePassword::State(this);
}

SOCKSAuthUsernamePassword::State::State(const SOCKSAuthUsernamePassword* auth)
    : auth_(auth),
      next_state_(STATE_INIT_WRITE) {
}

SOCKSAuthUsernamePassword::State::~State() = default;

uint8_t SOCKSAuthUsernamePassword::State::method_number() {
  return 0x02;
}

static const size_t kSOCKSAuthUsernamePasswordResponseLen = 2;

int SOCKSAuthUsernamePassword::State::Do(
    int rv, ClientSocketHandle& transport, CompletionCallback& callback) {
  const NetLogWithSource& net_log = transport.socket()->NetLog();
  do {
    switch (next_state_) {
      case STATE_INIT_WRITE: {
        DCHECK_EQ(OK, rv);
        // Initialize the buffer with 
        //        0x01, usernamelen, username, passwordlen, password
        size_t usernamelen = auth_->username_.size();
        size_t passwordlen = auth_->password_.size();
        buffer_ = std::string(1 + 1 + usernamelen + 1 + passwordlen, 0);
        buffer_[0] = 0x01;
        buffer_[1] = usernamelen;
        buffer_.replace(2, usernamelen, auth_->username_);
        buffer_[2 + usernamelen] = passwordlen;
        buffer_.replace(2 + usernamelen + 1, passwordlen, auth_->password_);
        DCHECK_EQ(buffer_.size(), 2 + usernamelen + 1 + passwordlen);
        buffer_left_ = buffer_.size();
        next_state_ = STATE_WRITE;
        rv = OK;
        break;
      }
      case STATE_WRITE:
        DCHECK_EQ(OK, rv);
        DCHECK_LT(0, buffer_left_);
        iobuf_ = new IOBuffer(buffer_left_);
        memcpy(iobuf_->data(),
               &buffer_.data()[buffer_.size() - buffer_left_],
               buffer_left_);
        next_state_ = STATE_WRITE_COMPLETE;
        net_log.BeginEvent(NetLogEventType::SOCKS5_AUTH_WRITE);
        rv = transport.socket()->Write(iobuf_.get(), buffer_left_, callback);
        break;

      case STATE_WRITE_COMPLETE:
        // TODO(riastradh): Zero iobuf?  Zero buffer?
        net_log.EndEventWithNetErrorCode(NetLogEventType::SOCKS5_AUTH_WRITE,
                                         std::max(rv, 0));
        if (rv < 0) {
          next_state_ = STATE_BAD;
          return rv;
        }
        DCHECK_LE(static_cast<size_t>(rv), buffer_left_);
        buffer_left_ -= rv;
        next_state_ = (buffer_left_ == 0 ? STATE_INIT_READ : STATE_WRITE);
        rv = OK;
        break;

      case STATE_INIT_READ:
        DCHECK_EQ(OK, rv);
        buffer_.clear();
        buffer_left_ = kSOCKSAuthUsernamePasswordResponseLen;
        iobuf_ = new IOBuffer(buffer_left_);
        next_state_ = STATE_READ;
        rv = OK;
        break;

      case STATE_READ:
        DCHECK_EQ(OK, rv);
        iobuf_ = new IOBuffer(buffer_left_);
        next_state_ = STATE_READ_COMPLETE;
        net_log.BeginEvent(NetLogEventType::SOCKS5_AUTH_READ);
        rv = transport.socket()->Read(iobuf_.get(), buffer_left_, callback);
        break;

      case STATE_READ_COMPLETE:
        net_log.EndEventWithNetErrorCode(NetLogEventType::SOCKS5_AUTH_READ,
                                         std::max(rv, 0));
        if (rv < 0) {
          next_state_ = STATE_BAD;
          return rv;
        }
        DCHECK_LE(static_cast<size_t>(rv), buffer_left_);
        buffer_.append(iobuf_->data(), rv);
        buffer_left_ -= rv;
        next_state_ = (buffer_left_ == 0 ? STATE_DONE : STATE_READ);
        rv = OK;
        break;

      case STATE_DONE: {
        DCHECK_EQ(OK, rv);
        DCHECK_EQ(buffer_.size(), kSOCKSAuthUsernamePasswordResponseLen);
        static_assert(kSOCKSAuthUsernamePasswordResponseLen == 2, "bad size");
        uint8_t ver = buffer_[0];
        uint8_t status = buffer_[1];
        next_state_ = STATE_BAD;  // Caller had better stop here.
        if (ver != 0x01 || status != 0x00)
          return ERR_FAILED;
        return OK;
      }

      case STATE_BAD:
      default:
        NOTREACHED() << "bad state";
        return ERR_UNEXPECTED;
    }
  } while (rv != ERR_IO_PENDING);
  return rv;
}

}  // namespace net
