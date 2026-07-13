if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/gui/ai/AgentController.cpp" CONTROLLER)
file(READ "${SOURCE_ROOT}/gui/ai/ChatWindow.cpp" CHAT_WINDOW)
file(READ "${SOURCE_ROOT}/gui/ai/OpenAIProvider.cpp" OPENAI)
file(READ "${SOURCE_ROOT}/gui/ai/ClaudeProvider.cpp" CLAUDE)
file(READ "${SOURCE_ROOT}/gui/ai/DeepSeekProvider.cpp" DEEPSEEK)
file(READ "${SOURCE_ROOT}/gui/ai/ChatWindowSettings.cpp" SETTINGS)
file(READ "${SOURCE_ROOT}/gui/ai/ApiKeyStore.cpp" KEY_STORE)

string(FIND "${CONTROLLER}" "prepareContextBudget(" BUDGET_POS)
string(FIND "${CONTROLLER}" "provider->sendCompletion(" SEND_POS)
if(BUDGET_POS EQUAL -1 OR SEND_POS EQUAL -1 OR BUDGET_POS GREATER SEND_POS)
    message(FATAL_ERROR
        "AgentController must prepare the context budget before provider dispatch")
endif()

foreach(REQUIRED IN ITEMS
        "completion.messages = std::move(budget.messages)"
        "completion.maxOutputTokens = budget.outputTokenReserve"
        "cfg.contextWindowTokens"
        "capabilities.maxContextTokens"
        "capabilities.maxOutputTokens")
    string(FIND "${CONTROLLER}" "${REQUIRED}" FOUND)
    if(FOUND EQUAL -1)
        message(FATAL_ERROR "AgentController budget integration missing: ${REQUIRED}")
    endif()
endforeach()

string(FIND "${CONTROLLER}" "completion.messages = request.messages" BYPASS_POS)
if(NOT BYPASS_POS EQUAL -1)
    message(FATAL_ERROR "AgentController still bypasses provider-aware trimming")
endif()

string(FIND "${CHAT_WINDOW}"
       "request.userTokenLimit = session_.getTokenLimit()" USER_LIMIT_POS)
if(USER_LIMIT_POS EQUAL -1)
    message(FATAL_ERROR "ChatWindow does not pass the global user token limit")
endif()

string(FIND "${OPENAI}" "max_completion_tokens" OPENAI_OUTPUT_POS)
string(FIND "${CLAUDE}" "max_tokens" CLAUDE_OUTPUT_POS)
string(FIND "${DEEPSEEK}" "max_tokens" DEEPSEEK_OUTPUT_POS)
if(OPENAI_OUTPUT_POS EQUAL -1 OR CLAUDE_OUTPUT_POS EQUAL -1 OR
   DEEPSEEK_OUTPUT_POS EQUAL -1)
    message(FATAL_ERROR "one or more providers omit the output-token bound")
endif()

string(FIND "${SETTINGS}" "Context window:" CONTEXT_UI_POS)
string(FIND "${KEY_STORE}" "contextWindowTokens" CONTEXT_STORE_POS)
if(CONTEXT_UI_POS EQUAL -1 OR CONTEXT_STORE_POS EQUAL -1)
    message(FATAL_ERROR "custom endpoint/model context override is not persisted")
endif()
