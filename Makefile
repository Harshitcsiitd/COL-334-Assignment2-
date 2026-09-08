CXX      := c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic
BIN      := exchange_server trader_client market_data_client

all: $(BIN)

exchange_server: src/server.cpp src/book.cpp src/framing.hpp src/protocol.hpp src/book.hpp src/netutil.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/server.cpp src/book.cpp

trader_client: src/trader.cpp src/framing.hpp src/client_common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/trader.cpp

market_data_client: src/market_data.cpp src/framing.hpp src/client_common.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/market_data.cpp

clean:
	rm -f $(BIN)

.PHONY: all clean
