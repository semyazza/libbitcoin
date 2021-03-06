#include <bitcoin/storage/postgresql_storage.hpp>
#include <bitcoin/util/logger.hpp>
#include <bitcoin/messages.hpp>
#include <bitcoin/verify.hpp>
#include <bitcoin/types.hpp>
#include <bitcoin/dialect.hpp>
#include <iostream>

using std::shared_ptr;
using libbitcoin::verify_block;
typedef shared_ptr<verify_block> verify_block_ptr;
using libbitcoin::postgresql_storage;
using libbitcoin::original_dialect;
using libbitcoin::storage_ptr;
using libbitcoin::dialect_ptr;

using std::placeholders::_1;
using std::placeholders::_2;

void block_status(std::error_code ec, bool is_good)
{
    if (ec)
        libbitcoin::log_error() << "Verifying block: " << ec.message() << "\n";
    std::cout << "block is " << (is_good ? "good" : "bad") << "\n";
    exit(0);
}

void recv_block(std::error_code ec, libbitcoin::message::block block, verify_block_ptr verif)
{
    if (ec)
    {
        std::cerr << ec.message() << "\n";
        return;
    }
    verif->start(block, block_status);
}

int main()
{
    storage_ptr psql(new postgresql_storage("bitcoin", "genjix", ""));
    dialect_ptr dialect(new original_dialect);
    verify_block_ptr verif(new verify_block(psql, dialect));
    psql->fetch_block_by_depth(2, std::bind(recv_block, _1, _2, verif));
    std::cin.get();
    return 0;
}

