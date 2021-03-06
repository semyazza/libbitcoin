#ifndef LIBBITCOIN_THREADED_SERVICE_H
#define LIBBITCOIN_THREADED_SERVICE_H

#include <thread>

#include <bitcoin/types.hpp>

namespace libbitcoin {

class threaded_service
{
protected:
    threaded_service();
    ~threaded_service();
    service_ptr service();
    strand_ptr strand();
private:
    service_ptr service_;
    strand_ptr strand_;
    std::thread runner_;
    work_ptr work_;
};

} // libbitcoin

#endif

