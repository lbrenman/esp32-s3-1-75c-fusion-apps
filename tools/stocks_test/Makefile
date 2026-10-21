# Host tests for the Stocks app's reply parsing and the MCP client's SSE/JSON-RPC handling.
#
#   cd tools/stocks_test
#   make          # first run clones cJSON (the same library ESP-IDF uses), then runs the tests

MAIN  := ../../main
CJSON := cJSON
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -DMCP_HOST_TEST -I$(CJSON) -I$(MAIN) -I$(MAIN)/apps

.PHONY: test clean
test: stocks_test
	./stocks_test

$(CJSON):
	git clone --depth 1 https://github.com/DaveGamble/cJSON.git $(CJSON)

stocks_test: test.c $(MAIN)/apps/stocks_parse.c $(MAIN)/apps/stocks_parse.h $(MAIN)/mcp_client.c $(MAIN)/mcp_client.h | $(CJSON)
	$(CC) $(CFLAGS) -o $@ test.c $(MAIN)/apps/stocks_parse.c $(MAIN)/mcp_client.c $(CJSON)/cJSON.c -lm

clean:
	rm -f stocks_test
