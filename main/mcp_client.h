#pragma once
/* A small MCP client for "Streamable HTTP" servers (MCP over HTTPS POST, JSON-RPC 2.0).
 *
 *   initialize -> notifications/initialized -> tools/call ...
 *
 * The server's Mcp-Session-Id is kept and sent with every later request, and the TLS connection
 * is kept open between calls (keep-alive), so a call after the first one is a single round trip.
 * Replies may be plain JSON or Server-Sent Events ("data: {...}"); both are handled.
 *
 * Blocking: call it from a worker task, never from the LVGL task. One client per task.
 * Nothing secret is logged: not the API key, not the URL's query string. */

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

typedef enum {
    MCP_OK = 0,
    MCP_ERR_CONFIG,     /* no usable URL */
    MCP_ERR_NET,        /* couldn't connect, timeout, connection dropped */
    MCP_ERR_HTTP,       /* the server answered with an HTTP error (see mcp_last_http_status) */
    MCP_ERR_PROTOCOL,   /* the reply wasn't the JSON-RPC message we expected */
    MCP_ERR_RPC,        /* JSON-RPC "error" reply (e.g. unknown tool) */
    MCP_ERR_TOOL,       /* the tool ran and reported isError: true */
} mcp_status_t;

typedef struct mcp_client mcp_client_t;

mcp_client_t *mcp_client_create(const char *client_name, const char *client_version);

/* Sets the server. Closes the connection and forgets the session if anything changed.
 * key_header is the HTTP header that carries the API key, e.g. "x-api-key". */
void mcp_client_configure(mcp_client_t *c, const char *url, const char *key_header, const char *api_key);

/* Closes the TLS connection and forgets the session (frees the TLS buffers). */
void mcp_client_close(mcp_client_t *c);

/* Calls a tool. args may be NULL ({}). On MCP_OK (and on MCP_ERR_TOOL) *text_out gets the
 * reply's text content, malloc'd: the caller frees it. On failure err gets a short message
 * that is fine to show on screen.
 * Network errors, server errors (5xx) and an expired session are retried once, with a new
 * connection and, if needed, a new session. Replies from the server (RPC or tool errors) are not. */
mcp_status_t mcp_call_tool(mcp_client_t *c, const char *tool, const cJSON *args,
                           char **text_out, char *err, size_t err_len);

int mcp_last_http_status(const mcp_client_t *c);

/* ---- reply parsing (no network; also used by the host tests) ---- */

/* Finds the JSON-RPC message with this id in a reply body (plain JSON, a JSON array, or an
 * SSE stream). Returns a new cJSON object (caller deletes) or NULL. */
cJSON *mcp_find_reply(const char *body, int id);

/* Joins a tools/call result's text content items with "\n" (malloc'd). Falls back to
 * structuredContent printed as JSON. *is_error gets the result's isError flag. */
char *mcp_result_text(const cJSON *result, bool *is_error);
