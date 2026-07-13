if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/gui/ai/OpenAIProvider.h" OPENAI)
file(READ "${SOURCE_ROOT}/gui/ai/AgentController.cpp" CONTROLLER)
file(READ "${SOURCE_ROOT}/gui/ai/ChatWindow.cpp" CHAT_WINDOW)
file(READ "${SOURCE_ROOT}/gui/ai/ChatWindowSettings.cpp" SETTINGS)
file(READ "${SOURCE_ROOT}/gui/ai/ApiKeyStore.cpp" KEY_STORE)

string(FIND "${OPENAI}" "https://api.openai.com/v1" OFFICIAL_POS)
string(FIND "${OPENAI}" "ai.ikik.net" THIRD_PARTY_POS)
if(OFFICIAL_POS EQUAL -1 OR NOT THIRD_PARTY_POS EQUAL -1)
    message(FATAL_ERROR "OpenAI must default to its official API endpoint")
endif()

foreach(SOURCE_NAME IN ITEMS CONTROLLER CHAT_WINDOW)
    string(FIND "${${SOURCE_NAME}}" "isProviderEndpointTrusted" TRUST_POS)
    if(TRUST_POS EQUAL -1)
        message(FATAL_ERROR "${SOURCE_NAME} bypasses endpoint trust validation")
    endif()
endforeach()

string(FIND "${SETTINGS}" "Trust this endpoint" TRUST_UI_POS)
string(FIND "${SETTINGS}" "canonicalEndpointForTrust" TRUST_BINDING_POS)
string(FIND "${KEY_STORE}" "trustedBaseUrl" TRUST_STORE_POS)
if(TRUST_UI_POS EQUAL -1 OR TRUST_BINDING_POS EQUAL -1 OR
   TRUST_STORE_POS EQUAL -1)
    message(FATAL_ERROR
        "custom endpoint trust is not explicitly bound and persisted")
endif()
