#include "sink.h"

namespace news_rewriter {

SinkRegistry& SinkRegistry::instance() {
    static SinkRegistry registry;
    return registry;
}

void SinkRegistry::register_factory(const char* type, SinkFactory factory) {
    if (!type || !factory) return;
    factories_[type] = factory;
}

std::unique_ptr<Sink> SinkRegistry::create(const SinkConfig& cfg, Storage& storage,
                                           const LogFn& log) {
    const auto it = factories_.find(cfg.type);
    if (it == factories_.end()) return nullptr;
    return it->second(cfg, storage, log);
}

} // namespace news_rewriter
