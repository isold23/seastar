/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2021 ScyllaDB
 */

#include <seastar/websocket/server.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/http/request.hh>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

namespace seastar::experimental::websocket {

static sstring magic_key_suffix = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static sstring http_upgrade_reply_template =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Accept: ";

static logger wlogger("websocket");

opcodes websocket_parser::opcode() const {
    if (_header) {
        return opcodes(_header->opcode);
    } else {
        return opcodes::INVALID;
    }
}

websocket_parser::buff_t websocket_parser::result() {
    return std::move(_result);
}

void server::listen(socket_address addr, listen_options lo) {
    _listeners.push_back(seastar::listen(addr, lo));
    accept(_listeners.back());
}
void server::listen(socket_address addr) {
    listen_options lo;
    lo.reuse_address = true;
    return listen(addr, lo);
}

void server::accept(server_socket &listener) {
    (void)try_with_gate(_task_gate, [this, &listener]() {
        return repeat([this, &listener]() {
            return accept_one(listener);
        });
    }).handle_exception_type([](const gate_closed_exception &e) {});
}

future<stop_iteration> server::accept_one(server_socket &listener) {
    return listener.accept().then([this](accept_result ar) {
        auto conn = std::make_unique<connection>(*this, std::move(ar.connection));
        (void)try_with_gate(_task_gate, [conn = std::move(conn)]() mutable {
            return conn->process().finally([conn = std::move(conn)] {
                wlogger.debug("Connection is finished");
            });
        }).handle_exception_type([](const gate_closed_exception &e) {});
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }).handle_exception_type([](const std::system_error &e) {
        // We expect a ECONNABORTED when server::stop is called,
        // no point in warning about that.
        if (e.code().value() != ECONNABORTED) {
            wlogger.error("accept failed: {}", e);
        }
        return make_ready_future<stop_iteration>(stop_iteration::yes);
    }).handle_exception([](std::exception_ptr ex) {
        wlogger.info("accept failed: {}", ex);
        return make_ready_future<stop_iteration>(stop_iteration::yes);
    });
}

future<> server::stop() {
    for (auto&& l : _listeners) {
        l.abort_accept();
    }

    for (auto&& c : _connections) {
        c.shutdown_input();
    }

    return _task_gate.close().finally([this] {
        return parallel_for_each(_connections, [] (connection& conn) {
            return conn.close(true).handle_exception([] (auto ignored) {});
        });
    });
}

connection::~connection() {
    _server._connections.erase(_server._connections.iterator_to(*this));
}

void connection::on_new_connection() {
    _server._connections.push_back(*this);
}

future<> connection::process() {
    return when_all_succeed(read_loop(), response_loop()).discard_result().handle_exception([] (const std::exception_ptr& e) {
        wlogger.debug("Processing failed: {}", e);
    });
}

static std::string sha1_base64(std::string_view source) {
    unsigned char hash[20];
    assert(sizeof(hash) == gnutls_hash_get_len(GNUTLS_DIG_SHA1));
    if (int ret = gnutls_hash_fast(GNUTLS_DIG_SHA1, source.data(), source.size(), hash);
        ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_hash_fast: {}", gnutls_strerror(ret)));
    }
    gnutls_datum_t hash_data{
        .data = hash,
        .size = sizeof(hash),
    };
    gnutls_datum_t base64_encoded;
    if (int ret = gnutls_base64_encode2(&hash_data, &base64_encoded);
        ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_base64_encode2: {}", gnutls_strerror(ret)));
    }
    auto free_base64_encoded = defer([&] () noexcept { gnutls_free(base64_encoded.data); });
    // base64_encoded.data is "unsigned char *"
    return std::string(reinterpret_cast<const char*>(base64_encoded.data), base64_encoded.size);
}

future<> connection::read_http_upgrade_request() {
    _http_parser.init();
    co_await _read_buf.consume(_http_parser);

    if (_http_parser.eof()) {
        _done = true;
        co_return;
    }
    std::unique_ptr<http::request> req = _http_parser.get_parsed_request();
    if (_http_parser.failed()) {
        throw websocket::exception("Incorrect upgrade request");
    }

    sstring upgrade_header = req->get_header("Upgrade");
    if (upgrade_header != "websocket") {
        throw websocket::exception("Upgrade header missing");
    }

    sstring subprotocol = req->get_header("Sec-WebSocket-Protocol");

    if (!_server.is_handler_registered(subprotocol)) {
        throw websocket::exception("Subprotocol not supported.");
    }
    this->_handler = this->_server._handlers[subprotocol];
    this->_subprotocol = subprotocol;
    wlogger.debug("Sec-WebSocket-Protocol: {}", subprotocol);

    sstring sec_key = req->get_header("Sec-Websocket-Key");
    sstring sec_version = req->get_header("Sec-Websocket-Version");

    sstring sha1_input = sec_key + magic_key_suffix;

    wlogger.debug("Sec-Websocket-Key: {}, Sec-Websocket-Version: {}", sec_key, sec_version);

    std::string sha1_output = sha1_base64(sha1_input);
    wlogger.debug("SHA1 output: {} of size {}", sha1_output, sha1_output.size());

    co_await _write_buf.write(http_upgrade_reply_template);
    co_await _write_buf.write(sha1_output);
    if (!_subprotocol.empty()) {
        co_await _write_buf.write("\r\nSec-WebSocket-Protocol: ", 26);
        co_await _write_buf.write(_subprotocol);
    }
    co_await _write_buf.write("\r\n\r\n", 4);
    co_await _write_buf.flush();
}

future<websocket_parser::consumption_result_t> websocket_parser::operator()(
        temporary_buffer<char> data) {
    if (data.size() == 0) {
        // EOF
        _cstate = connection_state::closed;
        return websocket_parser::stop(std::move(data));
    }
    if (_state == parsing_state::flags_and_payload_data) {
        if (_buffer.length() + data.size() >= 2) {
            // _buffer.length() is less than 2 when entering this if body due to how
            // the rest of code is structured. The else branch will never increase
            // _buffer.length() to >=2 and other paths to this condition will always
            // have buffer cleared.
            assert(_buffer.length() < 2);

            size_t hlen = _buffer.length();
            _buffer.append(data.get(), 2 - hlen);
            data.trim_front(2 - hlen);
            _header = std::make_unique<frame_header>(_buffer.data());
            _buffer = {};

            // https://datatracker.ietf.org/doc/html/rfc6455#section-5.1
            // We must close the connection if data isn't masked.
            if ((!_header->masked) ||
                    // RSVX must be 0
                    (_header->rsv1 | _header->rsv2 | _header->rsv3) ||
                    // Opcode must be known.
                    (!_header->is_opcode_known())) {
                _cstate = connection_state::error;
                return websocket_parser::stop(std::move(data));
            }
            _state = parsing_state::payload_length_and_mask;
        } else {
            _buffer.append(data.get(), data.size());
            return websocket_parser::dont_stop();
        }
    }
    if (_state == parsing_state::payload_length_and_mask) {
        size_t const required_bytes = _header->get_rest_of_header_length();
        if (_buffer.length() + data.size() >= required_bytes) {
            if (_buffer.length() < required_bytes) {
                size_t hlen = _buffer.length();
                _buffer.append(data.get(), required_bytes - hlen);
                data.trim_front(required_bytes - hlen);
            }
            _payload_length = _header->length;
            char const *input = _buffer.data();
            if (_header->length == 126) {
                _payload_length = consume_be<uint16_t>(input);
            } else if (_header->length == 127) {
                _payload_length = consume_be<uint64_t>(input);
            }

            _masking_key = consume_be<uint32_t>(input);
            _buffer = {};
            _state = parsing_state::payload;
        } else {
            _buffer.append(data.get(), data.size());
            return websocket_parser::dont_stop();
        }
    }
    if (_state == parsing_state::payload) {
        if (data.size() < remaining_payload_length()) {
            // data has insufficient data to complete the frame - consume data.size() bytes
            if (_result.empty()) {
                _result = temporary_buffer<char>(remaining_payload_length());
                _consumed_payload_length = 0;
            }
            std::copy(data.begin(), data.end(), _result.get_write() + _consumed_payload_length);
            _consumed_payload_length += data.size();
            return websocket_parser::dont_stop();
        } else {
            // data has sufficient data to complete the frame - consume remaining_payload_length()
            auto consumed_bytes = remaining_payload_length();
            if (_result.empty()) {
                // Try to avoid memory copies in case when network packets contain one or more full
                // websocket frames.
                if (consumed_bytes == data.size()) {
                    _result = std::move(data);
                    data = temporary_buffer<char>(0);
                } else {
                    _result = data.share();
                    _result.trim(consumed_bytes);
                    data.trim_front(consumed_bytes);
                }
            } else {
                std::copy(data.begin(), data.begin() + consumed_bytes,
                          _result.get_write() + _consumed_payload_length);
                data.trim_front(consumed_bytes);
            }
            remove_mask(_result, _payload_length);
            _consumed_payload_length = 0;
            _state = parsing_state::flags_and_payload_data;
            return websocket_parser::stop(std::move(data));
        }
    }
    _cstate = connection_state::error;
    return websocket_parser::stop(std::move(data));
}

future<> connection::handle_ping() {
    // TODO
    return make_ready_future<>();
}

future<> connection::handle_pong() {
    // TODO
    return make_ready_future<>();
}


future<> connection::read_one() {
    return _read_buf.consume(_websocket_parser).then([this] () mutable {
        if (_websocket_parser.is_valid()) {
            // FIXME: implement error handling
            switch(_websocket_parser.opcode()) {
                // We do not distinguish between these 3 types.
                case opcodes::CONTINUATION:
                case opcodes::TEXT:
                case opcodes::BINARY:
                    return _input_buffer.push_eventually(_websocket_parser.result());
                case opcodes::CLOSE:
                    wlogger.debug("Received close frame.");
                    /*
                     * datatracker.ietf.org/doc/html/rfc6455#section-5.5.1
                     */
                    return close(true);
                case opcodes::PING:
                    wlogger.debug("Received ping frame.");
                    return handle_ping();
                case opcodes::PONG:
                    wlogger.debug("Received pong frame.");
                    return handle_pong();
                default:
                    // Invalid - do nothing.
                    ;
            }
        } else if (_websocket_parser.eof()) {
            return close(false);
        }
        wlogger.debug("Reading from socket has failed.");
        return close(true);
    });
}

future<> connection::read_loop() {
    return read_http_upgrade_request().then([this] {
        return when_all_succeed(
            _handler(_input, _output).handle_exception([this] (std::exception_ptr e) mutable {
                return _read_buf.close().then([e = std::move(e)] () mutable {
                    return make_exception_future<>(std::move(e));
                });
            }),
            do_until([this] {return _done;}, [this] {return read_one();})
        ).discard_result();
    }).finally([this] {
        return _read_buf.close();
    });
}

void connection::shutdown_input() {
    _fd.shutdown_input();
}

future<> connection::close(bool send_close) {
    return [this, send_close]() {
        if (send_close) {
            return send_data(opcodes::CLOSE, temporary_buffer<char>(0));
        } else {
            return make_ready_future<>();
        }
    }().finally([this] {
        _done = true;
        return when_all_succeed(_input.close(), _output.close()).discard_result().finally([this] {
            _fd.shutdown_output();
        });
    });
}

future<> connection::send_data(opcodes opcode, temporary_buffer<char>&& buff) {
    char header[10] = {'\x80', 0};
    size_t header_size = 2;

    header[0] += opcode;

    if ((126 <= buff.size()) && (buff.size() <= std::numeric_limits<uint16_t>::max())) {
        header[1] = 0x7E;
        write_be<uint16_t>(header + 2, buff.size());
        header_size += sizeof(uint16_t);
    } else if (std::numeric_limits<uint16_t>::max() < buff.size()) {
        header[1] = 0x7F;
        write_be<uint64_t>(header + 2, buff.size());
        header_size += sizeof(uint64_t);
    } else {
        header[1] = uint8_t(buff.size());
    }

    scattered_message<char> msg;
    msg.append(sstring(header, header_size));
    msg.append(std::move(buff));
    return _write_buf.write(std::move(msg)).then([this] {
        return _write_buf.flush();
    });
}

future<> connection::response_loop() {
    return do_until([this] {return _done;}, [this] {
        // FIXME: implement error handling
        return _output_buffer.pop_eventually().then([this] (
                temporary_buffer<char> buf) {
            return send_data(opcodes::BINARY, std::move(buf));
        });
    }).finally([this]() {
        return _write_buf.close();
    });
}

bool server::is_handler_registered(std::string const& name) {
    return _handlers.find(name) != _handlers.end();
}

void server::register_handler(const std::string& name, handler_t handler) {
    _handlers[name] = handler;
}

}
