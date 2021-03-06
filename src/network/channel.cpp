#include "channel.hpp"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <functional>
#include <iterator>
#include <ctime>

#include <bitcoin/util/logger.hpp>
#include <bitcoin/util/assert.hpp>
#include <bitcoin/network/network.hpp>
#include <bitcoin/dialect.hpp>
#include <bitcoin/messages.hpp>

#include "shared_const_buffer.hpp"

namespace libbitcoin {

using std::placeholders::_1;
using std::placeholders::_2;
using boost::posix_time::seconds;
using boost::posix_time::minutes;
using boost::posix_time::time_duration;
using boost::asio::buffer;

// Connection timeout time
const time_duration disconnect_timeout = seconds(0) + minutes(90);

channel_handle channel_pimpl::chan_id_counter = 0;

channel_pimpl::channel_pimpl(const init_data& dat)
 : socket_(dat.socket), network_(dat.parent_gateway),
        translator_(dat.translator)
{
    // Unique IDs are assigned to channels by incrementing a shared counter
    // among instances.
    channel_id_ = chan_id_counter;
    chan_id_counter++;

    timeout_.reset(new deadline_timer(*dat.service));
    read_header();
    reset_timeout();

    send(create_version_message());
}

channel_pimpl::~channel_pimpl()
{
    tcp::endpoint remote_endpoint = socket_->remote_endpoint();
    log_debug() << "Closing channel "
            << remote_endpoint.address().to_string();
    boost::system::error_code ec;
    socket_->shutdown(tcp::socket::shutdown_both, ec);
    socket_->close(ec);
    timeout_->cancel();
}

void channel_pimpl::handle_timeout(const boost::system::error_code& ec)
{
    if (problems_check(ec))
        return;
    log_info() << "Forcing disconnect due to timeout.";
    // No response for a while so disconnect
    destroy_self();
}

void channel_pimpl::reset_timeout()
{
    timeout_->cancel();
    timeout_->expires_from_now(disconnect_timeout);
    timeout_->async_wait(std::bind(
            &channel_pimpl::handle_timeout, this, _1));
}

void channel_pimpl::destroy_self()
{
    network_->disconnect(channel_id_);
}

bool channel_pimpl::problems_check(const boost::system::error_code& ec)
{
    if (ec == boost::asio::error::operation_aborted)
    {
        // Do nothing
        return true;
    }
    else if (ec)
    {
        destroy_self();
        return true;
    }
    return false;
}

void channel_pimpl::read_header()
{
    auto callback = std::bind(
            &channel_pimpl::handle_read_header, this, _1, _2);
    async_read(*socket_, buffer(inbound_header_), callback);
}

void channel_pimpl::read_checksum(const message::header& header_msg)
{
    auto callback = std::bind(&channel_pimpl::handle_read_checksum, 
            this, header_msg, _1, _2);
    async_read(*socket_, buffer(inbound_checksum_), callback);
}

void channel_pimpl::read_payload(const message::header& header_msg)
{
    auto callback = std::bind(&channel_pimpl::handle_read_payload, 
            this, header_msg, _1, _2);
    inbound_payload_.resize(header_msg.payload_length);
    async_read(*socket_, buffer(inbound_payload_, header_msg.payload_length),
            callback);
}

void channel_pimpl::handle_read_header(const boost::system::error_code& ec,
        size_t bytes_transferred)
{
    if (problems_check(ec))
        return;
    BITCOIN_ASSERT(bytes_transferred == header_chunk_size);
    data_chunk header_stream =
            data_chunk(inbound_header_.begin(), inbound_header_.end());
    BITCOIN_ASSERT(header_stream.size() == header_chunk_size);
    message::header header_msg =
            translator_->header_from_network(header_stream);

    if (!translator_->verify_header(header_msg))
    {
        log_debug() << "Bad header received.";
        destroy_self();
        return;
    }

    log_info() << "r: " << header_msg.command
            << " (" << header_msg.payload_length << " bytes)";
    if (translator_->checksum_used(header_msg))
    {
        // Read checksum
        read_checksum(header_msg);
    }
    else
    {
        // Read payload
        read_payload(header_msg);
    }
    reset_timeout();
}

void channel_pimpl::handle_read_checksum(message::header& header_msg,
        const boost::system::error_code& ec, size_t bytes_transferred)
{
    if (problems_check(ec))
        return;
    BITCOIN_ASSERT(bytes_transferred == header_checksum_size);
    data_chunk checksum_stream = data_chunk(
            inbound_checksum_.begin(), inbound_checksum_.end());
    BITCOIN_ASSERT(checksum_stream.size() == header_checksum_size);
    //header_msg.checksum = cast_stream<uint32_t>(checksum_stream);
    header_msg.checksum = translator_->checksum_from_network(checksum_stream);
    read_payload(header_msg);
    reset_timeout();
}

void channel_pimpl::handle_read_payload(const message::header& header_msg,
        const boost::system::error_code& ec, size_t bytes_transferred)
{
    if (problems_check(ec))
        return;
    BITCOIN_ASSERT(bytes_transferred == header_msg.payload_length);
    data_chunk payload_stream = data_chunk(
            inbound_payload_.begin(), inbound_payload_.end());
    BITCOIN_ASSERT(payload_stream.size() == header_msg.payload_length);
    if (!translator_->verify_checksum(header_msg, payload_stream))
    {
        log_warning() << "Bad checksum!";
        destroy_self();
        return;
    }
    bool ret_errc = false;
    if (header_msg.command == "version")
    {
        message::version payload =
                translator_->version_from_network(
                    header_msg, payload_stream, ret_errc);
        if (!transport_payload(payload, ret_errc))
            return;
    }
    else if (header_msg.command == "verack")
    {
        message::verack payload;
        if (!transport_payload(payload, ret_errc))
            return;
    }
    else if (header_msg.command == "addr")
    {
        message::addr payload =
                translator_->addr_from_network(
                    header_msg, payload_stream, ret_errc);
        if (!transport_payload(payload, ret_errc))
            return;
    }
    else if (header_msg.command == "inv")
    {
        message::inv payload =
                translator_->inv_from_network(
                    header_msg, payload_stream, ret_errc);
        if (!transport_payload(payload, ret_errc))
            return;
    }
    else if (header_msg.command == "block")
    {
        message::block payload =
                translator_->block_from_network(
                    header_msg, payload_stream, ret_errc);
        if (!transport_payload(payload, ret_errc))
            return;
    }
    read_header();
    reset_timeout();
}

void channel_pimpl::handle_send(const boost::system::error_code& ec)
{
    if (problems_check(ec))
        return;
}

template<typename T>
void generic_send(const T& message_packet, channel_pimpl* chan_self,
        socket_ptr socket, dialect_ptr translator)
{
    data_chunk msg = translator->to_network(message_packet);
    shared_const_buffer buffer(msg);
    async_write(*socket, buffer, std::bind(
            &channel_pimpl::handle_send, chan_self, std::placeholders::_1));
}

void channel_pimpl::send(const message::version& version)
{
    generic_send(version, this, socket_, translator_);
}

void channel_pimpl::send(const message::verack& verack)
{
    generic_send(verack, this, socket_, translator_);
}

void channel_pimpl::send(const message::getaddr& getaddr)
{
    generic_send(getaddr, this, socket_, translator_);
}

void channel_pimpl::send(const message::getdata& getdata)
{
    generic_send(getdata, this, socket_, translator_);
}

void channel_pimpl::send(const message::getblocks& getblocks)
{
    generic_send(getblocks, this, socket_, translator_);
}

channel_handle channel_pimpl::get_id() const
{
    return channel_id_;
}

message::version channel_pimpl::create_version_message()
{
    message::version version;
    // this is test data.
    version.version = 31900;
    version.services = 1;
    version.timestamp = time(NULL);
    version.addr_me.services = version.services;
    version.addr_me.ip_addr = network_->get_ip_address();
    version.addr_me.port = 8333;
    version.addr_you.services = version.services;
    version.addr_you.ip_addr = decltype(version.addr_you.ip_addr){0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 0, 1};
    version.addr_you.port = 8333;
    version.nonce = 0xdeadbeef;
    version.sub_version_num = "";
    version.start_height = 0;
    return version;
}

} // libbitcoin

