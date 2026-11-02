#include "ob/order_book.hpp"

namespace ob {

// The implementation lives in the header (BasicOrderBook is a template over its
// callback type). This explicit instantiation compiles the std::function
// version once, so code using plain `OrderBook` does not recompile it.
template class BasicOrderBook<>;

}  // namespace ob
