#include "../../include/core/llama_interface.h"
#include "../include/core/llama_interface_impl.h"

namespace llama_gui {
namespace core {

// ============================================================================
// LlamaInterface Public API
// ============================================================================

LlamaInterface::LlamaInterface(const std::string& server_url)
    : pImpl(std::make_unique<impl::LlamaInterfaceImpl>(server_url)) {
}

LlamaInterface::~LlamaInterface() = default;

bool LlamaInterface::initialize(const std::string& server_url) {
    return pImpl->initialize(server_url);
}

void LlamaInterface::create_chat_completion_streaming(
    const ChatCompletionRequest& request,
    StreamCallback callback) {
    pImpl->create_chat_completion_streaming(request, callback);
}

std::future<ChatCompletionResponse> LlamaInterface::create_chat_completion_async(
    const ChatCompletionRequest& request) {
    return pImpl->create_chat_completion_async(request);
}

EmbeddingResponse LlamaInterface::create_embedding(const EmbeddingRequest& request) {
    return pImpl->create_embedding(request);
}

bool LlamaInterface::is_server_healthy() const {
    return pImpl->is_server_healthy();
}

json LlamaInterface::get_server_info() const {
    return pImpl->get_server_info();
}

json LlamaInterface::get_models() const {
    return pImpl->get_models();
}

json LlamaInterface::get_slots_status() const {
    return pImpl->get_slots_status();
}

bool LlamaInterface::save_slot_kv_cache(int slot_id, const std::string& filename) {
    return pImpl->save_slot_kv_cache(slot_id, filename);
}

bool LlamaInterface::restore_slot_kv_cache(int slot_id, const std::string& filename) {
    return pImpl->restore_slot_kv_cache(slot_id, filename);
}

impl::SlotOperationResult LlamaInterface::save_slot_kv_cache_detailed(
    int slot_id, const std::string& filename) {
    return pImpl->save_slot_kv_cache_detailed(slot_id, filename);
}

impl::SlotOperationResult LlamaInterface::restore_slot_kv_cache_detailed(
    int slot_id, const std::string& filename) {
    return pImpl->restore_slot_kv_cache_detailed(slot_id, filename);
}

bool LlamaInterface::reset_slot(int slot_id) {
    return pImpl->reset_slot(slot_id);
}

bool LlamaInterface::erase_slot(int slot_id) {
    return pImpl->erase_slot(slot_id);
}

impl::SlotOperationResult LlamaInterface::tokenize_text_in_slot(
    int slot_id, const std::string& text) {
    return pImpl->tokenize_text_in_slot(slot_id, text);
}

void LlamaInterface::set_api_key(const std::string& api_key) {
    pImpl->set_api_key(api_key);
}

void LlamaInterface::set_ssl_verify(bool verify) {
    pImpl->set_ssl_verify(verify);
}

void LlamaInterface::set_timeout(int seconds) {
    pImpl->set_timeout(seconds);
}

bool LlamaInterface::parse_streaming_response(
    const std::string& response, StreamCallback callback) {
    return pImpl->parse_streaming_response(response, callback);
}

std::string LlamaInterface::make_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data) const {
    return pImpl->make_http_request(endpoint, method, data);
}

void LlamaInterface::make_async_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data,
    HttpResponseCallback callback) {
    pImpl->make_async_http_request(endpoint, method, data, callback);
}

void LlamaInterface::make_streaming_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data,
    StreamCallback callback) {
    pImpl->make_streaming_http_request(endpoint, method, data, callback);
}

void LlamaInterface::stop_streaming_requests() {
    pImpl->stop_streaming_requests();
}

void LlamaInterface::create_chat_completion_async_callback(
    const ChatCompletionRequest& request,
    ChatCompletionCallback callback) {
    pImpl->create_chat_completion_async_callback(request, callback);
}

std::string LlamaInterface::validate_and_clean_utf8(const std::string& input) {
    return llama_gui::core::impl::LlamaInterfaceImpl::validate_and_clean_utf8(input);
}

llama_gui::core::json LlamaInterface::extract_json_from_response(const std::string& response) {
    return llama_gui::core::impl::LlamaInterfaceImpl::extract_json_from_response(response);
}

void LlamaInterface::process_async_requests() {
    pImpl->process_async_requests();
}

} // namespace core
} // namespace llama_gui
