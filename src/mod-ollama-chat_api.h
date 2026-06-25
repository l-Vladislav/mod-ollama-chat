#ifndef MOD_OLLAMA_CHAT_API_H
#define MOD_OLLAMA_CHAT_API_H

#include <string>
#include <future>
#include "mod-ollama-chat_querymanager.h"

// modelOverride:      if non-empty, overrides g_OllamaModel for this call.
// rawResponse:        if true, skips ExtractTextBetweenDoubleQuotes (needed for multi-sentence summaries).
// thinkMode:          -1 = use global g_ThinkModeEnableForModule, 0 = force off, 1 = force on.
// numPredictOverride: -1 = use g_OllamaNumPredict, 0 = unlimited (omit field), >0 = use this value.
std::string QueryOllamaAPI(const std::string& prompt,
                           const std::string& modelOverride = "",
                           bool rawResponse = false,
                           int thinkMode = -1,
                           int numPredictOverride = -1);

// Checks if an API response is valid (not an error message)
bool IsValidAPIResponse(const std::string& response);

// Submits a query to the API.
std::future<std::string> SubmitQuery(const std::string& prompt);

// Declare the global QueryManager variable.
extern QueryManager g_queryManager;

#endif // MOD_OLLAMA_CHAT_API_H
